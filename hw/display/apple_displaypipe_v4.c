/*
 * Apple Display Pipe V4 Controller.
 *
 * Copyright (c) 2023-2025 Visual Ehrmanntraut (VisualEhrmanntraut).
 * Copyright (c) 2023-2025 Christian Inci (chris-pcguy).
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
#include "exec/memory.h"
#include "hw/display/apple_displaypipe_v4.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/resettable.h"
#include "qemu/error-report.h"
#include "qom/object.h"
#include "sysemu/dma.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"
#include "framebuffer.h"
#include "pixman.h"

// #define DEBUG_DISP

#ifdef DEBUG_DISP
#define ADP_INFO(fmt, ...) info_report(fmt, __VA_ARGS__)
#else
#define ADP_INFO(fmt, ...) \
    do {                   \
    } while (0);
#endif

/**
 * Block Bases (DisplayTarget5)
 * 0x08000  |  M3 Control Mailbox
 * 0x0A000  |  M3 Video Mode Mailbox
 * 0x40000  |  Control
 * 0x48000  |  Vertical Frame Timing Generator
 * 0x50000  |  Generic Pixel Pipe 0
 * 0x58000  |  Generic Pixel Pipe 1
 * 0x60000  |  Blend Unit
 * 0x70000  |  White Point Correction
 * 0x7C000  |  Panel Response Correction
 * 0x80000  |  Dither
 * 0x82000  |  Dither: Enchanced ST Dither 0
 * 0x83000  |  Dither: Enchanced ST Dither 1
 * 0x84000  |  Content-Dependent Frame Duration
 * 0x88000  |  SPLR (Sub-Pixel Layout R?)
 * 0x90000  |  Burn-In Compensation Sampler
 * 0x98000  |  SPUC
 * 0xA0000  |  PDC (Panel D? Correction?)
 * 0xB0000  |  PCC (Pixel Color Correction?)
 * 0xD0000  |  PCC Mailbox
 * 0xF0000  |  DBM (Dynamic Backlight Modulation?)
 */

#define REG_CONTROL_INT_STATUS (0x45818)
#define CONTROL_INT_STATUS_MODE_CHANGED BIT(1)
#define CONTROL_INT_STATUS_DISP_UNDERRUN BIT(3)
#define CONTROL_INT_STATUS_VBLANK BIT(10) // "Swap Done"
#define CONTROL_INT_STATUS_SUB_FRAME_OVERFLOW BIT(11)
#define CONTROL_INT_STATUS_M3 BIT(13)
#define CONTROL_INT_STATUS_PCC BIT(17)
// #define CONTROL_INT_STATUS_?? BIT(19) // "Start accumulating"
// #define CONTROL_INT_STATUS_?? BIT(20) // "Frame Processed"
#define CONTROL_INT_STATUS_AXI_READ_ERR BIT(30)
#define CONTROL_INT_STATUS_AXI_WRITE_ERR BIT(31)
#define REG_CONTROL_VERSION (0x46020)
#define CONTROL_VERSION_A0 (0x70044)
#define CONTROL_VERSION_A1 (0x70045)
#define REG_CONTROL_FRAME_SIZE (0x4603C)
#define REG_CONTROL_CONFIG (0x46040)
#define REG_CONTROL_OUT_FIFO_CLK_GATE (0x46074)
#define REG_CONTROL_OUT_FIFO_DEPTH (0x46084)
#define REG_CONTROL_COMPRESSION_CFG (0x460E0)
#define REG_CONTROL_BACKPRESSURE (0x46120)
#define REG_CONTROL_POWER_GATE_CTRL (0x46158)
#define REG_CONTROL_BIS_UPDATE_INTERVAL (0x46198)
#define REG_CONTROL_MIN_BANDWIDTH_RATE (0x461C0)
#define REG_CONTROL_BANDWIDTH_RATE_SCALE_FACTOR (0x461C4)
#define REG_CONTROL_PIO_DMA_BANDWIDTH_RATE (0x461C8)
#define REG_CONTROL_REPLAY_DMA_BANDWIDTH_RATE (0x461CC)
#define REG_CONTROL_GATE_CONTROL (0x461D0)
#define REG_CONTROL_READ_LINK_GATE_METRIC (0x461D4)
#define REG_CONTROL_READ_LTR_CONFIG (0x461D8)
#define REG_CONTROL_LTR_TIMER (0x461DC)
#define REG_CONTROL_WRITE_LTR_CONFIG (0x461E0)

