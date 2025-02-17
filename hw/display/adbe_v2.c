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

#include "qemu/osdep.h"
#include "hw/display/adbe_v2.h"
#include "hw/qdev-properties.h"
#include "qemu/log.h"
#include "qom/object.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"
#include "framebuffer.h"

#define REG_SPDS_VERSION (0x1014)

#define REG_DBE_VFTG_CTRL (0x8)
#define REG_DBE_SCREEN_SIZE (0xC)
#define DBE_VFTG_CTRL_VFTG_ENABLE BIT(31)
#define DBE_VFTG_CTRL_VFTG_STATUS BIT(30)
#define DBE_VFTG_CTRL_UPDATE_ENABLE_TIMING BIT(15)
#define DBE_VFTG_CTRL_UPDATE_REQ_TIMING BIT(14)
#define REG_DBE_FRONT_PORCH (0x10)
#define REG_DBE_SYNC_PULSE (0x14)
#define REG_DBE_BACK_PORCH (0x18)
#define REG_DBE_CONST_COLOUR (0x34)

static void frontend_write(void *opaque, hwaddr addr, uint64_t data,
                           unsigned size)
{
    ADBEV2 *s = ADBE_V2(opaque);

    switch (addr) {
    default:
        qemu_log_mask(LOG_UNIMP,
                      "disp0: /frontend/ 0x" HWADDR_FMT_plx " <- 0x%X\n", addr,
                      (uint32_t)data);
        break;
    }
}

static uint64_t frontend_read(void *opaque, hwaddr addr, unsigned size)
{
    ADBEV2 *s = ADBE_V2(opaque);

    switch (addr) {
    case REG_SPDS_VERSION:
        qemu_log_mask(LOG_GUEST_ERROR, "disp0: REG_SPDS_VERSION -> 0x13\n");
        return 0x13;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "disp0: /frontend/ 0x" HWADDR_FMT_plx " -> 0x0\n", addr);
        return 0;
    }
}

static const MemoryRegionOps frontend_reg_ops = {
    .write = frontend_write,
    .read = frontend_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned = false,
};

static void backend_write(void *opaque, hwaddr addr, uint64_t data,
                          unsigned size)
{
    ADBEV2 *s = ADBE_V2(opaque);

    switch (addr) {
    case REG_DBE_VFTG_CTRL:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "disp0: /backend/ REG_DBE_VFTG_CTRL <- 0x%X\n",
                      (uint32_t)data);
        if (data & DBE_VFTG_CTRL_VFTG_ENABLE) {
            data |= DBE_VFTG_CTRL_VFTG_STATUS;
        }
        s->dbe_state.vftg_ctl = (uint32_t)data;
        break;
    case REG_DBE_SCREEN_SIZE:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "disp0: /backend/ attempted to set screen size, this is "
                      "NOT supported!\n");
        break;
    case REG_DBE_CONST_COLOUR:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "disp0: /backend/ REG_DBE_CONST_COLOUR <- 0x%X\n",
                      (uint32_t)data);
        s->dbe_state.const_colour = (uint32_t)data;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "disp0: /backend/ 0x" HWADDR_FMT_plx " <- 0x%X\n", addr,
                      (uint32_t)data);
        break;
    }
}

static uint64_t backend_read(void *opaque, hwaddr addr, unsigned size)
{
    ADBEV2 *s = ADBE_V2(opaque);

    switch (addr) {
    case REG_DBE_VFTG_CTRL:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "disp0: /backend/ REG_DBE_VFTG_CTRL -> 0x%X\n",
                      s->dbe_state.vftg_ctl);
        return s->dbe_state.vftg_ctl;
    case REG_DBE_SCREEN_SIZE:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "disp0: /backend/ REG_DBE_SCREEN_SIZE -> 0x%X\n",
                      s->width | (s->height << 16));
        return s->width | (s->height << 16);
    case REG_DBE_FRONT_PORCH:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "disp0: /backend/ REG_DBE_FRONT_PORCH -> 0x%X\n",
                      102 | (536 << 16));
        return 102 | (536 << 16);
    case REG_DBE_SYNC_PULSE:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "disp0: /backend/ REG_DBE_SYNC_PULSE -> 0x%X\n",
                      32 | (3 << 16));
        return 32 | (3 << 16);
    case REG_DBE_BACK_PORCH:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "disp0: /backend/ REG_DBE_BACK_PORCH -> 0x%X\n",
                      4 | (4 << 16));
        return 4 | (4 << 16);
    case REG_DBE_CONST_COLOUR:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "disp0: /backend/ REG_DBE_CONST_COLOUR -> 0x%X\n",
                      s->dbe_state.const_colour);
        return s->dbe_state.const_colour;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "disp0: /backend/ 0x" HWADDR_FMT_plx " -> 0x0\n", addr);
        return 0;
    }
}

static const MemoryRegionOps backend_reg_ops = {
    .write = backend_write,
    .read = backend_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned = false,
};

static void dummy_write(void *opaque, hwaddr addr, uint64_t data, unsigned size)
{
    switch (addr) {
    default:
        qemu_log_mask(LOG_UNIMP,
                      "disp0: /dummy/ @ 0x" HWADDR_FMT_plx " <- 0x%X\n", addr,
                      (uint32_t)data);
        break;
    }
}

static uint64_t dummy_read(void *opaque, hwaddr addr, unsigned size)
{
    switch (addr) {
    default:
        qemu_log_mask(LOG_UNIMP,
                      "disp0: /dummy/ @ 0x" HWADDR_FMT_plx " -> 0x0\n", addr);
        return 0;
    }
}

