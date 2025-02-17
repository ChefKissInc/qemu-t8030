#include "qemu/osdep.h"
#include "hw/arm/apple-silicon/dtb.h"
#include "hw/gpio/apple_gpio.h"
#include "hw/irq.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define GPIO_MAX_PIN_NR (512)
#define GPIO_MAX_INT_GRP_NR (0x7)

#define REG_GPIOCFG(_n) (0x000 + (_n) * 4)
#define REG_GPIOINT(_g, _n) (0x800 + (_g) * 0x40 + (((_n) + 31) >> 5) * 4)

#define REG_GPIO_NPL_IN_EN (0xC48)

/* Base Pin Defines for Apple GPIOs */

#define GPIOPADPINS (8)

#define GPIO2PIN(gpio) ((gpio) & (GPIOPADPINS - 1))
#define GPIO2PAD(gpio) (((gpio) >> 8) & 0xFF)
#define GPIO2CONTROLLER(gpio) (((gpio) >> 24) & 0xFF)

#define DATA_0 (0 << 0)
#define DATA_1 (1 << 0)

#define CFG_GP_IN (0 << 1)
#define CFG_GP_OUT (1 << 1)
#define CFG_INT_LVL_HI (2 << 1)
#define CFG_INT_LVL_LO (3 << 1)
#define CFG_INT_EDG_RIS (4 << 1)
#define CFG_INT_EDG_FAL (5 << 1)
#define CFG_INT_EDG_ANY (6 << 1)
#define CFG_DISABLE (7 << 1)
#define CFG_MASK (7 << 1)

#define FUNC_SHIFT (5)
#define FUNC_GPIO (0 << FUNC_SHIFT)
#define FUNC_ALT0 (1 << FUNC_SHIFT)
#define FUNC_ALT1 (2 << FUNC_SHIFT)
#define FUNC_ALT2 (3 << FUNC_SHIFT)
#define FUNC_MASK (3 << FUNC_SHIFT)

#define PULL_NONE (0 << 7)
#define PULL_UP (3 << 7)
#define PULL_UP_STRONG (2 << 7)
#define PULL_DOWN (1 << 7)
#define PULL_MASK (3 << 7)

#define INPUT_ENABLE (1 << 9)

#define INPUT_CMOS (0 << 14)
#define INPUT_SCHMITT (1 << 14)

#define INTR_GRP_SHIFT (16)
#define INTR_GRP_SEL0 (0 << INTR_GRP_SHIFT)
#define INTR_GRP_SEL1 (1 << INTR_GRP_SHIFT)
#define INTR_GRP_SEL2 (2 << INTR_GRP_SHIFT)
#define INTR_GRP_SEL3 (3 << INTR_GRP_SHIFT)
#define INTR_GRP_SEL4 (4 << INTR_GRP_SHIFT)
#define INTR_GRP_SEL5 (5 << INTR_GRP_SHIFT)
#define INTR_GRP_SEL6 (6 << INTR_GRP_SHIFT)
#define INT_MASKED (7 << INTR_GRP_SHIFT)

#define CFG_DISABLED (FUNC_GPIO | CFG_DISABLE | INT_MASKED)
#define CFG_IN (INPUT_ENABLE | FUNC_GPIO | CFG_GP_IN | INT_MASKED)
#define CFG_OUT (INPUT_ENABLE | FUNC_GPIO | CFG_GP_OUT | INT_MASKED)
#define CFG_OUT_0 (INPUT_ENABLE | FUNC_GPIO | CFG_GP_OUT | DATA_0 | INT_MASKED)
#define CFG_OUT_1 (INPUT_ENABLE | FUNC_GPIO | CFG_GP_OUT | DATA_1 | INT_MASKED)
#define CFG_FUNC0 (INPUT_ENABLE | FUNC_ALT0 | INT_MASKED)
#define CFG_FUNC1 (INPUT_ENABLE | FUNC_ALT1 | INT_MASKED)
#define CFG_FUNC2 (INPUT_ENABLE | FUNC_ALT2 | INT_MASKED)

