/*
 * Apple SEP.
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

#ifndef HW_ARM_APPLE_SILICON_SEP_SIM_H
#define HW_ARM_APPLE_SILICON_SEP_SIM_H

#include "qemu/osdep.h"
#include "hw/arm/apple-silicon/dtb.h"
#include "hw/misc/apple-silicon/a7iop/core.h"
#include "hw/sysbus.h"
#include "qemu/typedefs.h"
#include "qom/object.h"

#define TYPE_APPLE_SEP_SIM "apple-sep-sim"
OBJECT_DECLARE_TYPE(AppleSEPSimState, AppleSEPSimClass, APPLE_SEP_SIM)

struct AppleSEPSimClass {
    /*< private >*/
    SysBusDeviceClass base_class;

    DeviceRealize parent_realize;
    ResettablePhases parent_phases;
};

#define SEP_ENDPOINT_MAX 0x20

typedef struct {
    uint8_t in_min_pages;
    uint8_t in_max_pages;
    uint8_t out_min_pages;
    uint8_t out_max_pages;
} QEMU_PACKED AppleSEPSimOOLInfo;

typedef struct {
    uint64_t in_addr;
    uint32_t in_size;
    uint64_t out_addr;
    uint32_t out_size;
} AppleSEPSimOOLState;

struct AppleSEPSimState {
    /*< private >*/
    AppleA7IOP parent_obj;

    MemoryRegion *dma_mr;
    AddressSpace *dma_as;
    QemuMutex lock;
    bool rsep;
    uint32_t status;
    AppleSEPSimOOLInfo ool_info[SEP_ENDPOINT_MAX];
    AppleSEPSimOOLState ool_state[SEP_ENDPOINT_MAX];
};

AppleSEPSimState *apple_sep_sim_create(DTBNode *node, bool modern);

#endif /* HW_ARM_APPLE_SILICON_SEP_SIM_H */
