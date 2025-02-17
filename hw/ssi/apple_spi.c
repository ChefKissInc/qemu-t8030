#include "qemu/osdep.h"
#include "hw/arm/apple-silicon/dtb.h"
#include "hw/dma/apple_sio.h"
#include "hw/irq.h"
#include "hw/ssi/apple_spi.h"
#include "hw/ssi/ssi.h"
#include "migration/vmstate.h"
#include "qemu/error-report.h"
#include "qemu/fifo32.h"
#include "qemu/log.h"
#include "qemu/module.h"

/* XXX: Based on linux/drivers/spi/spi-apple.c */

#define REG_CTRL 0x000
#define REG_CTRL_RUN (1 << 0)
#define REG_CTRL_TX_RESET (1 << 2)
#define REG_CTRL_RX_RESET (1 << 3)

#define REG_CFG 0x004
#define REG_CFG_AGD (1 << 0)
#define REG_CFG_CPHA (1 << 1)
#define REG_CFG_CPOL (1 << 2)
#define REG_CFG_MODE(_x) (((_x) >> 5) & 0x3)
#define REG_CFG_MODE_INVALID 0
#define REG_CFG_MODE_IRQ 1
#define REG_CFG_MODE_DMA 2
#define REG_CFG_IE_RXREADY (1 << 7)
#define REG_CFG_IE_TXEMPTY (1 << 8)
#define REG_CFG_LSB_FIRST (1 << 13)
#define REG_CFG_WORD_SIZE(_x) (((_x) >> 15) & 0x3)
#define REG_CFG_WORD_SIZE_8B 0
#define REG_CFG_WORD_SIZE_16B 1
#define REG_CFG_WORD_SIZE_32B 2
#define REG_CFG_IE_COMPLETE (1 << 21)

#define REG_STATUS 0x008
#define REG_STATUS_RXREADY (1 << 0)
#define REG_STATUS_TXEMPTY (1 << 1)
#define REG_STATUS_RXOVERFLOW (1 << 3)
#define REG_STATUS_COMPLETE (1 << 22)
#define REG_STATUS_TXFIFO_SHIFT (6)
#define REG_STATUS_TXFIFO_MASK (31 << REG_STATUS_TXFIFO_SHIFT)
#define REG_STATUS_RXFIFO_SHIFT (11)
#define REG_STATUS_RXFIFO_MASK (31 << REG_STATUS_RXFIFO_SHIFT)

#define REG_PIN 0x00c
#define REG_PIN_CS (1 << 1)

#define REG_TXDATA 0x010
#define REG_RXDATA 0x020
#define REG_CLKDIV 0x030
#define REG_CLKDIV_MAX 0x7ff
#define REG_RXCNT 0x034
#define REG_WORD_DELAY 0x038
#define REG_TXCNT 0x04c
#define REG_MAX (0x50)

#define REG_FIFO_DEPTH 16
#define REG_FIFO_MAX_DEPTH (16 * 8)

#define REG(_s, _v) ((_s)->regs[(_v) >> 2])

struct AppleSPIState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    SSIBus *spi;
    AppleSIODMAEndpoint *tx_chan;
    AppleSIODMAEndpoint *rx_chan;

    qemu_irq irq;
    uint32_t last_irq;
    qemu_irq cs_line;

    Fifo32 rx_fifo;
    Fifo32 tx_fifo;
    uint32_t regs[APPLE_SPI_MMIO_SIZE >> 2];
    uint32_t mmio_size;

    int tx_chan_id;
    int rx_chan_id;
    bool dma_capable;
};

static int apple_spi_word_size(AppleSPIState *s)
{
    switch (REG_CFG_WORD_SIZE(REG(s, REG_CFG))) {
    case REG_CFG_WORD_SIZE_8B:
        return 1;
    case REG_CFG_WORD_SIZE_16B:
        return 2;
    case REG_CFG_WORD_SIZE_32B:
        return 4;
    default:
        break;
    }
    g_assert_not_reached();
}

static void apple_spi_update_xfer_tx(AppleSPIState *s)
{
    if (fifo32_is_empty(&s->tx_fifo)) {
        if ((REG_CFG_MODE(REG(s, REG_CFG))) == REG_CFG_MODE_DMA) {
            uint8_t buffer[REG_FIFO_MAX_DEPTH] = { 0 };
            int word_size = apple_spi_word_size(s);
            int dma_len = apple_sio_dma_remaining(s->tx_chan);
            int xfer_len = REG(s, REG_TXCNT) * word_size;
            int fifo_len = fifo32_num_free(&s->tx_fifo) * word_size;
            if (dma_len > xfer_len) {
                dma_len = xfer_len;
            }
            if (dma_len > fifo_len) {
                dma_len = fifo_len;
            }
            dma_len = apple_sio_dma_read(s->tx_chan, buffer, dma_len);
            if (dma_len == 0) {
                REG(s, REG_STATUS) |= REG_STATUS_TXEMPTY;
            } else {
                for (int i = 0; i < dma_len; i += word_size) {
                    uint32_t v = *(uint32_t *)&buffer[i];
                    v &= (1U << (word_size * 8)) - 1;
                    fifo32_push(&s->tx_fifo, v);
                }
            }
        } else {
            REG(s, REG_STATUS) |= REG_STATUS_TXEMPTY;
        }
    }
}

