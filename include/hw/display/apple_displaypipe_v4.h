/*
 * Apple Display Pipe V4 Controller.
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
#ifndef APPLE_DISPLAYPIPE_V4_H
#define APPLE_DISPLAYPIPE_V4_H

#include "qemu/osdep.h"
#include "hw/arm/apple-silicon/dtb.h"
#include "hw/sysbus.h"
#include "qom/object.h"
#include "ui/console.h"

#define TYPE_APPLE_DISPLAY_PIPE_V4 "apple-display-pipe-v4"
OBJECT_DECLARE_SIMPLE_TYPE(AppleDisplayPipeV4State, APPLE_DISPLAY_PIPE_V4);

#define ADP_V4_LAYER_COUNT (2)

typedef struct {
    size_t index;
    AddressSpace *dma_as;
    uint16_t disp_width;
    uint16_t disp_height;
    uint32_t config_control;
    uint32_t pixel_format;
    uint16_t width;
    uint16_t height;
    uint32_t base;
    uint32_t end;
    uint32_t stride;
    uint16_t buf_width;
    uint16_t buf_height;
    uint8_t *buf;
    bool dirty;
} ADPGenPipeState;

typedef struct {
    uint32_t layer_config[ADP_V4_LAYER_COUNT];
    bool dirty;
} ADPBlendUnitState;

struct AppleDisplayPipeV4State {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    QemuMutex lock;
    uint32_t width;
    uint32_t height;
    pixman_image_t *disp_image;
    MemoryRegion up_regs;
    MemoryRegion vram;
    MemoryRegion *dma_mr;
    AddressSpace dma_as;
    MemoryRegionSection vram_section;
    qemu_irq irqs[9];
    uint32_t int_status;
    ADPGenPipeState generic_pipe[ADP_V4_LAYER_COUNT];
    ADPBlendUnitState blend_unit;
    QemuConsole *console;
    bool invalidated;
};

AppleDisplayPipeV4State *adp_v4_create(DTBNode *node);

#endif /* APPLE_DISPLAYPIPE_V4_H */