static void apple_gpio_update_pincfg(AppleGPIOState *s, int pin, uint32_t value)
{
    if ((value & INT_MASKED) != INT_MASKED) {
        int irqgrp = (value & INT_MASKED) >> INTR_GRP_SHIFT;

        clear_bit32(pin, s->int_config[irqgrp]);

        switch (value & CFG_MASK) {
        case CFG_INT_LVL_HI:
            if (test_bit32(pin, s->in)) {
                set_bit32_atomic(pin, s->int_config[irqgrp]);
            }
            break;
        case CFG_INT_LVL_LO:
            if (!test_bit32(pin, s->in)) {
                set_bit32_atomic(pin, s->int_config[irqgrp]);
            }
            break;
        default:
            break;
        }
        qemu_set_irq(s->irqs[irqgrp],
                     find_first_bit32(s->int_config[irqgrp], s->pin_count) !=
                         s->pin_count);
    }

    s->gpio_cfg[pin] = value;

    if (value & FUNC_MASK) {
        // TODO: Is this how FUNC_ALT0 supposed to behave?
        // Visual: Not sure, but here's some more logic :^)
        switch (value & FUNC_MASK) {
        case FUNC_ALT0:
            if ((value & CFG_MASK) == CFG_GP_OUT) {
                s->gpio_cfg[pin] &= ~DATA_1;
            } else {
                s->gpio_cfg[pin] |= DATA_1;
            }
            qemu_set_irq(s->out[pin], 1);
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: set pin %u to unknown func %u\n",
                          __func__, pin, value & FUNC_MASK);
            break;
        }
    } else {
        if ((value & CFG_MASK) == CFG_GP_OUT) {
            qemu_set_irq(s->out[pin], value & DATA_1);
        } else {
            qemu_set_irq(s->out[pin], 1);
        }
    }
}


static void apple_gpio_set(void *opaque, int pin, int level)
{
    AppleGPIOState *s = APPLE_GPIO(opaque);
    int grp;
    int irqgrp = -1;

    if (pin >= s->pin_count) {
        return;
    }

    level = level != 0;
    if (level) {
        set_bit32_atomic(pin, s->in);
    } else {
        clear_bit32_atomic(pin, s->in);
    }

    grp = pin >> 5;
    if ((s->gpio_cfg[pin] & INT_MASKED) != INT_MASKED) {
        irqgrp = (s->gpio_cfg[pin] & INT_MASKED) >> INTR_GRP_SHIFT;

        switch (s->gpio_cfg[pin] & CFG_MASK) {
        case CFG_GP_IN:
        case CFG_GP_OUT:
            break;

        case CFG_INT_LVL_HI:
            if (level) {
                set_bit32_atomic(pin, s->int_config[irqgrp]);
            }
            break;

        case CFG_INT_LVL_LO:
            if (!level) {
                set_bit32_atomic(pin, s->int_config[irqgrp]);
            }
            break;

        case CFG_INT_EDG_RIS:
            if (test_bit32(pin, s->in_old) == 0 && level) {
                set_bit32_atomic(pin, s->int_config[irqgrp]);
            }
            break;

        case CFG_INT_EDG_FAL:
            if (test_bit32(pin, s->in_old) && !level) {
                set_bit32_atomic(pin, s->int_config[irqgrp]);
            }
            break;

        case CFG_INT_EDG_ANY:
            if (test_bit32(pin, s->in_old) != level) {
                set_bit32_atomic(pin, s->int_config[irqgrp]);
            }
            break;

        default:
            break;
        }
    }

    s->in_old[grp] = s->in[grp];

    if (irqgrp != -1) {
        qemu_set_irq(s->irqs[irqgrp],
                     find_first_bit32(s->int_config[irqgrp], s->pin_count) !=
                         s->pin_count);
    }
}

static void apple_gpio_realize(DeviceState *dev, Error **errp)
{
    int i;
    AppleGPIOState *s = APPLE_GPIO(dev);

    s->gpio_cfg = g_new0(uint32_t, s->pin_count);
    s->int_config = g_new0(uint32_t *, s->irq_group_count);

    for (i = 0; i < s->irq_group_count; i++) {
        s->int_config[i] = g_new0(uint32_t, s->pin_count);
    }

    s->in_old = g_new0(uint32_t, (s->pin_count + 31) / 32);
    s->in = g_new0(uint32_t, (s->pin_count + 31) / 32);
}