static void apple_spi_flush_rx(AppleSPIState *s)
{
    uint8_t buffer[REG_FIFO_MAX_DEPTH] = { 0 };
    int word_size = apple_spi_word_size(s);
    if ((REG_CFG_MODE(REG(s, REG_CFG))) != REG_CFG_MODE_DMA) {
        return;
    }
    int dma_len = apple_sio_dma_remaining(s->rx_chan);

    if (dma_len > fifo32_num_used(&s->rx_fifo)) {
        dma_len = fifo32_num_used(&s->rx_fifo);
    }
    if (dma_len == 0) {
        return;
    }

    for (int i = 0; i < dma_len; i += word_size) {
        uint32_t v = fifo32_pop(&s->rx_fifo);
        memcpy(buffer + i, &v, word_size);
    }

    dma_len = apple_sio_dma_write(s->rx_chan, buffer, dma_len);
}

static void apple_spi_update_xfer_rx(AppleSPIState *s)
{
    if (!fifo32_is_empty(&s->rx_fifo)) {
        REG(s, REG_STATUS) |= REG_STATUS_RXREADY;
    }
}

static void apple_spi_update_irq(AppleSPIState *s)
{
    uint32_t irq = 0;
    uint32_t mask = 0;

    if (REG(s, REG_CFG) & REG_CFG_IE_RXREADY) {
        mask |= REG_STATUS_RXREADY;
    }
    if (REG(s, REG_CFG) & REG_CFG_IE_TXEMPTY) {
        mask |= REG_STATUS_TXEMPTY;
    }
    if (REG(s, REG_CFG) & REG_CFG_IE_COMPLETE) {
        mask |= REG_STATUS_COMPLETE;
    }

    if (REG(s, REG_STATUS) & mask) {
        irq = 1;
    }
    if (irq != s->last_irq) {
        s->last_irq = irq;
        qemu_set_irq(s->irq, irq);
    }
}

static void apple_spi_update_cs(AppleSPIState *s)
{
    BusState *b = BUS(s->spi);
    BusChild *kid = QTAILQ_FIRST(&b->children);
    if (kid) {
        SSIPeripheralClass *ssc = SSI_PERIPHERAL_GET_CLASS(kid->child);
        if (ssc->cs_polarity == SSI_CS_NONE) {
            return;
        }
        qemu_irq cs_pin = qdev_get_gpio_in_named(kid->child, SSI_GPIO_CS, 0);
        if (cs_pin) {
            qemu_set_irq(cs_pin, (REG(s, REG_PIN) & REG_PIN_CS) != 0);
        }
    }
}

static void apple_spi_cs_set(void *opaque, int pin, int level)
{
    AppleSPIState *s = APPLE_SPI(opaque);
    if (level) {
        REG(s, REG_PIN) |= REG_PIN_CS;
    } else {
        REG(s, REG_PIN) &= ~REG_PIN_CS;
    }
    apple_spi_update_cs(s);
}