static const MemoryRegionOps dummy_reg_ops = {
    .write = dummy_write,
    .read = dummy_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned = false,
};

ADBEV2 *adbe_v2_create(DTBNode *node)
{
    DeviceState *dev;
    SysBusDevice *sbd;
    ADBEV2 *s;
    DTBProp *prop;
    uint64_t *reg;
    MemoryRegion *mr;

    dev = qdev_new(TYPE_ADBE_V2);
    sbd = SYS_BUS_DEVICE(dev);
    s = ADBE_V2(sbd);

    dtb_set_prop_u32(node, "dot-pitch", 326);

    prop = dtb_find_prop(node, "reg");
    g_assert_nonnull(prop);
    reg = (uint64_t *)prop->data;
    mr = g_new0(MemoryRegion, 5);
    memory_region_init_io(mr, OBJECT(sbd), &frontend_reg_ops, sbd,
                          "adbe.frontend", reg[1]);
    memory_region_init_io(&s->backend_regs, OBJECT(sbd), &backend_reg_ops, sbd,
                          "adbe.backend", reg[3]);
    memory_region_init_io(mr + 1, OBJECT(sbd), &dummy_reg_ops, sbd, "adbe.aap",
                          reg[5]);
    memory_region_init_io(mr + 2, OBJECT(sbd), &dummy_reg_ops, sbd,
                          "adbe.pixel-bl", reg[7]);
    memory_region_init_io(mr + 3, OBJECT(sbd), &dummy_reg_ops, sbd,
                          "adbe.dither", reg[9]);
    memory_region_init_io(mr + 4, OBJECT(sbd), &dummy_reg_ops, sbd, "adbe.prc",
                          reg[11]);

    object_property_add_const_link(OBJECT(sbd), "adbe.frontend", OBJECT(mr));
    object_property_add_const_link(OBJECT(sbd), "adbe.backend",
                                   OBJECT(&s->backend_regs));
    object_property_add_const_link(OBJECT(sbd), "adbe.aap", OBJECT(mr + 1));
    object_property_add_const_link(OBJECT(sbd), "adbe.pixel-bl",
                                   OBJECT(mr + 2));
    object_property_add_const_link(OBJECT(sbd), "adbe.dither", OBJECT(mr + 3));
    object_property_add_const_link(OBJECT(sbd), "adbe.prc", OBJECT(mr + 4));

    sysbus_init_mmio(sbd, mr);
    sysbus_init_mmio(sbd, &s->backend_regs);
    sysbus_init_mmio(sbd, mr + 1);
    sysbus_init_mmio(sbd, mr + 2);
    sysbus_init_mmio(sbd, mr + 3);
    sysbus_init_mmio(sbd, mr + 4);

    return s;
}

static void adbe_v2_draw_row(void *opaque, uint8_t *dest, const uint8_t *src,
                             int width, int dest_pitch)
{
    while (width--) {
        uint32_t colour = ldl_le_p(src);
        src += sizeof(colour);
        memcpy(dest, &colour, sizeof(colour));
        dest += sizeof(colour);
    }
}

static void adbe_v2_gfx_update(void *opaque)
{
    ADBEV2 *s = ADBE_V2(opaque);
    DisplaySurface *surface = qemu_console_surface(s->console);

    int stride = s->width * sizeof(uint32_t);

    int first = 0, last = 0;

    if (!s->vram_section.mr) {
        framebuffer_update_memory_section(&s->vram_section, &s->vram, 0,
                                          s->height, stride);
    }
    framebuffer_update_display(surface, &s->vram_section, s->width, s->height,
                               stride, stride, 0, 0, adbe_v2_draw_row, s,
                               &first, &last);
    if (first >= 0) {
        dpy_gfx_update(s->console, 0, first, s->width, last - first + 1);
    }
}

static const GraphicHwOps adbe_v2_ops = {
    .gfx_update = adbe_v2_gfx_update,
};

static void adbe_v2_realize(DeviceState *dev, Error **errp)
{
    ADBEV2 *s = ADBE_V2(dev);

    memset(&s->dbe_state, 0, sizeof(s->dbe_state));
    s->dbe_state.vftg_ctl =
        DBE_VFTG_CTRL_VFTG_ENABLE | DBE_VFTG_CTRL_VFTG_STATUS |
        DBE_VFTG_CTRL_UPDATE_ENABLE_TIMING | DBE_VFTG_CTRL_UPDATE_REQ_TIMING;
    s->console = graphic_console_init(dev, 0, &adbe_v2_ops, s);
    qemu_console_resize(s->console, s->width, s->height);
}

static Property adbe_v2_props[] = {
    // iPhone 4/4S
    DEFINE_PROP_UINT32("width", ADBEV2, width, 640),
    DEFINE_PROP_UINT32("height", ADBEV2, height, 960),
    DEFINE_PROP_END_OF_LIST(),
};

static void adbe_v2_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    device_class_set_props(dc, adbe_v2_props);
    dc->realize = adbe_v2_realize;
}

static const TypeInfo adbe_v2_type_info = {
    .name = TYPE_ADBE_V2,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ADBEV2),
    .class_init = adbe_v2_class_init,
};

static void adbe_v2_register_types(void)
{
    type_register_static(&adbe_v2_type_info);
}

type_init(adbe_v2_register_types);
