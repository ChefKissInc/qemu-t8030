/*
 * Apple Display Back End V2 Controller.
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

#ifndef HW_DISPLAY_ADBE_V2_H
#define HW_DISPLAY_ADBE_V2_H

#include "qemu/osdep.h"
#include "hw/arm/apple-silicon/dtb.h"
#include "hw/sysbus.h"
#include "qom/object.h"
#include "ui/console.h"

#define TYPE_ADBE_V2 "adbe-v2"
OBJECT_DECLARE_SIMPLE_TYPE(ADBEV2, ADBE_V2);

typedef struct {
    uint32_t vftg_ctl;
    uint32_t const_colour;
} DisplayBackEndState;

struct ADBEV2 {
    /*< private >*/
    SysBusDevice parent_obj;

    uint32_t width, height;
    MemoryRegion backend_regs, vram;
    MemoryRegion *dma_mr;
    AddressSpace dma_as;
    MemoryRegionSection vram_section;
    qemu_irq irqs[9];

    DisplayBackEndState dbe_state;
    QemuConsole *console;
};

ADBEV2 *adbe_v2_create(DTBNode *node);

#endif /* HW_DISPLAY_ADBE_V2_H */
