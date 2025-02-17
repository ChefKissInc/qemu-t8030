#include "qemu/osdep.h"
#include "hw/arm/apple-silicon/dtb.h"
#include "hw/irq.h"
#include "hw/watchdog/apple_wdt.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "sysemu/watchdog.h"
#include "trace.h"

#define TYPE_APPLE_WDT "apple.wdt"
OBJECT_DECLARE_SIMPLE_TYPE(AppleWDTState, APPLE_WDT)

#define REG_CHIP_WDOG_TMR (0x0)
#define REG_CHIP_WDOG_RST_CNT (0x4)
#define REG_CHIP_WDOG_INTR_CNT (0x8)
#define REG_CHIP_WDOG_CTL (0xc)
#define REG_SYS_WDOG_TMR (0x10)
#define REG_SYS_WDOG_RST_CNT (0x14)
#define REG_SYS_WDOG_CTL (0x1c)

#define WDOG_CTL_EN_IRQ (1 << 0)
#define WDOG_CTL_ACK_IRQ (1 << 1)
#define WDOG_CTL_EN_RESET (1 << 2)

#define WDOG_CNTFRQ_HZ (24000000)

struct AppleWDTState {
    SysBusDevice parent_obj;
    MemoryRegion iomems[2];
    qemu_irq irqs[2];

    QEMUTimer *timer;
    uint64_t cnt_period_ns;
    uint64_t cntfrq_hz;
#pragma pack(push, 1)
    union {
#define REG_SIZE 0x20
        uint32_t raw[0x20 / sizeof(uint32_t)];
        struct {
            uint32_t chip_timer;
            uint32_t chip_reset_counter;
            uint32_t chip_interrupt_counter;
            uint32_t chip_control;

            uint32_t sys_timer;
            uint32_t sys_reset_counter;
            uint32_t rsvd;
            uint32_t sys_control;
        };
    } reg;
#pragma pack(pop)

    uint32_t scratch;
};

static unsigned int wdog_cntfrq_period_ns(AppleWDTState *s)
{
    return NANOSECONDS_PER_SECOND > s->cntfrq_hz ?
               NANOSECONDS_PER_SECOND / s->cntfrq_hz :
               1;
}

static void apple_wdt_reset(DeviceState *dev);

static void wdt_set_irq(AppleWDTState *s, int level)
{
    trace_apple_wdt_set_irq(level != 0);
    if (level) {
        qemu_set_irq(s->irqs[0], 1);
    } else {
        qemu_set_irq(s->irqs[0], 0);
    }
}

static inline uint32_t wdt_get_clock(AppleWDTState *s)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / s->cnt_period_ns;
}

static inline uint32_t wdt_get_chip_timer(AppleWDTState *s)
{
    return wdt_get_clock(s) - s->reg.chip_timer;
}

static inline uint32_t wdt_get_sys_timer(AppleWDTState *s)
{
    return wdt_get_clock(s) - s->reg.sys_timer;
}

static void wdt_update(void *opaque)
{
    AppleWDTState *s = APPLE_WDT(opaque);
    uint64_t expiry = 0xffffffff;
    uint32_t chip_tmr = wdt_get_chip_timer(s);
    uint32_t sys_tmr = wdt_get_sys_timer(s);

    if (s->reg.chip_control & WDOG_CTL_EN_RESET) {
        if (chip_tmr >= s->reg.chip_reset_counter) {
            trace_apple_wdt_chip_reset();
            watchdog_perform_action();
            apple_wdt_reset(DEVICE(s));
            return;
        } else {
            uint32_t d = s->reg.chip_reset_counter - chip_tmr;
            expiry = MIN(expiry, d);
        }
    }

    if (s->reg.sys_control & WDOG_CTL_EN_RESET) {
        if (sys_tmr >= s->reg.sys_reset_counter) {
            trace_apple_wdt_system_reset();
            watchdog_perform_action();
            apple_wdt_reset(DEVICE(s));
            return;
        } else {
            uint32_t d = s->reg.sys_reset_counter - sys_tmr;
            expiry = MIN(expiry, d);
        }
    }

    if (s->reg.chip_control & WDOG_CTL_EN_IRQ) {
        if (chip_tmr >= s->reg.chip_interrupt_counter) {
            if (!(s->reg.chip_control & WDOG_CTL_ACK_IRQ)) {
                s->reg.chip_control |= WDOG_CTL_ACK_IRQ;
                wdt_set_irq(s, 1);
            }
        } else {
            uint32_t d = s->reg.chip_interrupt_counter - chip_tmr;
            expiry = MIN(expiry, d);
        }
    }
    expiry *= s->cnt_period_ns;
    timer_mod_ns(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + expiry);
}

