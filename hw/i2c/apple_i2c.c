#include "qemu/osdep.h"
#include "hw/i2c/apple_i2c.h"
#include "hw/i2c/i2c.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

// #define DEBUG_APPLE_I2C

#define MMIO_SIZE (0x10000)

#define REG_MTXFIFO 0x00
#define REG_MRXFIFO 0x04
#define REG_MCNT 0x08
#define REG_XFSTA 0x0C
#define REG_SMSTA 0x14
#define REG_IMASK 0x18
#define REG_CTL 0x1C
#define REG_VERSION 0x28
#define REG_RDCOUNT 0x2C
#define REG_FILTER 0x38

#define kMTXFIFOWrite (0 << 10)
#define kMTXFIFORead (1 << 10)
#define kMTXFIFOStop (1 << 9)
#define kMTXFIFOStart (1 << 8)
#define kMTXFIFOData(_v) (((_v) >> 0) & 0xff)

#define kMRXFIFOEmpty (1 << 8)
#define kMRXFIFOData(_d) (((_d) & 0xff) << 0)

#define kMCNTRxCnt(_v) (((_v) & 0xff) << 8)
#define kMCNTTxCnt(_v) (((_v) & 0xff) << 0)

#define kXFSTAmst(_v) (((_v) >> 28) & 0xf)
#define kIDLE 0
#define kXFSTAxfifo(_v) (((_v) >> 21) & 0x3)
#define kXFSTAxcnt(_v) (((_v) >> 0) & 0xfffff)

/* Transaction in progress */
#define kSMSTAxip (1 << 28)

/* Transaction ended */
#define kSMSTAxen (1 << 27)

/* Unjam failed */
#define kSMSTAujf (1 << 26)
#define kSMSTAjmd (1 << 25)

/* Bus jammed */
#define kSMSTAjam (1 << 24)

/* IO error */
#define kSMSTAmto (1 << 23)

/* NAK */
#define kSMSTAmtn (1 << 21)

/* RX full */
#define kSMSTAmrf (1 << 20)
/* RX not empty */
#define kSMSTAmrne (1 << 19)

#define kSMSTAmtr (1 << 18)

/* TX full */
#define kSMSTAmtf (1 << 17)
/* Tx empty */
#define kSMSTAmte (1 << 16)

/* timeout error */
#define kSMSTAtom (1 << 6)

/* reset RX */
#define kCTLMRR (1 << 10)

/* reset TX */
#define kCTLMTR (1 << 9)

/* unjam */
#define kCTLUJM (1 << 8)

#define kCTLCLK(_c) (((_c) & 0xff) << 0)

#define kMIXDIV 4

#define kRDCOUNT(_v) (((_v) >> 8) & 0xff)

#define REG(_s, _x) *(uint32_t *)&_s->reg[_x]

static void apple_i2c_update_irq(AppleI2CState *s)
{
    bool level = false;
    if (REG(s, REG_SMSTA) & REG(s, REG_IMASK)) {
        level = true;
    } else {
        level = false;
    }
    if (level != s->last_irq) {
        qemu_set_irq(s->irq, level);
        s->last_irq = level;
    }
}

static void apple_i2c_reg_write(void *opaque, hwaddr addr, uint64_t data,
                                unsigned size)
{
    AppleI2CState *s = APPLE_I2C(opaque);
    DeviceState *dev = DEVICE(opaque);
#ifdef DEBUG_APPLE_I2C
    qemu_log_mask(LOG_UNIMP,
                  "%s: reg WRITE @ 0x" HWADDR_FMT_plx
                  " value: 0x" HWADDR_FMT_plx "\n",
                  dev->id, addr, data);
#endif

    uint32_t *mmio = (uint32_t *)&s->reg[addr];
    uint32_t value = data;
    uint32_t orig = *mmio;
    bool iflg = false;

    switch (addr) {
    case REG_MTXFIFO: {
        uint8_t i2c_addr = kMTXFIFOData(value) >> 1;
        iflg = true;
        if ((value & kMTXFIFOStart)) {
            if (kMTXFIFOData(value) & 1) {
                s->is_recv = true;
            } else {
                s->is_recv = false;
            }
            if (i2c_start_transfer(s->bus, i2c_addr, s->is_recv) != 0) {
                qemu_log_mask(LOG_GUEST_ERROR, "%s: can't find device @ 0x%x\n",
                              dev->id, i2c_addr);
                REG(s, REG_SMSTA) |= kSMSTAmtn;
                break;
            }

            s->xip = true;
            REG(s, REG_SMSTA) |= kSMSTAxip;
        } else if (s->xip) {
            if (value & kMTXFIFORead) {
                uint8_t len = kMTXFIFOData(value);
                if (!s->is_recv) {
                    s->is_recv = 1;
                    if (i2c_start_transfer(s->bus, i2c_addr, s->is_recv) != 0) {
                        REG(s, REG_SMSTA) |= kSMSTAmtn;
                        break;
                    }
                }
                while (len--) {
                    fifo8_push(&s->rx_fifo, i2c_recv(s->bus));
                }
                if (kMTXFIFOData(value) > 0) {
                    REG(s, REG_SMSTA) |= (kSMSTAmrne);
                    if (kMTXFIFOData(value) >= kRDCOUNT(REG(s, REG_RDCOUNT))) {
                        REG(s, REG_SMSTA) |= (kSMSTAmrf);
                    }
                }
            } else {
                if (s->is_recv) {
                    s->is_recv = 0;
                    if (i2c_start_transfer(s->bus, i2c_addr, s->is_recv) != 0) {
                        REG(s, REG_SMSTA) |= kSMSTAmtn;
                        break;
                    }
                }
                if (i2c_send(s->bus, kMTXFIFOData(value))) {
                    REG(s, REG_SMSTA) |= kSMSTAmtn;
                    /* XXX: Should we end it here? */
                }
            }
        }
        if (value & kMTXFIFOStop) {
            if (s->xip) {
                i2c_end_transfer(s->bus);
                REG(s, REG_SMSTA) |= kSMSTAxen;
                s->xip = false;
            }
        }
        break;
    }
    case REG_SMSTA:
        value = orig & (~value);
        iflg = true;
        break;
    case REG_IMASK:
        iflg = true;
        break;
    case REG_CTL:
        if (value & kCTLMRR) {
            fifo8_reset(&s->rx_fifo);
        }
        value = 0;
        break;
    default:
        break;
    }

    *mmio = value;
    if (iflg) {
        apple_i2c_update_irq(s);
    }
}