static void apple_gpio_reset(DeviceState *dev)
{
    int i;
    AppleGPIOState *s = APPLE_GPIO(dev);

    for (i = 0; i < s->pin_count; i++) {
        s->gpio_cfg[i] = CFG_DISABLED;
    }

    for (i = 0; i < s->irq_group_count; i++) {
        memset(s->int_config[i], 0, 4 * s->pin_count);
    }

    memset(s->in_old, 0, sizeof(*s->in_old) * ((s->pin_count + 31) / 32));
    memset(s->in, 0, sizeof(*s->in_old) * ((s->pin_count + 31) / 32));
}

static void apple_gpio_cfg_write(AppleGPIOState *s, unsigned int pin,
                                 hwaddr addr, uint32_t value)
{
    if (pin >= s->pin_count) {
        qemu_log_mask(LOG_UNIMP, "%s: Bad offset 0x" HWADDR_FMT_plx "\n",
                      __func__, addr);
        return;
    }

    apple_gpio_update_pincfg(s, pin, value);
}

static uint32_t apple_gpio_cfg_read(AppleGPIOState *s, unsigned int pin,
                                    hwaddr addr)
{
    uint32_t val;

    if (pin >= s->pin_count) {
        qemu_log_mask(LOG_UNIMP, "%s: Bad offset 0x" HWADDR_FMT_plx "\n",
                      __func__, addr);
        return 0;
    }

    val = s->gpio_cfg[pin];

    if (((val & FUNC_MASK) == FUNC_GPIO) && ((val & CFG_MASK) == CFG_GP_IN)) {
        val &= ~DATA_1;
        val |= test_bit32(pin, s->in);
    }

    return val;
}

static void apple_gpio_int_write(AppleGPIOState *s, unsigned int group,
                                 hwaddr addr, uint32_t value)
{
    unsigned int offset;

    if (group >= s->irq_group_count) {
        qemu_log_mask(LOG_UNIMP, "%s: Bad offset 0x" HWADDR_FMT_plx "\n",
                      __func__, addr);
        return;
    }

    offset = addr - REG_GPIOINT(group, 0);
    s->int_config[group][offset >> 2] &= ~value;

    if (find_first_bit32(s->int_config[group], s->pin_count) == s->pin_count) {
        qemu_irq_lower(s->irqs[group]);
    }
}

static uint32_t apple_gpio_int_read(AppleGPIOState *s, unsigned int group,
                                    hwaddr addr)
{
    unsigned int offset;

    if (group >= s->irq_group_count) {
        qemu_log_mask(LOG_UNIMP, "%s: Bad offset 0x" HWADDR_FMT_plx "\n",
                      __func__, addr);
        return 0;
    }

    offset = addr - REG_GPIOINT(group, 0);
    return s->int_config[group][offset >> 2];
}

static void apple_gpio_reg_write(void *opaque, hwaddr addr, uint64_t data,
                                 unsigned size)
{
    AppleGPIOState *s = APPLE_GPIO(opaque);

    switch (addr) {
    case REG_GPIOCFG(0)... REG_GPIOCFG(GPIO_MAX_PIN_NR - 1):
        if ((data & FUNC_MASK) > FUNC_ALT0) {
            qemu_log_mask(LOG_UNIMP,
                          "%s: alternate function 0x" HWADDR_FMT_plx
                          " is not supported\n",
                          __func__, ((data & FUNC_MASK) >> FUNC_SHIFT) - 1);
        }
        return apple_gpio_cfg_write(s, (addr - REG_GPIOCFG(0)) >> 2, addr,
                                    data);

    case REG_GPIOINT(0, 0)... REG_GPIOINT(GPIO_MAX_INT_GRP_NR,
                                          GPIO_MAX_PIN_NR - 1):
        return apple_gpio_int_write(s, (addr - REG_GPIOINT(0, 0)) >> 6, addr,
                                    data);

    case REG_GPIO_NPL_IN_EN:
        s->npl = data;
        break;

    default:
        qemu_log_mask(LOG_UNIMP,
                      "%s: Bad offset 0x" HWADDR_FMT_plx ": " HWADDR_FMT_plx
                      "\n",
                      __func__, addr, data);
        break;
    }
}

