#include "qemu/osdep.h"
#include "hw/arm/apple-silicon/dtb.h"
#include "hw/irq.h"
#include "hw/misc/apple-silicon/spmi-pmu.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "sysemu/runstate.h"
#include "sysemu/sysemu.h"

#define TYPE_APPLE_SPMI_PMU "apple.spmi.pmu"
OBJECT_DECLARE_SIMPLE_TYPE(AppleSPMIPMUState, APPLE_SPMI_PMU)

#define LEG_SCRPAD_OFFSET_SECS_OFFSET (4)
#define LEG_SCRPAD_OFFSET_TICKS_OFFSET (21)
#define RTC_TICK_FREQ (32768)
#define kDIALOG_RTC_CONTROL_MONITOR (1 << 0)
#define kDIALOG_RTC_CONTROL_ALARM_EN (1 << 6)
#define kDIALOG_RTC_EVENT_ALARM (1 << 0)

#define REG32(_p, _offset) (*(uint32_t *)&_p->reg[_offset])

struct AppleSPMIPMUState {
    /*< private >*/
    SPMISlave parent_obj;

    /*< public >*/
    qemu_irq irq;
    QEMUTimer *timer;
    uint64_t rtc_offset;
    uint64_t tick_offset;
    uint32_t tick_period;
    uint32_t reg_leg_scrpad;
    uint32_t reg_rtc;
    uint32_t reg_rtc_irq_mask;
    uint32_t reg_alarm;
    uint32_t reg_alarm_ctrl;
    uint32_t reg_alarm_event;
    uint8_t reg[0xffff];
    uint16_t addr;
};

static unsigned int frq_to_period_ns(unsigned int freq_hz)
{
    return NANOSECONDS_PER_SECOND > freq_hz ? NANOSECONDS_PER_SECOND / freq_hz :
                                              1;
}

static uint64_t G_GNUC_UNUSED tick_to_ns(AppleSPMIPMUState *p, uint64_t tick)
{
    return (tick >> 15) * NANOSECONDS_PER_SECOND +
           (tick & 0x7fff) * p->tick_period;
}

static uint64_t rtc_get_tick(AppleSPMIPMUState *p, uint64_t *out_ns)
{
    uint64_t now = qemu_clock_get_ns(rtc_clock);
    uint64_t offset = p->rtc_offset;
    if (out_ns) {
        *out_ns = now;
    }
    now -= offset;
    return ((now / NANOSECONDS_PER_SECOND) << 15) |
           ((now / p->tick_period) & 0x7fff);
}

static uint64_t apple_spmi_pmu_get_tick_offset(AppleSPMIPMUState *p)
{
    uint64_t tick_offset = 0;

    tick_offset =
        ((uint64_t)REG32(p, p->reg_leg_scrpad + LEG_SCRPAD_OFFSET_SECS_OFFSET)
         << 15) +
        (REG32(p, p->reg_leg_scrpad + LEG_SCRPAD_OFFSET_TICKS_OFFSET) & 0x7fff);

    return tick_offset;
}

static void apple_spmi_pmu_set_tick_offset(AppleSPMIPMUState *p,
                                           uint64_t tick_offset)
{
    REG32(p, p->reg_leg_scrpad + LEG_SCRPAD_OFFSET_SECS_OFFSET) =
        tick_offset >> 15;
    p->reg[p->reg_leg_scrpad + LEG_SCRPAD_OFFSET_TICKS_OFFSET + 0] =
        tick_offset & 0xff;
    p->reg[p->reg_leg_scrpad + LEG_SCRPAD_OFFSET_TICKS_OFFSET + 1] =
        (tick_offset >> 8) & 0x7f;
}

static void apple_spmi_pmu_update_irq(AppleSPMIPMUState *p)
{
    if (REG32(p, p->reg_rtc_irq_mask) & REG32(p, p->reg_alarm_event)) {
        qemu_irq_raise(p->irq);
    } else {
        qemu_irq_lower(p->irq);
    }
}

static void apple_spmi_pmu_alarm(void *opaque)
{
    AppleSPMIPMUState *p = APPLE_SPMI_PMU(opaque);
    REG32(p, p->reg_alarm_event) |= kDIALOG_RTC_EVENT_ALARM;
    apple_spmi_pmu_update_irq(p);
    qemu_system_wakeup_request(QEMU_WAKEUP_REASON_RTC, NULL);
}

static void apple_spmi_pmu_set_alarm(AppleSPMIPMUState *p)
{
    uint32_t seconds = REG32(p, p->reg_alarm) - (rtc_get_tick(p, NULL) >> 15);
    if (REG32(p, p->reg_alarm_ctrl) & kDIALOG_RTC_CONTROL_ALARM_EN) {
        if (seconds == 0) {
            timer_del(p->timer);
            apple_spmi_pmu_alarm(p);
        } else {
            int64_t now = qemu_clock_get_ns(rtc_clock);
            timer_mod_ns(p->timer,
                         now + (int64_t)seconds * NANOSECONDS_PER_SECOND);
        }
    } else {
        timer_del(p->timer);
    }
}

