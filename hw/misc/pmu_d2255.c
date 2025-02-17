/*
 * Apple PMU D2255.
 *
 * Copyright (c) 2023-2025 Visual Ehrmanntraut.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "hw/irq.h"
#include "hw/misc/pmu_d2255.h"
#include "qemu/compiler.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "sysemu/sysemu.h"

// #define DEBUG_PMU_D2255

#define RTC_TICK_FREQ (32768)

#define REG_DIALOG_ALARM_EVENT (0x142)
#define DIALOG_RTC_EVENT_ALARM (1 << 0)

#define REG_DIALOG_RTC_IRQ_MASK (0x1C0)
#define REG_DIALOG_MASK_REV_CODE (0x200)
#define REG_DIALOG_TRIM_REL_CODE (0x201)
#define REG_DIALOG_PLATFORM_ID (0x202)
#define REG_DIALOG_DEVICE_ID0 (0x203)
#define REG_DIALOG_DEVICE_ID1 (0x204)
#define REG_DIALOG_DEVICE_ID2 (0x205)
#define REG_DIALOG_DEVICE_ID3 (0x206)
#define REG_DIALOG_DEVICE_ID4 (0x207)
#define REG_DIALOG_DEVICE_ID5 (0x208)
#define REG_DIALOG_DEVICE_ID6 (0x209)
#define REG_DIALOG_DEVICE_ID7 (0x20A)

#define REG_DIALOG_RTC_CONTROL (0x500)
#define DIALOG_RTC_CONTROL_MONITOR (1 << 0)
#define DIALOG_RTC_CONTROL_ALARM_EN (1 << 6)

#define REG_DIALOG_RTC_SUB_SECOND_A (0x0502)

#define REG_DIALOG_SCRATCH (0x5000)
#define DIALOG_SCRATCH_LEN (0x27)
#define OFF_DIALOG_SCRATCH_SECS_OFFSET (4)
#define OFF_DIALOG_SCRATCH_TICKS_OFFSET (21)

#define RREG32(off) *(uint32_t *)(s->reg + (off))
#define WREG32(off, val) *(uint32_t *)(s->reg + (off)) = val
#define WREG32_OR(off, val) *(uint32_t *)(s->reg + (off)) |= val

static unsigned int frq_to_period_ns(unsigned int freq_hz)
{
    return NANOSECONDS_PER_SECOND > freq_hz ? NANOSECONDS_PER_SECOND / freq_hz :
                                              1;
}

static uint64_t G_GNUC_UNUSED tick_to_ns(PMUD2255State *s, uint64_t tick)
{
    return (tick >> 15) * NANOSECONDS_PER_SECOND +
           (tick & 0x7fff) * s->tick_period;
}

static uint64_t rtc_get_tick(PMUD2255State *s, uint64_t *out_ns)
{
    uint64_t now = qemu_clock_get_ns(rtc_clock);
    uint64_t offset = s->rtc_offset;
    if (out_ns) {
        *out_ns = now;
    }
    now -= offset;
    return ((now / NANOSECONDS_PER_SECOND) << 15) |
           ((now / s->tick_period) & 0x7fff);
}

static void pmu_d2255_set_tick_offset(PMUD2255State *s, uint64_t tick_offset)
{
    RREG32(REG_DIALOG_SCRATCH + OFF_DIALOG_SCRATCH_SECS_OFFSET) =
        tick_offset >> 15;
    WREG32(REG_DIALOG_SCRATCH + OFF_DIALOG_SCRATCH_TICKS_OFFSET + 0,
           tick_offset & 0xff);
    WREG32(REG_DIALOG_SCRATCH + OFF_DIALOG_SCRATCH_TICKS_OFFSET + 1,
           (tick_offset >> 8) & 0x7f);
}

static void pmu_d2255_update_irq(PMUD2255State *s)
{
    if (RREG32(REG_DIALOG_RTC_IRQ_MASK) & RREG32(REG_DIALOG_ALARM_EVENT)) {
        qemu_irq_raise(s->irq);
#ifdef DEBUG_PMU_D2255
        info_report("PMU D2255: raised IRQ");
#endif
    } else {
        qemu_irq_lower(s->irq);
#ifdef DEBUG_PMU_D2255
        info_report("PMU D2255: lowered IRQ");
#endif
    }
}

static void pmu_d2255_alarm(void *opaque)
{
    PMUD2255State *s;

    s = PMU_D2255(opaque);
    WREG32_OR(REG_DIALOG_ALARM_EVENT, DIALOG_RTC_EVENT_ALARM);
    pmu_d2255_update_irq(s);
}

static void pmu_d2255_set_alarm(PMUD2255State *s)
{
    uint32_t seconds =
        RREG32(REG_DIALOG_RTC_SUB_SECOND_A) - (rtc_get_tick(s, NULL) >> 15);
    if (RREG32(REG_DIALOG_RTC_CONTROL) & DIALOG_RTC_CONTROL_ALARM_EN) {
        if (seconds == 0) {
            timer_del(s->timer);
            pmu_d2255_alarm(s);
        } else {
            int64_t now = qemu_clock_get_ns(rtc_clock);
            timer_mod_ns(s->timer,
                         now + (int64_t)seconds * NANOSECONDS_PER_SECOND);
        }
    } else {
        timer_del(s->timer);
    }
}

static int pmu_d2255_event(I2CSlave *i2c, enum i2c_event event)
{
    PMUD2255State *s;

    s = PMU_D2255(i2c);

    switch (event) {
    case I2C_START_RECV:
        if (s->op_state != PMU_OP_STATE_NONE) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "PMU D2255: attempted to start transaction while a "
                          "transaction is already ongoing.\n");
            return -1;
        }

        s->op_state = PMU_OP_STATE_RECV;
#ifdef DEBUG_PMU_D2255
        info_report("PMU D2255: recv started.");
#endif
        return 0;
    case I2C_START_SEND:
        if (s->op_state != PMU_OP_STATE_NONE) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "PMU D2255: attempted to start transaction while a "
                          "transaction is already ongoing.\n");
            return -1;
        }

        s->op_state = PMU_OP_STATE_SEND;
        s->address = 0;
        s->address_state = PMU_ADDR_UPPER;
#ifdef DEBUG_PMU_D2255
        info_report("PMU D2255: send started.");
#endif
        return 0;
    case I2C_START_SEND_ASYNC:
#ifdef DEBUG_PMU_D2255
        info_report("PMU D2255: async is not supported.");
#endif
        return -1;
    case I2C_FINISH:
        s->op_state = PMU_OP_STATE_NONE;
#ifdef DEBUG_PMU_D2255
        info_report("PMU D2255: transaction end.");
#endif
        return 0;
    case I2C_NACK:
#ifdef DEBUG_PMU_D2255
        info_report("PMU D2255: transaction nack.");
#endif
        return -1;
    default:
        info_report("PMU D2255: TODO: DEFAULT CASE!!! what to return?");
        return -1;
    }
    return 0;
}

static uint8_t pmu_d2255_rx(I2CSlave *i2c)
{
    PMUD2255State *s;

    s = PMU_D2255(i2c);

    if (s->op_state != PMU_OP_STATE_RECV) {
        qemu_log_mask(
            LOG_GUEST_ERROR,
            "PMU D2255: RX attempted with but transaction is not recv.\n");
        return 0x00;
    }

    if (s->address_state != PMU_ADDR_RECEIVED) {
        qemu_log_mask(LOG_GUEST_ERROR, "PMU D2255: no address was sent.\n");
        return 0x00;
    }

    if (s->address + 1 > sizeof(s->reg)) {
        qemu_log_mask(LOG_GUEST_ERROR, "PMU D2255: 0x%X -> 0x%X is INVALID.\n",
                      s->address, s->reg[s->address]);
        return 0x00;
    }

    switch (s->address) {
    case REG_DIALOG_RTC_SUB_SECOND_A ... REG_DIALOG_RTC_SUB_SECOND_A + 6: {
        uint64_t now = rtc_get_tick(s, NULL);
        s->reg[REG_DIALOG_RTC_SUB_SECOND_A] = now << 1;
        s->reg[REG_DIALOG_RTC_SUB_SECOND_A + 1] = now >> 7;
        s->reg[REG_DIALOG_RTC_SUB_SECOND_A + 2] = now >> 15;
        s->reg[REG_DIALOG_RTC_SUB_SECOND_A + 3] = now >> 23;
        s->reg[REG_DIALOG_RTC_SUB_SECOND_A + 4] = now >> 31;
        s->reg[REG_DIALOG_RTC_SUB_SECOND_A + 5] = now >> 39;
    }
    default:
        break;
    }

#ifdef DEBUG_PMU_D2255
    info_report("PMU D2255: 0x%X -> 0x%X.", s->address, s->reg[s->address]);
#endif

    return s->reg[s->address++];
}

static int pmu_d2255_tx(I2CSlave *i2c, uint8_t data)
{
    PMUD2255State *s;

    s = PMU_D2255(i2c);

    if (s->op_state != PMU_OP_STATE_SEND) {
        qemu_log_mask(
            LOG_GUEST_ERROR,
            "PMU D2255: TX attempted with but transaction is not send.\n");
        return 0x00;
    }

    switch (s->address_state) {
    case PMU_ADDR_UPPER:
        s->address |= data << 8;
        s->address_state = PMU_ADDR_LOWER;
        break;
    case PMU_ADDR_LOWER:
        s->address |= data;
        s->address = le16_to_cpu(s->address);
        s->address_state = PMU_ADDR_RECEIVED;
#ifdef DEBUG_PMU_D2255
        info_report("PMU D2255: address set to 0x%X.", s->address);
#endif
        break;
    case PMU_ADDR_RECEIVED:
        if (s->op_state == PMU_OP_STATE_RECV) {
            qemu_log_mask(
                LOG_GUEST_ERROR,
                "PMU D2255: send transaction attempted but transaction "
                "is recv.\n");
            return -1;
        }

        if (s->address + 1 > sizeof(s->reg)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "PMU D2255: 0x%X <- 0x%X is INVALID.\n", s->address,
                          data);
            return -1;
        }

#ifdef DEBUG_PMU_D2255
        info_report("PMU D2255: 0x%X <- 0x%X.", s->address, data);
#endif
        s->reg[s->address++] = data;

        switch (s->address) {
        case REG_DIALOG_RTC_CONTROL:
            QEMU_FALLTHROUGH;
        case REG_DIALOG_RTC_SUB_SECOND_A ... REG_DIALOG_RTC_SUB_SECOND_A + 4:
            pmu_d2255_set_alarm(s);
            break;
        default:
            break;
        }
        break;
    }
    return 0;
}

static void pmu_d2255_reset(DeviceState *device)
{
    PMUD2255State *s;

    s = PMU_D2255(device);
    s->op_state = PMU_OP_STATE_NONE;
    s->address = 0;
    s->address_state = PMU_ADDR_UPPER;
    memset(s->reg, 0, sizeof(s->reg));
    memset(s->reg + REG_DIALOG_MASK_REV_CODE, 0xFF,
           REG_DIALOG_DEVICE_ID7 - REG_DIALOG_MASK_REV_CODE);
}

static void pmu_d2255_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *c = I2C_SLAVE_CLASS(klass);

    dc->desc = "Apple PMU D2255";
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    device_class_set_legacy_reset(dc, pmu_d2255_reset);

    c->event = pmu_d2255_event;
    c->recv = pmu_d2255_rx;
    c->send = pmu_d2255_tx;
}

static void pmu_d2255_instance_init(Object *obj)
{
    PMUD2255State *s;

    s = PMU_D2255(obj);

    s->tick_period = frq_to_period_ns(RTC_TICK_FREQ);
    pmu_d2255_set_tick_offset(s, rtc_get_tick(s, &s->rtc_offset));

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, pmu_d2255_alarm, s);

    qdev_init_gpio_out(DEVICE(s), &s->irq, 1);
}

static const TypeInfo pmu_d2255_type_info = {
    .name = TYPE_PMU_D2255,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(PMUD2255State),
    .instance_init = pmu_d2255_instance_init,
    .class_init = pmu_d2255_class_init,
};

static void pmu_d2255_register_types(void)
{
    type_register_static(&pmu_d2255_type_info);
}

type_init(pmu_d2255_register_types);
