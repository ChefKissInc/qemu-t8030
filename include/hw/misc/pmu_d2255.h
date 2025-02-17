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

#ifndef PMU_D2255_H
#define PMU_D2255_H

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "qom/object.h"

#define TYPE_PMU_D2255 "pmu-d2255"
OBJECT_DECLARE_SIMPLE_TYPE(PMUD2255State, PMU_D2255);

enum PMUOpState {
    PMU_OP_STATE_NONE,
    PMU_OP_STATE_RECV,
    PMU_OP_STATE_SEND,
};

enum PMUAddrState {
    PMU_ADDR_UPPER,
    PMU_ADDR_LOWER,
    PMU_ADDR_RECEIVED,
};

struct PMUD2255State {
    /*< private >*/
    I2CSlave i2c;

    uint8_t reg[0x8800];
    QEMUTimer *timer;
    qemu_irq irq;
    uint32_t tick_period;
    uint64_t rtc_offset;
    enum PMUOpState op_state;
    uint16_t address;
    enum PMUAddrState address_state;
};

#endif
