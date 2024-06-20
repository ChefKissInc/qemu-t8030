/*
 * Apple Display Pipe V2 Controller.
 *
 * Copyright (c) 2023-2024 Visual Ehrmanntraut (VisualEhrmanntraut).
 * Copyright (c) 2023 Christian Inci (chris-pcguy).
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
#include "hw/display/apple_displaypipe_v2.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "qemu/log.h"
#include "qom/object.h"
#include "sysemu/dma.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"
#include "framebuffer.h"

// #define DEBUG_DISP

#ifdef DEBUG_DISP
#define DISP_DBGLOG(fmt, ...) \
    qemu_log_mask(LOG_GUEST_ERROR, fmt "\n", __VA_ARGS__)
#else
#define DISP_DBGLOG(fmt, ...) \
    do {                      \
    } while (0);
#endif

#define REG_DISP_INT_FILTER 0x45818
#define REG_DISP_VER 0x46020
#define DISP_VER_A1 0x70045
#define REG_DISP_FRAME_SIZE 0x4603C

#define GP_BLOCK_BASE 0x50000
#define REG_GP_REG_SIZE 0x08000
#define REG_GP_CONFIG_CONTROL 0x00004
#define GP_CONFIG_CONTROL_RUN BIT(0)
#define GP_CONFIG_CONTROL_USE_DMA BIT(18)
#define GP_CONFIG_CONTROL_HDR BIT(24)
#define GP_CONFIG_CONTROL_ENABLED BIT(31)
#define REG_GP_PIXEL_FORMAT 0x0001C
#define GP_PIXEL_FORMAT_BGRA ((BIT(4) << 22) | BIT(24) | BIT(13))
#define GP_PIXEL_FORMAT_ARGB ((BIT(4) << 22) | BIT(24))
#define REG_GP_LAYER_0_START 0x00030
#define REG_GP_LAYER_1_START 0x00034
#define REG_GP_LAYER_0_END 0x00040
#define REG_GP_LAYER_1_END 0x00044
#define REG_GP_LAYER_0_STRIDE 0x00060
#define REG_GP_LAYER_1_STRIDE 0x00064
#define REG_GP_LAYER_0_SIZE 0x00070
#define REG_GP_LAYER_1_SIZE 0x00074
#define REG_GP_FRAME_SIZE 0x00080
#define REG_GP_CRC 0x00160
#define REG_GP_BANDWIDTH_RATE 0x00170

#define GP_BLOCK_BASE_FOR(i) (GP_BLOCK_BASE + i * REG_GP_REG_SIZE)
#define GP_BLOCK_END_FOR(i) (GP_BLOCK_BASE_FOR(i) + (REG_GP_REG_SIZE - 1))

static void apple_genpipev2_write(GenPipeState *s, hwaddr addr, uint64_t data)
{
    switch (addr - GP_BLOCK_BASE_FOR(s->index)) {
    case REG_GP_CONFIG_CONTROL: {
        DISP_DBGLOG("[GP%zu] Control <- 0x" HWADDR_FMT_plx, s->index, data);
        s->config_control = (uint32_t)data;
        break;
    }
    case REG_GP_PIXEL_FORMAT: {
        DISP_DBGLOG("[GP%zu] Pixel Format <- 0x" HWADDR_FMT_plx, s->index,
                    data);
        s->pixel_format = (uint32_t)data;
        break;
    }
    case REG_GP_LAYER_0_START: {
        DISP_DBGLOG("[GP%zu] Layer 0 Start <- 0x" HWADDR_FMT_plx, s->index,
                    data);
        s->layers[0].start = (uint32_t)data;
        break;
    }
    case REG_GP_LAYER_0_END: {
        DISP_DBGLOG("[GP%zu] Layer 0 End <- 0x" HWADDR_FMT_plx, s->index, data);
        s->layers[0].end = (uint32_t)data;
        break;
    }
    case REG_GP_LAYER_0_STRIDE: {
        s->layers[0].stride = (uint32_t)data;
        DISP_DBGLOG("[GP%zu] Layer 0 Stride <- 0x" HWADDR_FMT_plx, s->index,
                    data);
        break;
    }
    case REG_GP_LAYER_0_SIZE: {
        s->layers[0].size = (uint32_t)data;
        DISP_DBGLOG("[GP%zu] Layer 0 Size <- 0x" HWADDR_FMT_plx, s->index,
                    data);
        break;
    }
    case REG_GP_FRAME_SIZE: {
        DISP_DBGLOG("[GP%zu] Frame Size <- 0x" HWADDR_FMT_plx, s->index, data);
        s->height = data & 0xFFFF;
        s->width = (data >> 16) & 0xFFFF;
    }
    default: {
        DISP_DBGLOG("[GP%zu] Unknown write @ 0x" HWADDR_FMT_plx
                    " value: 0x" HWADDR_FMT_plx,
                    s->index, addr, data);
        break;
    }
    }
}

static uint32_t apple_genpipev2_read(GenPipeState *s, hwaddr addr)
{
    switch (addr - GP_BLOCK_BASE_FOR(s->index)) {
    case REG_GP_CONFIG_CONTROL: {
        DISP_DBGLOG("[GP%zu] Control -> 0x%x", s->index, s->config_control);
        return s->config_control;
    }
    case REG_GP_LAYER_0_START: {
        DISP_DBGLOG("[GP%zu] Layer 0 Start -> 0x%x", s->index,
                    s->layers[0].start);
        return s->layers[0].start;
    }
    case REG_GP_LAYER_0_END: {
        DISP_DBGLOG("[GP%zu] Layer 0 End -> 0x%x", s->index, s->layers[0].end);
        return s->layers[0].end;
    }
    case REG_GP_LAYER_0_STRIDE: {
        DISP_DBGLOG("[GP%zu] Layer 0 Stride -> 0x%x", s->index,
                    s->layers[0].stride);
        return s->layers[0].stride;
    }
    case REG_GP_PIXEL_FORMAT: {
        DISP_DBGLOG("[GP%zu] Pixel Format -> 0x%x", s->index, s->pixel_format);
        return s->pixel_format;
    }
    case REG_GP_FRAME_SIZE: {
        DISP_DBGLOG("[GP%zu] Frame Size -> 0x%x (width: %d height: %d)",
                    s->index, (s->width << 16) | s->height, s->width,
                    s->height);
        return (s->width << 16) | s->height;
    }
    default: {
        DISP_DBGLOG("[GP%zu] Unknown read @ 0x" HWADDR_FMT_plx, s->index, addr);
        return 0;
    }
    }
}

static uint8_t *apple_disp_gp_read_layer(GenPipeState *s, AddressSpace *dma_as,
                                         size_t *size_out)
{
    if (s->layers[0].start && s->layers[0].end && s->layers[0].stride &&
        s->layers[0].size) {
        size_t size = s->layers[0].end - s->layers[0].start;
        uint8_t *buf = g_malloc0(size);
        if (dma_memory_read(dma_as, s->layers[0].start, buf, size,
                            MEMTXATTRS_UNSPECIFIED) == MEMTX_OK) {
            *size_out = size;
            return buf;
        }
    }
    *size_out = 0;
    return NULL;
}

static bool apple_genpipev2_init(GenPipeState *s, size_t index, uint32_t width,
                                 uint32_t height)
{
    bzero(s, sizeof(*s));
    s->index = index;
    s->width = width;
    s->height = height;
    s->config_control = GP_CONFIG_CONTROL_ENABLED | GP_CONFIG_CONTROL_USE_DMA;
    s->pixel_format = GP_PIXEL_FORMAT_ARGB;
    return true;
}


static void apple_displaypipe_v2_write(void *opaque, hwaddr addr, uint64_t data,
                                       unsigned size)
{
    AppleDisplayPipeV2State *s = APPLE_DISPLAYPIPE_V2(opaque);
    if (addr >= 0x200000) {
        addr -= 0x200000;
    }
    switch (addr) {
    case GP_BLOCK_BASE_FOR(0)... GP_BLOCK_END_FOR(0):
        apple_genpipev2_write(&s->genpipes[0], addr, data);
        break;

    case GP_BLOCK_BASE_FOR(1)... GP_BLOCK_END_FOR(1):
        apple_genpipev2_write(&s->genpipes[1], addr, data);
        break;

    case REG_DISP_INT_FILTER:
        s->uppipe_int_filter &= ~(uint32_t)data;
        s->frame_processed = false;
        qemu_irq_lower(s->irqs[0]);
        break;

    default:
        DISP_DBGLOG("[disp] Unknown write @ 0x" HWADDR_FMT_plx
                    " value: 0x" HWADDR_FMT_plx,
                    addr, data);
        break;
    }
}

static uint64_t apple_displaypipe_v2_read(void *opaque, hwaddr addr,
                                          const unsigned size)
{
    AppleDisplayPipeV2State *s = APPLE_DISPLAYPIPE_V2(opaque);
    if (addr >= 0x200000) {
        addr -= 0x200000;
    }
    switch (addr) {
    case GP_BLOCK_BASE_FOR(0)... GP_BLOCK_END_FOR(0): {
        return apple_genpipev2_read(&s->genpipes[0], addr);
    }
    case GP_BLOCK_BASE_FOR(1)... GP_BLOCK_END_FOR(1): {
        return apple_genpipev2_read(&s->genpipes[1], addr);
    }
    case REG_DISP_VER: {
        return DISP_VER_A1;
    }
    case REG_DISP_FRAME_SIZE: {
        return (s->width << 16) | s->height;
    }
    case REG_DISP_INT_FILTER: {
        return s->uppipe_int_filter;
    }
    default:
        DISP_DBGLOG("[disp] Unknown read @ 0x" HWADDR_FMT_plx, addr);
        return 0;
    }
}

static const MemoryRegionOps apple_displaypipe_v2_reg_ops = {
    .write = apple_displaypipe_v2_write,
    .read = apple_displaypipe_v2_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned = false,
};

AppleDisplayPipeV2State *apple_displaypipe_v2_create(MachineState *machine,
                                                     DTBNode *node)
{
    DeviceState *dev;
    SysBusDevice *sbd;
    AppleDisplayPipeV2State *s;

    dev = qdev_new(TYPE_APPLE_DISPLAYPIPE_V2);
    sbd = SYS_BUS_DEVICE(dev);
    s = APPLE_DISPLAYPIPE_V2(sbd);

    assert(set_dtb_prop(node, "display-target", 15, "DisplayTarget5"));
    uint32_t dispTimingInfo[] = { 0x33C, 0x90, 0x1, 0x1, 0x700, 0x1, 0x1, 0x1 };
    assert(set_dtb_prop(node, "display-timing-info", sizeof(dispTimingInfo),
                        &dispTimingInfo));
    uint32_t data = 0xD;
    assert(set_dtb_prop(node, "bics-param-set", sizeof(data), &data));
    uint32_t dot_pitch = 326;
    assert(set_dtb_prop(node, "dot-pitch", sizeof(dot_pitch), &dot_pitch));
    assert(set_dtb_prop(node, "function-brightness_update", 0, ""));

    DTBProp *prop = find_dtb_prop(node, "reg");
    assert(prop);
    uint64_t *reg = (uint64_t *)prop->value;
    memory_region_init_io(&s->up_regs, OBJECT(sbd),
                          &apple_displaypipe_v2_reg_ops, sbd, "up.regs",
                          reg[1]);
    sysbus_init_mmio(sbd, &s->up_regs);
    object_property_add_const_link(OBJECT(sbd), "up.regs", OBJECT(&s->up_regs));

    return s;
}

static void apple_displaypipe_v2_draw_row(void *opaque, uint8_t *dest,
                                          const uint8_t *src, int width,
                                          int dest_pitch)
{
    while (width--) {
        uint32_t colour = ldl_le_p(src);
        src += sizeof(colour);
        memcpy(dest, &colour, sizeof(colour));
        dest += sizeof(colour);
    }
}

static void apple_displaypipe_v2_gfx_update(void *opaque)
{
    AppleDisplayPipeV2State *s = APPLE_DISPLAYPIPE_V2(opaque);
    DisplaySurface *surface = qemu_console_surface(s->console);

    if ((!s->genpipes[0].layers[0].start || !s->genpipes[0].layers[0].end) &&
        (!s->genpipes[1].layers[0].start || !s->genpipes[1].layers[0].end)) {
        int stride = s->width * sizeof(uint32_t);
        int first = 0, last = 0;

        if (!s->vram_section.mr) {
            framebuffer_update_memory_section(&s->vram_section, &s->vram, 0,
                                              s->height, stride);
        }
        framebuffer_update_display(
            surface, &s->vram_section, s->width, s->height, stride, stride, 0,
            0, apple_displaypipe_v2_draw_row, s, &first, &last);
        if (first >= 0) {
            dpy_gfx_update(s->console, 0, first, s->width, last - first + 1);
        }

        return;
    }

    if (!s->frame_processed) {
        uint8_t *dest = surface_data(surface);

        size_t size = 0;
        uint8_t *buf =
            apple_disp_gp_read_layer(&s->genpipes[0], &s->dma_as, &size);
        if (size && buf != NULL) {
            size_t height = size / s->genpipes[0].layers[0].stride;
            for (size_t y = 0; y < height; y++) {
                memcpy(dest + (y * (s->width * sizeof(uint32_t))),
                       buf + (y * s->genpipes[0].layers[0].stride),
                       s->genpipes[0].layers[0].stride);
            }
            g_free(buf);
        }

        buf = apple_disp_gp_read_layer(&s->genpipes[1], &s->dma_as, &size);
        if (size && buf != NULL) {
            size_t height = size / s->genpipes[1].layers[0].stride;
            for (size_t y = 0; y < height; y++) {
                memcpy(dest + (y * (s->width * sizeof(uint32_t))),
                       buf + (y * s->genpipes[1].layers[0].stride),
                       s->genpipes[1].layers[0].stride);
            }
            g_free(buf);
        }

        dpy_gfx_update_full(s->console);
        s->uppipe_int_filter |= (1UL << 10) | (1UL << 20);
        qemu_irq_raise(s->irqs[0]);
        s->frame_processed = true;
    }
}

static const GraphicHwOps apple_displaypipe_v2_ops = {
    .gfx_update = apple_displaypipe_v2_gfx_update,
};

static void apple_displaypipe_v2_reset(DeviceState *dev)
{
    AppleDisplayPipeV2State *s = APPLE_DISPLAYPIPE_V2(dev);

    qemu_irq_lower(s->irqs[0]);
    s->uppipe_int_filter = 0;
    s->frame_processed = false;
    apple_genpipev2_init(&s->genpipes[0], 0, s->width, s->height);
    apple_genpipev2_init(&s->genpipes[1], 1, s->width, s->height);
}

static void apple_displaypipe_v2_realize(DeviceState *dev, Error **errp)
{
    AppleDisplayPipeV2State *s = APPLE_DISPLAYPIPE_V2(dev);

    s->console = graphic_console_init(dev, 0, &apple_displaypipe_v2_ops, s);
    qemu_console_resize(s->console, s->width, s->height);
    apple_displaypipe_v2_reset(dev);
}

static Property apple_displaypipe_v2_props[] = {
    // iPhone 4/4S
    DEFINE_PROP_UINT32("width", AppleDisplayPipeV2State, width, 640),
    DEFINE_PROP_UINT32("height", AppleDisplayPipeV2State, height, 960),
    // iPhone 11
    // DEFINE_PROP_UINT32("width", AppleDisplayPipeV2State, width, 828),
    // DEFINE_PROP_UINT32("height", AppleDisplayPipeV2State, height, 1792),
    DEFINE_PROP_END_OF_LIST()
};

static void apple_displaypipe_v2_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    device_class_set_props(dc, apple_displaypipe_v2_props);
    dc->realize = apple_displaypipe_v2_realize;
    dc->reset = apple_displaypipe_v2_reset;
}

static const TypeInfo apple_displaypipe_v2_type_info = {
    .name = TYPE_APPLE_DISPLAYPIPE_V2,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AppleDisplayPipeV2State),
    .class_init = apple_displaypipe_v2_class_init,
};

static void apple_displaypipe_v2_register_types(void)
{
    type_register_static(&apple_displaypipe_v2_type_info);
}

type_init(apple_displaypipe_v2_register_types);