static uint64_t apple_gpio_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    AppleGPIOState *s = APPLE_GPIO(opaque);

    switch (addr) {
    case REG_GPIOCFG(0)... REG_GPIOCFG(GPIO_MAX_PIN_NR - 1):
        return apple_gpio_cfg_read(s, (addr - REG_GPIOCFG(0)) >> 2, addr);

    case REG_GPIOINT(0, 0)... REG_GPIOINT(GPIO_MAX_INT_GRP_NR,
                                          GPIO_MAX_PIN_NR - 1):
        return apple_gpio_int_read(s, (addr - REG_GPIOINT(0, 0)) >> 6, addr);
    case REG_GPIO_NPL_IN_EN:
        return s->npl;
    case 0xC4C:
        return 0xFF;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: Bad offset 0x" HWADDR_FMT_plx "\n",
                      __func__, addr);
    }

    return 0;
}

static const MemoryRegionOps gpio_reg_ops = {
    .write = apple_gpio_reg_write,
    .read = apple_gpio_reg_read,
    .valid.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.unaligned = false,
};

DeviceState *apple_custom_gpio_create(char *name, uint64_t mmio_size,
                                      uint32_t gpio_pins,
                                      uint32_t gpio_int_groups)
{
    int i;
    DeviceState *dev;
    SysBusDevice *sbd;
    AppleGPIOState *s;

    dev = qdev_new(TYPE_APPLE_GPIO);
    sbd = SYS_BUS_DEVICE(dev);
    s = APPLE_GPIO(dev);

    s->iomem = g_new(MemoryRegion, 1);
    dev->id = g_strdup(name);
    memory_region_init_io(s->iomem, OBJECT(dev), &gpio_reg_ops, s,
                          (const char *)name, mmio_size);
    sysbus_init_mmio(sbd, s->iomem);

    s->pin_count = gpio_pins;
    assert(s->pin_count < GPIO_MAX_PIN_NR);
    qdev_init_gpio_in(dev, apple_gpio_set, s->pin_count);

    s->out = g_new(qemu_irq, s->pin_count);
    qdev_init_gpio_out(dev, s->out, s->pin_count);

    s->irq_group_count = gpio_int_groups;
    s->irqs = g_new(qemu_irq, s->irq_group_count);

    for (i = 0; i < s->irq_group_count; i++) {
        sysbus_init_irq(sbd, &s->irqs[i]);
    }

    return dev;
}

DeviceState *apple_gpio_create(DTBNode *node)
{
    int i;
    uint64_t mmio_size;
    DeviceState *dev;
    SysBusDevice *sbd;
    AppleGPIOState *s;

    dev = qdev_new(TYPE_APPLE_GPIO);
    sbd = SYS_BUS_DEVICE(dev);
    s = APPLE_GPIO(dev);

    s->iomem = g_new(MemoryRegion, 1);
    DTBProp *prop = dtb_find_prop(node, "reg");
    mmio_size = ((hwaddr *)prop->data)[1];
    prop = dtb_find_prop(node, "name");
    dev->id = g_strdup((const char *)prop->data);
    memory_region_init_io(s->iomem, OBJECT(dev), &gpio_reg_ops, s,
                          (const char *)prop->data, mmio_size);
    sysbus_init_mmio(sbd, s->iomem);

    prop = dtb_find_prop(node, "#gpio-pins");
    s->pin_count = *(uint32_t *)prop->data;
    assert(s->pin_count < GPIO_MAX_PIN_NR);
    qdev_init_gpio_in(dev, apple_gpio_set, s->pin_count);

    s->out = g_new(qemu_irq, s->pin_count);
    qdev_init_gpio_out(dev, s->out, s->pin_count);

    prop = dtb_find_prop(node, "#gpio-int-groups");
    s->irq_group_count = *(uint32_t *)prop->data;
    s->irqs = g_new(qemu_irq, s->irq_group_count);

    for (i = 0; i < s->irq_group_count; i++) {
        sysbus_init_irq(sbd, &s->irqs[i]);
    }

    return dev;
}

static void apple_gpio_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = apple_gpio_realize;
    device_class_set_legacy_reset(dc, apple_gpio_reset);
    dc->desc = "Apple General Purpose Input/Output Controller";
}

static const TypeInfo apple_gpio_info = {
    .name = TYPE_APPLE_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AppleGPIOState),
    .class_init = apple_gpio_class_init,
};

static void apple_gpio_register_types(void)
{
    type_register_static(&apple_gpio_info);
}

type_init(apple_gpio_register_types);
