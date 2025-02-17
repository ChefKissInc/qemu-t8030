/*
 * Apple LM Backlight Controller.
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
#include "hw/arm/apple-silicon/lm-backlight.h"
#include "hw/i2c/i2c.h"

struct AppleLMBacklightState {
    /*< private >*/
    I2CSlave i2c;

    /*< public >*/
};

static uint8_t apple_lm_backlight_rx(I2CSlave *i2c)
{
    return 0x00;
}

static int apple_lm_backlight_tx(I2CSlave *i2c, uint8_t data)
{
    return 0;
}

static void apple_lm_backlight_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *c = I2C_SLAVE_CLASS(klass);

    c->recv = apple_lm_backlight_rx;
    c->send = apple_lm_backlight_tx;
    dc->desc = "Apple LM Backlight";
    dc->user_creatable = false;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo apple_lm_backlight_type_info = {
    .name = TYPE_APPLE_LM_BACKLIGHT,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(AppleLMBacklightState),
    .class_init = apple_lm_backlight_class_init,
};

static void apple_lm_backlight_register_types(void)
{
    type_register_static(&apple_lm_backlight_type_info);
}

type_init(apple_lm_backlight_register_types);