#define GP_BLOCK_BASE (0x50000)
#define GP_BLOCK_SIZE (0x8000)
#define REG_GP_CONFIG_CONTROL (0x4)
#define GP_CONFIG_CONTROL_RUN BIT(0)
#define GP_CONFIG_CONTROL_USE_DMA BIT(18)
#define GP_CONFIG_CONTROL_HDR BIT(24)
#define GP_CONFIG_CONTROL_ENABLED BIT(31)
#define REG_GP_PIXEL_FORMAT (0x0001C)
#define GP_PIXEL_FORMAT_BGRA ((BIT(4) << 22) | BIT(24) | (3 << 13))
#define GP_PIXEL_FORMAT_ARGB ((BIT(4) << 22) | BIT(24))
#define GP_PIXEL_FORMAT_COMPRESSED BIT(30)
#define REG_GP_BASE (0x30)
#define REG_GP_END (0x40)
#define REG_GP_STRIDE (0x60)
#define REG_GP_SIZE (0x70)
#define REG_GP_FRAME_SIZE (0x80)
#define REG_GP_CRC_DATA (0x160)
#define REG_GP_BANDWIDTH_RATE (0x170)
#define REG_GP_STATUS (0x184)
#define GP_STATUS_DECOMPRESSION_FAIL BIT(0)

#define GP_BLOCK_BASE_FOR(i) (GP_BLOCK_BASE + i * GP_BLOCK_SIZE)
#define GP_BLOCK_END_FOR(i) (GP_BLOCK_BASE_FOR(i) + (GP_BLOCK_SIZE - 1))

#define BLEND_BLOCK_BASE (0x60000)
#define BLEND_BLOCK_SIZE (0x8000)
#define REG_BLEND_CONFIG (0x4)
#define REG_BLEND_BG (0x8)
#define REG_BLEND_LAYER_0_BG (0xC)
#define REG_BLEND_LAYER_1_BG (0x10)
#define REG_BLEND_LAYER_0_CONFIG (0x14)
#define REG_BLEND_LAYER_1_CONFIG (0x18)
#define BLEND_LAYER_CONFIG_PIPE(v) ((v) & 0xF)
#define BLEND_LAYER_CONFIG_MODE(v) ((v >> 4) & 0xF)
#define BLEND_MODE_NONE 0
#define BLEND_MODE_ALPHA 1
#define BLEND_MODE_PREMULT 2
#define BLEND_MODE_BYPASS 3
#define REG_BLEND_GAMMA_TABLE_R (0x1C)
#define REG_BLEND_GAMMA_TABLE_G (0x1024)
#define REG_BLEND_GAMMA_TABLE_B (0x202C)
// #define REG_BLEND_?? (0x3034)

static void adp_update_irqs(AppleDisplayPipeV4State *s)
{
    qemu_set_irq(s->irqs[0], (s->int_status & CONTROL_INT_STATUS_VBLANK) != 0);
}

static pixman_format_code_t adp_gp_fmt_to_pixman(ADPGenPipeState *s)
{
    if ((s->pixel_format & GP_PIXEL_FORMAT_BGRA) == GP_PIXEL_FORMAT_BGRA) {
        ADP_INFO("[gp%zu] Pixel Format is BGRA (0x%X).", s->index,
                 s->pixel_format);
        return PIXMAN_b8g8r8a8;
    } else if ((s->pixel_format & GP_PIXEL_FORMAT_ARGB) ==
               GP_PIXEL_FORMAT_ARGB) {
        ADP_INFO("[gp%zu] Pixel Format is ARGB (0x%X).", s->index,
                 s->pixel_format);
        return PIXMAN_a8r8g8b8;
    } else {
        error_report("[gp%zu] Pixel Format is unknown (0x%X).", s->index,
                     s->pixel_format);
        return 0;
    }
}