static void apple_spi_run(AppleSPIState *s)
{
    uint32_t tx;
    uint32_t rx;
    int word_size = apple_spi_word_size(s);

    if (REG_CFG_MODE(REG(s, REG_CFG)) == REG_CFG_MODE_INVALID) {
        return;
    }
    if (!(REG(s, REG_CTRL) & REG_CTRL_RUN)) {
        return;
    }
    if (REG(s, REG_RXCNT) == 0 && REG(s, REG_TXCNT) == 0) {
        return;
    }
    if ((REG_CFG_MODE(REG(s, REG_CFG))) == REG_CFG_MODE_DMA &&
        !s->dma_capable) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: DMA mode is not supported on this device\n",
                      __func__);
        return;
    }

    apple_spi_update_xfer_tx(s);

    while (REG(s, REG_TXCNT) && !fifo32_is_empty(&s->tx_fifo)) {
        tx = fifo32_pop(&s->tx_fifo);
        rx = 0;
        for (int i = 0; i < word_size; i++) {
            rx <<= 8;
            rx |= ssi_transfer(s->spi, tx & 0xff);
            tx >>= 8;
        }
        REG(s, REG_TXCNT)--;
        apple_spi_update_xfer_tx(s);
        if (REG(s, REG_RXCNT) > 0) {
            if (fifo32_is_full(&s->rx_fifo)) {
                apple_spi_flush_rx(s);
            }
            if (fifo32_is_full(&s->rx_fifo)) {
                qemu_log_mask(LOG_GUEST_ERROR, "%s: rx overflow\n", __func__);
                REG(s, REG_STATUS) |= REG_STATUS_RXOVERFLOW;
                break;
            } else {
                fifo32_push(&s->rx_fifo, rx);
                REG(s, REG_RXCNT)--;
                apple_spi_update_xfer_rx(s);
            }
        }
    }

    if (fifo32_is_full(&s->rx_fifo)) {
        apple_spi_flush_rx(s);
    }
    while (!fifo32_is_full(&s->rx_fifo) && (REG(s, REG_RXCNT) > 0) &&
           (REG(s, REG_CFG) & REG_CFG_AGD)) {
        rx = 0;
        for (int i = 0; i < word_size; i++) {
            rx <<= 8;
            rx |= ssi_transfer(s->spi, 0xff);
        }
        if (fifo32_is_full(&s->rx_fifo)) {
            apple_spi_flush_rx(s);
        }
        if (fifo32_is_full(&s->rx_fifo)) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: rx overflow\n", __func__);
            REG(s, REG_STATUS) |= REG_STATUS_RXOVERFLOW;
            break;
        } else {
            fifo32_push(&s->rx_fifo, rx);
            REG(s, REG_RXCNT)--;
            apple_spi_update_xfer_rx(s);
        }
    }

    apple_spi_flush_rx(s);
    if (REG(s, REG_RXCNT) == 0 && REG(s, REG_TXCNT) == 0) {
        REG(s, REG_STATUS) |= REG_STATUS_COMPLETE;
    }
}

static void apple_spi_reg_write(void *opaque, hwaddr addr, uint64_t data,
                                unsigned size)
{
    AppleSPIState *s = APPLE_SPI(opaque);
    uint32_t r = data;
    uint32_t *mmio = &REG(s, addr);
    uint32_t old = *mmio;
    bool cs_flg = false;
    bool run = false;

    if (addr >= REG_MAX) {
        qemu_log_mask(LOG_UNIMP,
                      "%s: reg WRITE @ 0x" HWADDR_FMT_plx
                      " value: 0x" HWADDR_FMT_plx "\n",
                      __func__, addr, data);
        return;
    }

    switch (addr) {
    case REG_CTRL:
        if (r & REG_CTRL_TX_RESET) {
            fifo32_reset(&s->tx_fifo);
            r &= ~REG_CTRL_TX_RESET;
        }
        if (r & REG_CTRL_RX_RESET) {
            fifo32_reset(&s->rx_fifo);
            r &= ~REG_CTRL_RX_RESET;
        }
        if (r & REG_CTRL_RUN) {
            run = true;
        }
        break;
    case REG_STATUS:
        r = old & (~r);
        break;
    case REG_PIN:
        cs_flg = true;
        break;
    case REG_TXDATA: {
        int word_size = apple_spi_word_size(s);
        if (fifo32_is_full(&s->tx_fifo)) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: tx overflow\n", __func__);
            r = 0;
            break;
        }
        r &= (1U << (word_size * 8)) - 1;
        fifo32_push(&s->tx_fifo, r);
        run = true;
        break;
    case REG_TXCNT:
    case REG_RXCNT:
    case REG_CFG:
        run = true;
        break;
    }
    default:
        break;
    }

    *mmio = r;
    if (cs_flg) {
        apple_spi_update_cs(s);
    }
    if (run) {
        apple_spi_run(s);
    }
    apple_spi_update_irq(s);
}

static uint64_t apple_spi_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    AppleSPIState *s = APPLE_SPI(opaque);
    uint32_t r;
    bool run = false;

    if (addr >= REG_MAX) {
        qemu_log_mask(LOG_UNIMP, "%s: reg READ @ 0x" HWADDR_FMT_plx "\n",
                      __func__, addr);
        return 0;
    }

    r = s->regs[addr >> 2];
    switch (addr) {
    case REG_RXDATA: {
        if (fifo32_is_empty(&s->rx_fifo)) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: rx underflow\n", __func__);
            r = 0;
            break;
        }
        r = fifo32_pop(&s->rx_fifo);
        if (fifo32_is_empty(&s->rx_fifo)) {
            run = true;
        }
        break;
    }
    case REG_STATUS: {
        uint32_t val = 0;
        val |= fifo32_num_used(&s->tx_fifo) << REG_STATUS_TXFIFO_SHIFT;
        val |= fifo32_num_used(&s->rx_fifo) << REG_STATUS_RXFIFO_SHIFT;
        val &= (REG_STATUS_TXFIFO_MASK | REG_STATUS_RXFIFO_MASK);
        r &= ~(REG_STATUS_TXFIFO_MASK | REG_STATUS_RXFIFO_MASK);
        r |= val;
        break;
    }
    default:
        break;
    }

    if (run) {
        apple_spi_run(s);
    }
    apple_spi_update_irq(s);
    return r;
}