static int apple_spmi_pmu_send(SPMISlave *s, uint8_t *data, uint8_t len)
{
    AppleSPMIPMUState *p = APPLE_SPMI_PMU(s);
    bool aflg = false;
    uint16_t addr;
    for (addr = p->addr; addr < p->addr + len; addr++) {
        p->reg[addr] = data[addr - p->addr];
        if (addr == p->reg_alarm_ctrl) {
            aflg = true;
        }
        if (addr >= p->reg_alarm && addr < p->reg_alarm + 4) {
            aflg = true;
        }
        if ((addr >= p->reg_leg_scrpad + LEG_SCRPAD_OFFSET_SECS_OFFSET &&
             addr < p->reg_leg_scrpad + LEG_SCRPAD_OFFSET_SECS_OFFSET + 4) ||
            (addr >= p->reg_leg_scrpad + LEG_SCRPAD_OFFSET_TICKS_OFFSET &&
             addr < p->reg_leg_scrpad + LEG_SCRPAD_OFFSET_TICKS_OFFSET + 2)) {
            p->tick_offset = apple_spmi_pmu_get_tick_offset(p);
        }
    }
    p->addr = addr;
    if (aflg) {
        apple_spmi_pmu_set_alarm(p);
    }
    return len;
}

static int apple_spmi_pmu_recv(SPMISlave *s, uint8_t *data, uint8_t len)
{
    AppleSPMIPMUState *p = APPLE_SPMI_PMU(s);
    uint16_t addr;

    for (addr = p->addr; addr < p->addr + len; addr++) {
        if (addr >= p->reg_rtc && addr < p->reg_rtc + 6) {
            uint64_t now = rtc_get_tick(p, NULL);
            p->reg[p->reg_rtc] = now << 1;
            p->reg[p->reg_rtc + 1] = now >> 7;
            p->reg[p->reg_rtc + 2] = now >> 15;
            p->reg[p->reg_rtc + 3] = now >> 23;
            p->reg[p->reg_rtc + 4] = now >> 31;
            p->reg[p->reg_rtc + 5] = now >> 39;
        }
        data[addr - p->addr] = p->reg[addr];
    }
    p->addr = addr;
    return len;
}

static int apple_spmi_pmu_command(SPMISlave *s, uint8_t opcode, uint16_t addr)
{
    AppleSPMIPMUState *p = APPLE_SPMI_PMU(s);
    p->addr = addr;

    switch (opcode) {
    case SPMI_CMD_EXT_READ:
    case SPMI_CMD_EXT_READL:
    case SPMI_CMD_EXT_WRITE:
    case SPMI_CMD_EXT_WRITEL:
        return 0;
    default:
        return 1;
    }
}

DeviceState *apple_spmi_pmu_create(DTBNode *node)
{
    DeviceState *dev = qdev_new(TYPE_APPLE_SPMI_PMU);
    AppleSPMIPMUState *p = APPLE_SPMI_PMU(dev);
    DTBProp *prop;

    prop = dtb_find_prop(node, "reg");
    g_assert_nonnull(prop);
    spmi_set_slave_sid(SPMI_SLAVE(dev), *(uint32_t *)prop->data);

    prop = dtb_find_prop(node, "info-rtc");
    p->reg_rtc = *(uint32_t *)prop->data;

    prop = dtb_find_prop(node, "info-rtc_alarm_offset");
    p->reg_alarm = *(uint32_t *)prop->data;

    prop = dtb_find_prop(node, "info-rtc_alarm_ctrl");
    p->reg_alarm_ctrl = *(uint32_t *)prop->data;

    prop = dtb_find_prop(node, "info-rtc_alarm_event");
    p->reg_alarm_event = *(uint32_t *)prop->data;

    prop = dtb_find_prop(node, "info-rtc_irq_mask_offset");
    p->reg_rtc_irq_mask = *(uint32_t *)prop->data;

    prop = dtb_find_prop(node, "info-leg_scrpad");
    p->reg_leg_scrpad = *(uint32_t *)prop->data;

    p->tick_period = frq_to_period_ns(RTC_TICK_FREQ);
    p->tick_offset = rtc_get_tick(p, &p->rtc_offset);
    apple_spmi_pmu_set_tick_offset(p, p->tick_offset);

    p->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, apple_spmi_pmu_alarm, p);
    qemu_system_wakeup_enable(QEMU_WAKEUP_REASON_RTC, true);

    qdev_init_gpio_out(dev, &p->irq, 1);
    return dev;
}

static const VMStateDescription vmstate_apple_spmi_pmu = {
    .name = "apple_spmi_pmu",
    .fields =
        (VMStateField[]){
            VMSTATE_UINT64(tick_offset, AppleSPMIPMUState),
            VMSTATE_UINT64(rtc_offset, AppleSPMIPMUState),
            VMSTATE_UINT16(addr, AppleSPMIPMUState),
            VMSTATE_UINT8_ARRAY(reg, AppleSPMIPMUState, 0xffff),
            VMSTATE_TIMER_PTR(timer, AppleSPMIPMUState),
            VMSTATE_END_OF_LIST(),
        }
};

static void apple_spmi_pmu_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SPMISlaveClass *sc = SPMI_SLAVE_CLASS(klass);

    dc->desc = "Apple Dialog SPMI PMU";
    dc->vmsd = &vmstate_apple_spmi_pmu;

    sc->send = apple_spmi_pmu_send;
    sc->recv = apple_spmi_pmu_recv;
    sc->command = apple_spmi_pmu_command;
}

static const TypeInfo apple_spmi_pmu_type_info = {
    .name = TYPE_APPLE_SPMI_PMU,
    .parent = TYPE_SPMI_SLAVE,
    .instance_size = sizeof(AppleSPMIPMUState),
    .class_init = apple_spmi_pmu_class_init,
};

static void apple_spmi_pmu_register_types(void)
{
    type_register_static(&apple_spmi_pmu_type_info);
}

type_init(apple_spmi_pmu_register_types)