static uint8_t *adp_gp_read(ADPGenPipeState *s)
{
    uint8_t *buf;

    // TODO: Decompress the data and display it properly.
    if (s->pixel_format & GP_PIXEL_FORMAT_COMPRESSED) {
        error_report("[gp%zu] Dropping frame as it's compressed.", s->index);
        return NULL;
    }

    ADP_INFO("[gp%zu] Width and height is %dx%d.", s->index, s->buf_width,
             s->buf_height);
    ADP_INFO("[gp%zu] Stride is %d.", s->index, s->stride);

    if (s->buf_height == 0 || s->buf_width == 0 || s->stride == 0) {
        error_report(
            "[gp%zu] Dropping frame as width, height or stride is zero.",
            s->index);
        return NULL;
    }

    if (s->buf_width > s->disp_width || s->buf_height > s->disp_height) {
        error_report("[gp%zu] Dropping frame as it's larger than the screen.",
                     s->index);
        return NULL;
    }


    buf = g_malloc(s->buf_height * s->buf_width * s->stride);
    if (dma_memory_read(s->dma_as, s->base, buf, s->end - s->base,
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        error_report("[gp%zu] Failed to read from DMA.", s->index);
        g_free(buf);
        return NULL;
    }
    return buf;
}

static void adp_gp_reg_write(ADPGenPipeState *s, hwaddr addr, uint64_t data)
{
    switch (addr) {
    case REG_GP_CONFIG_CONTROL: {
        ADP_INFO("[gp%zu] Control <- 0x" HWADDR_FMT_plx, s->index, data);
        s->config_control = (uint32_t)data;
        if (s->config_control & GP_CONFIG_CONTROL_RUN) {
            g_free(s->buf);
            s->buf = adp_gp_read(s);
            s->dirty = true;
        }
        break;
    }
    case REG_GP_PIXEL_FORMAT: {
        ADP_INFO("[gp%zu] Pixel format <- 0x" HWADDR_FMT_plx, s->index, data);
        s->pixel_format = (uint32_t)data;
        break;
    }
    case REG_GP_BASE: {
        ADP_INFO("[gp%zu] Base <- 0x" HWADDR_FMT_plx, s->index, data);
        s->base = (uint32_t)data;
        break;
    }
    case REG_GP_END: {
        ADP_INFO("[gp%zu] End <- 0x" HWADDR_FMT_plx, s->index, data);
        s->end = (uint32_t)data;
        break;
    }
    case REG_GP_STRIDE: {
        ADP_INFO("[gp%zu] Stride <- 0x" HWADDR_FMT_plx, s->index, data);
        s->stride = (uint32_t)data;
        break;
    }
    case REG_GP_SIZE: {
        ADP_INFO("[gp%zu] Size <- 0x" HWADDR_FMT_plx, s->index, data);
        s->buf_height = data & 0xFFFF;
        s->buf_width = (data >> 16) & 0xFFFF;
        break;
    }
    case REG_GP_FRAME_SIZE: {
        ADP_INFO("[gp%zu] Frame Size <- 0x" HWADDR_FMT_plx, s->index, data);
        s->height = data & 0xFFFF;
        s->width = (data >> 16) & 0xFFFF;
        break;
    }
    default: {
        ADP_INFO("[gp%zu] Unknown @ 0x" HWADDR_FMT_plx " <- 0x" HWADDR_FMT_plx,
                 s->index, addr, data);
        break;
    }
    }
}

static uint32_t adp_gp_reg_read(ADPGenPipeState *s, hwaddr addr)
{
    switch (addr) {
    case REG_GP_CONFIG_CONTROL: {
        ADP_INFO("[gp%zu] Control -> 0x%x", s->index, s->config_control);
        return s->config_control;
    }
    case REG_GP_PIXEL_FORMAT: {
        ADP_INFO("[gp%zu] Pixel format -> 0x%x", s->index, s->pixel_format);
        return s->pixel_format;
    }
    case REG_GP_BASE: {
        ADP_INFO("[gp%zu] Base -> 0x%x", s->index, s->base);
        return s->base;
    }
    case REG_GP_END: {
        ADP_INFO("[gp%zu] End -> 0x%x", s->index, s->end);
        return s->end;
    }
    case REG_GP_STRIDE: {
        ADP_INFO("[gp%zu] Stride -> 0x%x", s->index, s->stride);
        return s->stride;
    }
    case REG_GP_SIZE: {
        ADP_INFO("[gp%zu] Size -> 0x%x", s->index,
                 (s->buf_width << 16) | s->buf_height);
        return (s->buf_width << 16) | s->buf_height;
    }
    case REG_GP_FRAME_SIZE: {
        ADP_INFO("[gp%zu] Frame Size -> 0x%x (width: %d height: %d)", s->index,
                 (s->width << 16) | s->height, s->width, s->height);
        return (s->width << 16) | s->height;
    }
    default: {
        ADP_INFO("[gp%zu] Unknown @ 0x" HWADDR_FMT_plx " -> 0x" HWADDR_FMT_plx,
                 s->index, addr, (hwaddr)0);
        return 0;
    }
    }
}

static void adp_gp_reset(ADPGenPipeState *s, size_t index, AddressSpace *dma_as,
                         uint16_t disp_width, uint16_t disp_height)
{
    if (s->buf != NULL) {
        g_free(s->buf);
    }
    memset(s, 0, sizeof(*s));
    s->index = index;
    s->dma_as = dma_as;
    s->disp_width = disp_width;
    s->disp_height = disp_height;
    s->dirty = true;
}

static void adp_blend_reg_write(ADPBlendUnitState *s, uint64_t addr,
                                uint64_t data)
{
    switch (addr) {
    case REG_BLEND_LAYER_0_CONFIG: {
        ADP_INFO("[blend] Layer 0 Config <- 0x" HWADDR_FMT_plx, data);
        s->layer_config[0] = (uint32_t)data;
        s->dirty = true;
        break;
    }
    case REG_BLEND_LAYER_1_CONFIG: {
        s->layer_config[1] = (uint32_t)data;
        s->dirty = true;
        ADP_INFO("[blend] Layer 1 Config <- 0x" HWADDR_FMT_plx, data);
        break;
    }
    default: {
        ADP_INFO("[blend] Unknown @ 0x" HWADDR_FMT_plx " <- 0x" HWADDR_FMT_plx,
                 addr, data);
        break;
    }
    }
}

static uint64_t adp_blend_reg_read(ADPBlendUnitState *s, uint64_t addr)
{
    switch (addr) {
    case REG_BLEND_LAYER_0_CONFIG: {
        ADP_INFO("[blend] Layer 0 Config -> 0x%X", s->layer_config[0]);
        return s->layer_config[0];
    }
    case REG_BLEND_LAYER_1_CONFIG: {
        ADP_INFO("[blend] Layer 1 Config -> 0x%X", s->layer_config[1]);
        return s->layer_config[1];
    }
    default: {
        ADP_INFO("[blend] Unknown @ 0x" HWADDR_FMT_plx " -> 0x" HWADDR_FMT_plx,
                 addr, (hwaddr)0);
        return 0;
    }
    }
}

static void adp_blend_reset(ADPBlendUnitState *s)
{
    memset(s, 0, sizeof(*s));
}

static void adp_reg_write(void *opaque, hwaddr addr, uint64_t data,
                          unsigned size)
{
    AppleDisplayPipeV4State *s;

    s = APPLE_DISPLAY_PIPE_V4(opaque);

    QEMU_LOCK_GUARD(&s->lock);

    if (addr >= 0x200000) {
        addr -= 0x200000;
    }

    switch (addr) {
    case REG_CONTROL_INT_STATUS: {
        s->int_status &= ~(uint32_t)data;
        adp_update_irqs(s);
        break;
    }
    case GP_BLOCK_BASE_FOR(0)... GP_BLOCK_END_FOR(0): {
        adp_gp_reg_write(&s->generic_pipe[0], addr - GP_BLOCK_BASE_FOR(0),
                         data);
        break;
    }
    case GP_BLOCK_BASE_FOR(1)... GP_BLOCK_END_FOR(1): {
        adp_gp_reg_write(&s->generic_pipe[1], addr - GP_BLOCK_BASE_FOR(1),
                         data);
        break;
    }
    case BLEND_BLOCK_BASE ...(BLEND_BLOCK_BASE + BLEND_BLOCK_SIZE): {
        adp_blend_reg_write(&s->blend_unit, addr - BLEND_BLOCK_BASE, data);
        break;
    }
    default: {
        ADP_INFO("[disp] Unknown @ 0x" HWADDR_FMT_plx " <- 0x" HWADDR_FMT_plx,
                 addr, data);
        break;
    }
    }
}

static uint64_t adp_reg_read(void *opaque, hwaddr addr, const unsigned size)
{
    AppleDisplayPipeV4State *s;

    s = APPLE_DISPLAY_PIPE_V4(opaque);

    QEMU_LOCK_GUARD(&s->lock);

    if (addr >= 0x200000) {
        addr -= 0x200000;
    }

    switch (addr) {
    case REG_CONTROL_VERSION: {
        ADP_INFO("[disp] Version -> 0x%x", CONTROL_VERSION_A0);
        return CONTROL_VERSION_A0;
    }
    case REG_CONTROL_FRAME_SIZE: {
        ADP_INFO("[disp] Frame Size -> 0x%x", (s->width << 16) | s->height);
        return (s->width << 16) | s->height;
    }
    case REG_CONTROL_INT_STATUS: {
        ADP_INFO("[disp] Int Status -> 0x%x", s->int_status);
        return s->int_status;
    }
    case GP_BLOCK_BASE_FOR(0)... GP_BLOCK_END_FOR(0): {
        return adp_gp_reg_read(&s->generic_pipe[0],
                               addr - GP_BLOCK_BASE_FOR(0));
    }
    case GP_BLOCK_BASE_FOR(1)... GP_BLOCK_END_FOR(1): {
        return adp_gp_reg_read(&s->generic_pipe[1],
                               addr - GP_BLOCK_BASE_FOR(1));
    }
    case BLEND_BLOCK_BASE ...(BLEND_BLOCK_BASE + BLEND_BLOCK_SIZE): {
        return adp_blend_reg_read(&s->blend_unit, addr - BLEND_BLOCK_BASE);
    }
    default: {
        ADP_INFO("[disp] Unknown @ 0x" HWADDR_FMT_plx " -> 0x" HWADDR_FMT_plx,
                 addr, (hwaddr)0);
        return 0;
    }
    }
}

static const MemoryRegionOps adp_reg_ops = {
    .write = adp_reg_write,
    .read = adp_reg_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned = false,
};

static void adp_draw_row(void *opaque, uint8_t *dest, const uint8_t *src,
                         int width, int dest_pitch)
{
    while (width--) {
        uint32_t colour = ldl_le_p(src);
        memcpy(dest, &colour, sizeof(colour));
        src += sizeof(colour);
        dest += sizeof(colour);
    }
}

static void adp_v4_invalidate(void *opaque)
{
    AppleDisplayPipeV4State *s = APPLE_DISPLAY_PIPE_V4(opaque);

    QEMU_LOCK_GUARD(&s->lock);

    s->invalidated = true;
}

static void adp_v4_blit_rect_black(AppleDisplayPipeV4State *s, uint16_t width,
                                   uint16_t height)
{
    size_t y;
    hwaddr off;

    for (y = 0; y < height; y += 1) {
        off = y * s->width * sizeof(uint32_t);
        memset(memory_region_get_ram_ptr(&s->vram) + off, 0,
               s->width * sizeof(uint32_t));
        memory_region_set_dirty(&s->vram, off, width * sizeof(uint32_t));
    }
}

// TODO: Where is the destination X and Y?
static void adp_v4_update_disp_image(AppleDisplayPipeV4State *s)
{
    ADPGenPipeState *layer_0_pipe;
    ADPGenPipeState *layer_1_pipe;
    uint8_t layer_0_blend_mode;
    uint8_t layer_1_blend_mode;
    size_t i;
    hwaddr off;
    pixman_format_code_t layer_0_fmt;
    pixman_format_code_t layer_1_fmt;
    pixman_image_t *layer_0_image;
    pixman_image_t *layer_1_image;

    QEMU_LOCK_GUARD(&s->lock);

    if (!s->blend_unit.dirty && !s->generic_pipe[0].dirty &&
        !s->generic_pipe[1].dirty) {
        return;
    }

    layer_0_pipe = &s->generic_pipe[BLEND_LAYER_CONFIG_PIPE(
        s->blend_unit.layer_config[0])];
    layer_1_pipe = &s->generic_pipe[BLEND_LAYER_CONFIG_PIPE(
        s->blend_unit.layer_config[1])];
    layer_0_blend_mode = BLEND_LAYER_CONFIG_MODE(s->blend_unit.layer_config[0]);
    layer_1_blend_mode = BLEND_LAYER_CONFIG_MODE(s->blend_unit.layer_config[1]);

    // Display is empty.
    if ((layer_0_blend_mode == BLEND_MODE_NONE &&
         layer_1_blend_mode == BLEND_MODE_NONE) ||
        ((layer_0_pipe->width == 0 || layer_0_pipe->height == 0) &&
         (layer_1_pipe->width == 0 || layer_1_pipe->height == 0))) {
        adp_v4_blit_rect_black(s, s->width, s->height);
        return;
    }

    if (layer_1_blend_mode == BLEND_MODE_BYPASS ||
        (layer_1_blend_mode != BLEND_MODE_NONE &&
         layer_0_blend_mode == BLEND_MODE_NONE)) {
        if (layer_1_pipe->base == 0 || layer_1_pipe->end == 0) {
            adp_v4_blit_rect_black(s, layer_1_pipe->width,
                                   layer_1_pipe->height);
            layer_1_pipe->dirty = false;
        } else {
            g_assert_nonnull(layer_1_pipe->buf);
            for (i = 0; i < layer_1_pipe->buf_height; i += 1) {
                off = i * s->width * sizeof(uint32_t);
                memcpy(memory_region_get_ram_ptr(&s->vram) + off,
                       layer_1_pipe->buf + i * layer_1_pipe->stride,
                       layer_1_pipe->buf_width * sizeof(uint32_t));
                memory_region_set_dirty(
                    &s->vram, off, layer_1_pipe->buf_width * sizeof(uint32_t));
            }
        }
    } else if (layer_0_blend_mode == BLEND_MODE_BYPASS ||
               (layer_0_blend_mode != BLEND_MODE_NONE &&
                layer_1_blend_mode == BLEND_MODE_NONE)) {
        if (layer_0_pipe->base == 0 || layer_0_pipe->end == 0) {
            adp_v4_blit_rect_black(s, layer_0_pipe->width,
                                   layer_0_pipe->height);
        } else {
            g_assert_nonnull(layer_0_pipe->buf);
            for (i = 0; i < layer_0_pipe->buf_height; i += 1) {
                off = i * s->width * sizeof(uint32_t);
                memcpy(memory_region_get_ram_ptr(&s->vram) + off,
                       layer_0_pipe->buf + i * layer_0_pipe->stride,
                       layer_0_pipe->buf_width * sizeof(uint32_t));
                memory_region_set_dirty(
                    &s->vram, off, layer_0_pipe->buf_width * sizeof(uint32_t));
            }
        }
        layer_0_pipe->dirty = false;
    } else {
        g_assert(layer_0_pipe != layer_1_pipe);

        g_assert_nonnull(layer_0_pipe->buf);
        layer_0_fmt = adp_gp_fmt_to_pixman(layer_0_pipe);
        g_assert_cmphex(layer_0_fmt, !=, 0);
        layer_0_image = pixman_image_create_bits(
            layer_0_fmt, layer_0_pipe->buf_width, layer_0_pipe->buf_height,
            (uint32_t *)layer_0_pipe->buf, layer_0_pipe->stride);
        g_assert_nonnull(layer_0_image);

        g_assert_nonnull(layer_1_pipe->buf);
        layer_1_fmt = adp_gp_fmt_to_pixman(layer_1_pipe);
        g_assert_cmphex(layer_1_fmt, !=, 0);
        layer_1_image = pixman_image_create_bits(
            layer_1_fmt, layer_1_pipe->buf_width, layer_1_pipe->buf_height,
            (uint32_t *)layer_1_pipe->buf, layer_1_pipe->stride);
        g_assert_nonnull(layer_1_image);

        adp_v4_blit_rect_black(s, s->width, s->height);

        pixman_image_composite(PIXMAN_OP_OVER, layer_0_image, NULL,
                               s->disp_image, 0, 0, 0, 0, 0, 0,
                               layer_0_pipe->width, layer_0_pipe->height);
        pixman_image_composite(PIXMAN_OP_OVER, layer_1_image, NULL,
                               s->disp_image, 0, 0, 0, 0, 0, 0,
                               layer_1_pipe->width, layer_1_pipe->height);

        layer_0_pipe->dirty = false;
        layer_1_pipe->dirty = false;
        pixman_image_unref(layer_0_image);
        pixman_image_unref(layer_1_image);

        memory_region_set_dirty(&s->vram, 0,
                                s->height * s->width * sizeof(uint32_t));
    }

    s->blend_unit.dirty = false;
}

static void adp_v4_gfx_update(void *opaque)
{
    AppleDisplayPipeV4State *s = APPLE_DISPLAY_PIPE_V4(opaque);
    DisplaySurface *surface = qemu_console_surface(s->console);

    int stride = s->width * sizeof(uint32_t);
    int first = 0, last = 0;

    adp_v4_update_disp_image(s);

    if (s->invalidated) {
        framebuffer_update_memory_section(&s->vram_section, &s->vram, 0,
                                          s->height, stride);
        s->invalidated = false;
    }

    framebuffer_update_display(surface, &s->vram_section, s->width, s->height,
                               stride, stride, 0, 0, adp_draw_row, s, &first,
                               &last);
    if (first >= 0) {
        dpy_gfx_update(s->console, 0, first, s->width, last - first + 1);
    }

    s->int_status |= CONTROL_INT_STATUS_VBLANK;
    adp_update_irqs(s);
}

static const GraphicHwOps adp_v4_ops = {
    .invalidate = adp_v4_invalidate,
    .gfx_update = adp_v4_gfx_update,
};

static void adp_v4_reset_hold(Object *obj, ResetType type)
{
    AppleDisplayPipeV4State *s = APPLE_DISPLAY_PIPE_V4(obj);

    QEMU_LOCK_GUARD(&s->lock);

    s->invalidated = true;

    s->int_status = 0;
    adp_update_irqs(s);

    qemu_pixman_image_unref(s->disp_image);
    s->disp_image = pixman_image_create_bits(
        PIXMAN_a8r8g8b8, s->width, s->height,
        (uint32_t *)memory_region_get_ram_ptr(&s->vram),
        s->width * sizeof(uint32_t));

    adp_gp_reset(&s->generic_pipe[0], 0, &s->dma_as, s->width, s->height);
    adp_gp_reset(&s->generic_pipe[1], 1, &s->dma_as, s->width, s->height);

    adp_blend_reset(&s->blend_unit);
}

static void adp_v4_realize(DeviceState *dev, Error **errp)
{
    AppleDisplayPipeV4State *s = APPLE_DISPLAY_PIPE_V4(dev);

    QEMU_LOCK_GUARD(&s->lock);

    s->console = graphic_console_init(dev, 0, &adp_v4_ops, s);
    qemu_console_resize(s->console, s->width, s->height);
}

static Property adp_v4_props[] = {
    DEFINE_PROP_UINT32("width", AppleDisplayPipeV4State, width, 828),
    DEFINE_PROP_UINT32("height", AppleDisplayPipeV4State, height, 1792),
    DEFINE_PROP_END_OF_LIST(),
};

static void adp_v4_class_init(ObjectClass *klass, void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    rc->phases.hold = adp_v4_reset_hold;

    device_class_set_props(dc, adp_v4_props);
    dc->realize = adp_v4_realize;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo adp_v4_type_info = {
    .name = TYPE_APPLE_DISPLAY_PIPE_V4,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AppleDisplayPipeV4State),
    .class_init = adp_v4_class_init,
};

static void adp_v4_register_types(void)
{
    type_register_static(&adp_v4_type_info);
}

type_init(adp_v4_register_types);

static uint32_t adp_timing_info[] = { 0x33C, 0x90, 0x1, 0x1,
                                      0x700, 0x1,  0x1, 0x1 };

AppleDisplayPipeV4State *adp_v4_create(DTBNode *node)
{
    DeviceState *dev;
    SysBusDevice *sbd;
    AppleDisplayPipeV4State *s;
    DTBProp *prop;
    uint64_t *reg;
    int i;

    dev = qdev_new(TYPE_APPLE_DISPLAY_PIPE_V4);
    sbd = SYS_BUS_DEVICE(dev);
    s = APPLE_DISPLAY_PIPE_V4(sbd);

    qemu_mutex_init(&s->lock);

    dtb_set_prop(node, "display-target", 15, "DisplayTarget5");
    dtb_set_prop(node, "display-timing-info", sizeof(adp_timing_info),
                 adp_timing_info);
    dtb_set_prop_u32(node, "bics-param-set", 0xD);
    dtb_set_prop_u32(node, "dot-pitch", 326);
    dtb_set_prop_null(node, "function-brightness_update");

    prop = dtb_find_prop(node, "reg");
    g_assert_nonnull(prop);
    reg = (uint64_t *)prop->data;
    memory_region_init_io(&s->up_regs, OBJECT(sbd), &adp_reg_ops, sbd,
                          "up.regs", reg[1]);
    sysbus_init_mmio(sbd, &s->up_regs);
    object_property_add_const_link(OBJECT(sbd), "up.regs", OBJECT(&s->up_regs));

    for (i = 0; i < 9; i++) {
        sysbus_init_irq(sbd, &s->irqs[i]);
    }

    return s;
}
