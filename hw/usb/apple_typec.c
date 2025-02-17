#include "qemu/osdep.h"
#include "hw/qdev-properties.h"
#include "hw/usb/apple_typec.h"
#include "hw/usb/hcd-tcp.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"

static void apple_typec_realize(DeviceState *dev, Error **errp)
{
    AppleTypeCState *s = APPLE_TYPEC(dev);
    Object *obj;
    BusState *bus = NULL;

    memory_region_init(&s->dma_container_mr, OBJECT(dev),
                       TYPE_APPLE_TYPEC ".dma-container-mr", UINT32_MAX);
    obj = object_property_get_link(OBJECT(dev), "dma-drd", errp);
    s->dma_mr = MEMORY_REGION(obj);
    memory_region_add_subregion(&s->dma_container_mr, 0, s->dma_mr);

    assert(object_property_add_const_link(OBJECT(&s->dwc3), "dma-mr",
                                          OBJECT(&s->dma_container_mr)));

    obj = object_property_get_link(OBJECT(dev), "dma-xhci", errp);
    assert(obj);
    assert(object_property_add_const_link(OBJECT(&s->dwc3), "dma-xhci", obj));

    obj = object_property_get_link(OBJECT(dev), "dma-otg", errp);
    assert(obj);
    assert(object_property_add_const_link(OBJECT(&s->dwc2), "dma-mr", obj));

    sysbus_realize(SYS_BUS_DEVICE(&s->dwc2), errp);
    sysbus_realize(SYS_BUS_DEVICE(&s->dwc3), errp);
    sysbus_pass_irq(SYS_BUS_DEVICE(s), SYS_BUS_DEVICE(&s->dwc3));
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->dwc2.irq);

    s->host = SYS_BUS_DEVICE(qdev_new(TYPE_USB_TCP_HOST));
    sysbus_realize(s->host, errp);

    bus = QLIST_FIRST(&DEVICE(s->host)->child_bus);
    qdev_realize(DEVICE(s->dwc2.device), bus, errp);
    qdev_realize(DEVICE(&s->dwc3.device), bus, errp);
}

static void apple_typec_reset(DeviceState *dev)
{
    AppleTypeCState *s = APPLE_TYPEC(dev);
    device_cold_reset(DEVICE(&s->dwc2));
    device_cold_reset(DEVICE(&s->dwc3));
    device_cold_reset(DEVICE(s->host));
}

static void phy_reg_write(void *opaque, hwaddr addr, uint64_t data,
                          unsigned size)
{
    // qemu_log_mask(LOG_UNIMP, "ATC: phy reg WRITE @ 0x" HWADDR_FMT_plx "
    // value: 0x" HWADDR_FMT_plx "\n", addr, data);

    AppleTypeCState *s = APPLE_TYPEC(opaque);
    memcpy(s->phy_reg + addr, &data, size);
}

static uint64_t phy_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    // qemu_log_mask(LOG_UNIMP, "ATC: phy reg READ @ 0x" HWADDR_FMT_plx "\n",
    // addr);
    AppleTypeCState *s = APPLE_TYPEC(opaque);
    uint64_t val = 0;

    memcpy(&val, s->phy_reg + addr, size);
    return val;
}

static const MemoryRegionOps phy_reg_ops = {
    .write = phy_reg_write,
    .read = phy_reg_read,
};

static void config_reg_write(void *opaque, hwaddr addr, uint64_t data,
                             unsigned size)
{
    // qemu_log_mask(LOG_UNIMP, "ATC: config reg WRITE @ 0x" HWADDR_FMT_plx "
    // value: 0x" HWADDR_FMT_plx "\n", addr, data); arm_cpu_backtrace();

    AppleTypeCState *s = APPLE_TYPEC(opaque);
    memcpy(s->config_reg + addr, &data, size);
}

static uint64_t config_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    // qemu_log_mask(LOG_UNIMP, "ATC: config reg READ @ 0x" HWADDR_FMT_plx "\n",
    // addr); arm_cpu_backtrace();
    AppleTypeCState *s = APPLE_TYPEC(opaque);
    uint64_t val = 0;

    memcpy(&val, s->config_reg + addr, size);
    return val;
}

static const MemoryRegionOps config_reg_ops = {
    .write = config_reg_write,
    .read = config_reg_read,
};

static void apple_typec_init(Object *obj)
{
    DeviceState *dev;
    SysBusDevice *sbd;
    AppleTypeCState *s;

    dev = DEVICE(obj);
    sbd = SYS_BUS_DEVICE(dev);
    s = APPLE_TYPEC(dev);

    memory_region_init(&s->container, OBJECT(dev),
                       TYPE_APPLE_TYPEC ".container", ATC_USB_MMIO_SIZE);
    memory_region_init_io(&s->phy, OBJECT(dev), &phy_reg_ops, s,
                          TYPE_APPLE_TYPEC ".phy", sizeof(s->phy_reg));
    memory_region_add_subregion(&s->container, 0x0000, &s->phy);

    memory_region_init_io(&s->config, OBJECT(dev), &config_reg_ops, s,
                          TYPE_APPLE_TYPEC ".config", sizeof(s->config_reg));
    memory_region_add_subregion(&s->container, 0x20000, &s->config);
    *(uint32_t *)(s->config_reg + 0x20) |= 0x40000000; // pipe ready
    *(uint32_t *)(s->phy_reg + 0x64) |= (1 << 16); // OTG cable connected

    object_initialize_child(OBJECT(dev), "dwc2", &s->dwc2, TYPE_DWC2_USB);
    object_initialize_child(OBJECT(dev), "dwc3", &s->dwc3, TYPE_DWC3_USB);
    object_property_set_uint(OBJECT(&s->dwc3), "intrs", 4, &error_fatal);
    object_property_set_uint(OBJECT(&s->dwc3), "slots", 1, &error_fatal);
    memory_region_add_subregion(
        &s->container, 0x10000,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->dwc3), 0));
    memory_region_add_subregion(
        &s->container, 0x100000,
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->dwc2), 0));
    sysbus_init_mmio(sbd, &s->container);
}

static int apple_typec_post_load(void *opaque, int version_id)
{
    AppleTypeCState *s = APPLE_TYPEC(opaque);
    return 0;
}

static Property apple_typec_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription vmstate_apple_typec = {
    .name = "apple_typec",
    .post_load = apple_typec_post_load,
    .fields =
        (VMStateField[]){
            VMSTATE_UINT8_ARRAY(phy_reg, AppleTypeCState, 0x100),
            VMSTATE_END_OF_LIST(),
        }
};

static void apple_typec_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = apple_typec_realize;
    device_class_set_legacy_reset(dc, apple_typec_reset);
    dc->desc = "Apple Type C USB PHY";
    dc->vmsd = &vmstate_apple_typec;
    device_class_set_props(dc, apple_typec_properties);
}

static const TypeInfo apple_typec_info = {
    .name = TYPE_APPLE_TYPEC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AppleTypeCState),
    .instance_init = apple_typec_init,
    .class_init = apple_typec_class_init,
};

static void apple_typec_register_types(void)
{
    type_register_static(&apple_typec_info);
}

type_init(apple_typec_register_types);