static const MemoryRegionOps apple_spi_reg_ops = {
    .write = apple_spi_reg_write,
    .read = apple_spi_reg_read,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void apple_spi_reset(DeviceState *dev)
{
    AppleSPIState *s = APPLE_SPI(dev);

    memset(s->regs, 0, sizeof(s->regs));
    fifo32_reset(&s->tx_fifo);
    fifo32_reset(&s->rx_fifo);
}

SSIBus *apple_spi_get_bus(AppleSPIState *s)
{
    return s->spi;
}

static void apple_spi_realize(DeviceState *dev, struct Error **errp)
{
    AppleSPIState *s = APPLE_SPI(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    char mmio_name[32] = { 0 };
    char bus_name[32] = { 0 };
    Object *obj;
    AppleSIOState *sio;

    snprintf(bus_name, sizeof(bus_name), "%s.bus", dev->id);
    s->spi = ssi_create_bus(dev, (const char *)bus_name);

    snprintf(mmio_name, sizeof(mmio_name), "%s.mmio", dev->id);
    memory_region_init_io(&s->iomem, OBJECT(dev), &apple_spi_reg_ops, s,
                          mmio_name, s->mmio_size);

    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    sysbus_init_irq(sbd, &s->cs_line);
    qdev_init_gpio_in_named(dev, apple_spi_cs_set, SSI_GPIO_CS, 1);

    fifo32_create(&s->tx_fifo, REG_FIFO_DEPTH);
    fifo32_create(&s->rx_fifo, REG_FIFO_DEPTH);

    obj = object_property_get_link(OBJECT(dev), "sio", NULL);
    sio = APPLE_SIO(obj);

    if (sio == NULL) {
        if (s->dma_capable) {
            warn_report("%s: SPI bus is DMA capable, but no SIO is attached. "
                        "This is a bug.",
                        __func__);
        }
        s->dma_capable = false;
    } else if (s->dma_capable) {
        s->tx_chan = apple_sio_get_endpoint(sio, s->tx_chan_id);
        s->rx_chan = apple_sio_get_endpoint(sio, s->rx_chan_id);
    }
}

SysBusDevice *apple_spi_create(DTBNode *node)
{
    DeviceState *dev = qdev_new(TYPE_APPLE_SPI);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    AppleSPIState *s = APPLE_SPI(dev);
    DTBProp *prop = dtb_find_prop(node, "reg");
    uint64_t mmio_size = ((hwaddr *)prop->data)[1];

    prop = dtb_find_prop(node, "name");
    dev->id = g_strdup((const char *)prop->data);
    s->mmio_size = mmio_size;

    if ((prop = dtb_find_prop(node, "dma-channels")) != NULL) {
        uint32_t *data = (uint32_t *)prop->data;
        s->dma_capable = true;
        s->tx_chan_id = data[0];
        s->rx_chan_id = data[8];
    }
    return sbd;
}

static void apple_spi_init(Object *obj)
{
    AppleSPIState *s = APPLE_SPI(obj);
    s->mmio_size = APPLE_SPI_MMIO_SIZE;
}

static const VMStateDescription vmstate_apple_spi = {
    .name = "apple_spi",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields =
        (VMStateField[]){
            VMSTATE_UINT32_ARRAY(regs, AppleSPIState, APPLE_SPI_MMIO_SIZE >> 2),
            VMSTATE_FIFO32(rx_fifo, AppleSPIState),
            VMSTATE_FIFO32(tx_fifo, AppleSPIState),
            VMSTATE_UINT32(last_irq, AppleSPIState),
            VMSTATE_UINT32(mmio_size, AppleSPIState),
            VMSTATE_END_OF_LIST(),
        }
};

static void apple_spi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "Apple Samsung SPI Controller";

    device_class_set_legacy_reset(dc, apple_spi_reset);
    dc->realize = apple_spi_realize;
    dc->vmsd = &vmstate_apple_spi;
}

static const TypeInfo apple_spi_type_info = {
    .name = TYPE_APPLE_SPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AppleSPIState),
    .instance_init = apple_spi_init,
    .class_init = apple_spi_class_init,
};

static void apple_spi_register_types(void)
{
    type_register_static(&apple_spi_type_info);
}

type_init(apple_spi_register_types)