static void wdt_reg_write(void *opaque, hwaddr addr, uint64_t data,
                          unsigned size)
{
    AppleWDTState *s = APPLE_WDT(opaque);
    uint32_t index = addr >> 2;
    uint32_t *mmio;
    uint32_t old;
    uint32_t val = data;
    bool nowrite = false;

    if (addr >= REG_SIZE) {
        qemu_log_mask(LOG_UNIMP, "%s: Bad offset 0x" HWADDR_FMT_plx "\n",
                      __func__, addr);
        return;
    }

    mmio = &s->reg.raw[index];
    old = *mmio;

    switch (addr) {
    case REG_CHIP_WDOG_TMR:
        val = wdt_get_clock(s) - val;
        break;
    case REG_CHIP_WDOG_CTL:
        if (val & WDOG_CTL_ACK_IRQ) {
            wdt_set_irq(s, 0);
        }
        val &= ~WDOG_CTL_ACK_IRQ;
        break;
    case REG_SYS_WDOG_TMR:
        val = wdt_get_clock(s) - val;
        break;
    default:
        break;
    }

    if (!nowrite) {
        *mmio = val;
    }

    trace_apple_wdt_write(addr, data, old, val);
    timer_mod_ns(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

static uint64_t wdt_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    AppleWDTState *s = APPLE_WDT(opaque);
    uint32_t val = 0;
    uint32_t *mmio = NULL;

    if (addr >= REG_SIZE) {
        qemu_log_mask(LOG_UNIMP, "%s: Bad offset 0x" HWADDR_FMT_plx "\n",
                      __func__, addr);
        return 0;
    }

    mmio = &s->reg.raw[addr >> 2];

    val = *mmio;

    switch (addr) {
    case REG_CHIP_WDOG_TMR:
        val = wdt_get_chip_timer(s);
        break;
    case REG_SYS_WDOG_TMR:
        val = wdt_get_sys_timer(s);
        break;
    default:
        break;
    }

    trace_apple_wdt_read(addr, val);
    return val;
}

static void apple_wdt_reset(DeviceState *dev)
{
    AppleWDTState *s = APPLE_WDT(dev);
    memset(s->reg.raw, 0, REG_SIZE);
}

static const MemoryRegionOps wdt_reg_ops = {
    .write = wdt_reg_write,
    .read = wdt_reg_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned = false,
};

static void apple_wdt_realize(DeviceState *dev, Error **errp)
{
    AppleWDTState *s = APPLE_WDT(dev);
    s->cntfrq_hz = WDOG_CNTFRQ_HZ;
    s->cnt_period_ns = wdog_cntfrq_period_ns(s);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, wdt_update, s);
    apple_wdt_reset(dev);
}

static void apple_wdt_unrealize(DeviceState *dev)
{
    AppleWDTState *s = APPLE_WDT(dev);

    timer_free(s->timer);
    s->timer = NULL;
}

SysBusDevice *apple_wdt_create(DTBNode *node)
{
    DeviceState *dev;
    AppleWDTState *s;
    SysBusDevice *sbd;
    DTBProp *prop;
    uint64_t *reg;

    dev = qdev_new(TYPE_APPLE_WDT);
    s = APPLE_WDT(dev);
    sbd = SYS_BUS_DEVICE(dev);

    prop = dtb_find_prop(node, "wdt-version");
    assert(prop);
    *(uint32_t *)prop->data = 1;

    prop = dtb_find_prop(node, "reg");
    assert(prop);

    reg = (uint64_t *)prop->data;

    /*
     * 0: reg
     * 1: scratch reg
     */
    memory_region_init_io(&s->iomems[0], OBJECT(dev), &wdt_reg_ops, s,
                          TYPE_APPLE_WDT ".reg", reg[1]);

    sysbus_init_mmio(sbd, &s->iomems[0]);

    memory_region_init_ram_device_ptr(&s->iomems[1], OBJECT(dev),
                                      TYPE_APPLE_WDT ".scratch",
                                      sizeof(s->scratch), &s->scratch);
    sysbus_init_mmio(sbd, &s->iomems[1]);

    sysbus_init_irq(sbd, &s->irqs[0]);
    sysbus_init_irq(sbd, &s->irqs[1]);


    return sbd;
}

static const VMStateDescription vmstate_apple_wdt = {
    .name = "apple_wdt",
    .fields =
        (VMStateField[]){
            VMSTATE_TIMER_PTR(timer, AppleWDTState),
            VMSTATE_UINT64(cnt_period_ns, AppleWDTState),
            VMSTATE_UINT64(cntfrq_hz, AppleWDTState),
            VMSTATE_UINT32_ARRAY(reg.raw, AppleWDTState,
                                 REG_SIZE / sizeof(uint32_t)),
            VMSTATE_UINT32(scratch, AppleWDTState),
            VMSTATE_TIMER_PTR(timer, AppleWDTState),
            VMSTATE_END_OF_LIST(),
        }
};

static void apple_wdt_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = apple_wdt_realize;
    dc->unrealize = apple_wdt_unrealize;
    device_class_set_legacy_reset(dc, apple_wdt_reset);
    dc->desc = "Apple Watch Dog Timer";
    dc->vmsd = &vmstate_apple_wdt;
    set_bit(DEVICE_CATEGORY_WATCHDOG, dc->categories);
}

static const TypeInfo apple_wdt_info = {
    .name = TYPE_APPLE_WDT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AppleWDTState),
    .class_init = apple_wdt_class_init,
};

static void apple_wdt_register_types(void)
{
    type_register_static(&apple_wdt_info);
}

type_init(apple_wdt_register_types);