static uint64_t apple_i2c_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    AppleI2CState *s = APPLE_I2C(opaque);
    uint32_t *mmio = (uint32_t *)&s->reg[addr];
    uint32_t value = *mmio;

    switch (addr) {
    case REG_MRXFIFO:
        if (fifo8_is_empty(&s->rx_fifo)) {
            value = kMRXFIFOEmpty;
            break;
        }
        value = kMRXFIFOData(fifo8_pop(&s->rx_fifo));
        break;
    case REG_MCNT:
        value &= ~(kMCNTRxCnt(0xff) | kMCNTTxCnt(0xff));
        value |= kMCNTRxCnt(fifo8_num_used(&s->rx_fifo));
        break;
    case REG_VERSION:
        value = 2;
        break;
    default:
        break;
    }

#ifdef DEBUG_APPLE_I2C
    qemu_log_mask(LOG_UNIMP,
                  "%s: reg READ @ 0x" HWADDR_FMT_plx " value: 0x%x\n",
                  DEVICE(s)->id, addr, value);
#endif
    return value;
}

static const MemoryRegionOps i2c_reg_ops = {
    .write = apple_i2c_reg_write,
    .read = apple_i2c_reg_read,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void apple_i2c_reset_enter(Object *obj, ResetType type)
{
    AppleHWI2CClass *c = APPLE_I2C_GET_CLASS(obj);
    AppleI2CState *s = APPLE_I2C(obj);

    if (c->parent_phases.enter) {
        c->parent_phases.enter(obj, type);
    }
    memset(s->reg, 0, sizeof(s->reg));
    s->nak = s->xip = s->is_recv = 0;
    fifo8_reset(&s->rx_fifo);
}

static void apple_i2c_reset_hold(Object *obj, ResetType type)
{
    AppleHWI2CClass *c = APPLE_I2C_GET_CLASS(obj);
    AppleI2CState *s = APPLE_I2C(obj);

    if (c->parent_phases.hold != NULL) {
        c->parent_phases.hold(obj, type);
    }
    qemu_set_irq(s->irq, 0);
    s->last_irq = 0;
}

static void apple_i2c_reset_exit(Object *obj, ResetType type)
{
    AppleHWI2CClass *c = APPLE_I2C_GET_CLASS(obj);
    AppleI2CState *s = APPLE_I2C(obj);

    if (c->parent_phases.exit != NULL) {
        c->parent_phases.exit(obj, type);
    }

    qemu_set_irq(s->sda, 1);
    qemu_set_irq(s->scl, 1);
}

SysBusDevice *apple_i2c_create(const char *name)
{
    DeviceState *dev = qdev_new(TYPE_APPLE_I2C);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    AppleI2CState *s = APPLE_I2C(dev);
    char bus_name[32] = { 0 };

    dev->id = g_strdup(name);
    snprintf(bus_name, sizeof(bus_name), "%s.bus", name);
    s->bus = i2c_init_bus(dev, bus_name);
    memory_region_init_io(&s->iomem, OBJECT(dev), &i2c_reg_ops, s, dev->id,
                          MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    qdev_init_gpio_out_named(DEVICE(dev), &s->sda, APPLE_I2C_SDA, 1);
    qdev_init_gpio_out_named(DEVICE(dev), &s->scl, APPLE_I2C_SCL, 1);

    s->last_irq = 0;
    fifo8_create(&s->rx_fifo, 0x100);

    return sbd;
}

static const VMStateDescription vmstate_apple_i2c = {
    .name = "apple_i2c",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields =
        (VMStateField[]){
            VMSTATE_UINT8_ARRAY(reg, AppleI2CState, APPLE_I2C_MMIO_SIZE),
            VMSTATE_FIFO8(rx_fifo, AppleI2CState),
            VMSTATE_BOOL(last_irq, AppleI2CState),
            VMSTATE_BOOL(nak, AppleI2CState),
            VMSTATE_BOOL(xip, AppleI2CState),
            VMSTATE_BOOL(is_recv, AppleI2CState),
            VMSTATE_END_OF_LIST(),
        }
};

static void apple_i2c_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    AppleHWI2CClass *c = APPLE_I2C_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "Apple I2C Controller";
    dc->vmsd = &vmstate_apple_i2c;
    resettable_class_set_parent_phases(rc, apple_i2c_reset_enter,
                                       apple_i2c_reset_hold,
                                       apple_i2c_reset_exit, &c->parent_phases);
}

static const TypeInfo apple_i2c_type_info = {
    .name = TYPE_APPLE_I2C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AppleI2CState),
    .class_size = sizeof(AppleHWI2CClass),
    .class_init = apple_i2c_class_init,
};

static void apple_i2c_register_types(void)
{
    type_register_static(&apple_i2c_type_info);
}

type_init(apple_i2c_register_types)
