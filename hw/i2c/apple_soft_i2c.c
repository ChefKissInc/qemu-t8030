#include "qemu/osdep.h"
#include "hw/arm/apple-silicon/dtb.h"
#include "hw/i2c/apple_soft_i2c.h"
#include "hw/i2c/bitbang_i2c.h"
#include "hw/i2c/i2c.h"
#include "hw/irq.h"
#include "qemu/log.h"
#include "qemu/module.h"

static void apple_soft_i2c_gpio_set(void *opaque, int line, int level)
{
    AppleSoftI2CState *s = APPLE_SOFT_I2C(opaque);

    level = bitbang_i2c_set(&s->bitbang, line, level);
    if (level != s->last_level) {
        s->last_level = level;
        qemu_set_irq(s->out, level);
    }
}

static void i2c_reg_write(void *opaque, hwaddr addr, uint64_t data,
                          unsigned size)
{
    qemu_log_mask(LOG_UNIMP,
                  "I2C: reg WRITE @ 0x" HWADDR_FMT_plx
                  " value: 0x" HWADDR_FMT_plx "\n",
                  addr, data);
}

static uint64_t i2c_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "I2C: reg READ @ 0x" HWADDR_FMT_plx "\n", addr);

    return 0;
}

static const MemoryRegionOps i2c_reg_ops = {
    .write = i2c_reg_write,
    .read = i2c_reg_read,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

DeviceState *apple_soft_i2c_create(DTBNode *node)
{
    DeviceState *dev = qdev_new(TYPE_APPLE_SOFT_I2C);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    AppleSoftI2CState *s = APPLE_SOFT_I2C(dev);
    DTBProp *prop = dtb_find_prop(node, "reg");
    uint64_t mmio_size = ((hwaddr *)prop->data)[1];
    char bus_name[32] = { 0 };

    prop = dtb_find_prop(node, "name");
    dev->id = g_strdup((const char *)prop->data);
    memory_region_init_io(&s->iomem, OBJECT(dev), &i2c_reg_ops, s,
                          (const char *)prop->data, mmio_size);

    snprintf(bus_name, sizeof(bus_name), "%s.bus", (const char *)prop->data);
    s->bus = i2c_init_bus(dev, (const char *)bus_name);
    sysbus_init_mmio(sbd, &s->iomem);

    dtb_set_prop(node, "compatible", 9, "iic,soft");

    qdev_init_gpio_in(dev, apple_soft_i2c_gpio_set, 2);
    qdev_init_gpio_out(dev, &s->out, 1);

    sysbus_init_irq(sbd, &s->irq);

    bitbang_i2c_init(&s->bitbang, s->bus);

    return dev;
}

static void apple_soft_i2c_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "Apple Software I2C Controller";
}

static const TypeInfo apple_soft_i2c_type_info = {
    .name = TYPE_APPLE_SOFT_I2C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AppleSoftI2CState),
    .class_init = apple_soft_i2c_class_init,
};

static void apple_soft_i2c_register_types(void)
{
    type_register_static(&apple_soft_i2c_type_info);
}

type_init(apple_soft_i2c_register_types)
