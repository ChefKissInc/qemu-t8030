#include "qemu/osdep.h"
#include "hw/arm/apple-silicon/dtb.h"
#include "hw/qdev-properties.h"
#include "hw/usb/apple_otg.h"
#include "hw/usb/hcd-dwc2.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define REG_AUSB_USB20PHY_CTL (0x00)
#define REG_AUSB_USB20PHY_OTGSIG (0x04)
#define REG_AUSB_USB20PHY_CFG0 (0x08)
#define REG_AUSB_USB20PHY_CFG1 (0x0C)
#define REG_AUSB_USB20PHY_BATCTL (0x10)
#define REG_AUSB_USB20PHY_TEST (0x1C)

#define REG_AUSB_WIDGET_OTG_QOS (0x14)
#define REG_AUSB_WIDGET_OTG_CACHE (0x18)
#define REG_AUSB_WIDGET_OTG_ADDR (0x1C)
#define REG_AUSB_WIDGET_EHCI0_QOS (0x34)
#define REG_AUSB_WIDGET_EHCI0_CACHE (0x38)
#define REG_AUSB_WIDGET_EHCI0_ADDR (0x3C)
#define REG_AUSB_WIDGET_OHCI0_QOS (0x54)
#define REG_AUSB_WIDGET_OHCI0_CACHE (0x58)
#define REG_AUSB_WIDGET_OHCI0_ADDR (0x5C)
#define REG_AUSB_WIDGET_EHCI1_QOS (0x74)
#define REG_AUSB_WIDGET_EHCI1_CACHE (0x78)
#define REG_AUSB_WIDGET_EHCI1_ADDR (0x7C)

static void apple_otg_realize(DeviceState *dev, Error **errp)
{
    AppleOTGState *s = APPLE_OTG(dev);
    Object *obj;
    Error *local_err = NULL;

    memory_region_init(&s->dma_container_mr, OBJECT(dev),
                       TYPE_APPLE_OTG ".dma-container-mr", UINT32_MAX);
    obj = object_property_get_link(OBJECT(dev), "dma-mr", &local_err);
    if (obj) {
        s->dma_mr = MEMORY_REGION(obj);
        memory_region_add_subregion(&s->dma_container_mr, 0, s->dma_mr);
        s->dart = true;
    } else {
        if (local_err) {
            error_reportf_err(local_err, "No DMA memory region found: ");
        }
        warn_report("Redirecting all OTG DMA accesses to 0x800000000");
        s->dma_mr = g_new(MemoryRegion, 1);
        memory_region_init_alias(s->dma_mr, OBJECT(dev),
                                 TYPE_APPLE_OTG ".dma-mr", get_system_memory(),
                                 0x800000000, UINT32_MAX);
        memory_region_add_subregion(&s->dma_container_mr, 0, s->dma_mr);
        s->dart = false;
    }
    assert(object_property_add_const_link(OBJECT(&s->dwc2), "dma-mr",
                                          OBJECT(&s->dma_container_mr)));
    sysbus_realize(SYS_BUS_DEVICE(&s->dwc2), errp);
    sysbus_pass_irq(SYS_BUS_DEVICE(s), SYS_BUS_DEVICE(&s->dwc2));

    object_initialize_child(OBJECT(dev), "host", &s->usbtcp, TYPE_USB_TCP_HOST);
    sysbus_realize(SYS_BUS_DEVICE(&s->usbhcd), errp);
    qdev_realize(DEVICE(s->dwc2.device), &s->usbtcp.bus.qbus, errp);
}

static void apple_otg_reset(DeviceState *dev)
{
    AppleOTGState *s = APPLE_OTG(dev);
    device_cold_reset(DEVICE(&s->dwc2));
    device_cold_reset(&s->usbhcd);
}

static void phy_reg_write(void *opaque, hwaddr addr, uint64_t data,
                          unsigned size)
{
    qemu_log_mask(LOG_UNIMP,
                  "OTG: phy reg WRITE @ 0x" HWADDR_FMT_plx
                  " value: 0x" HWADDR_FMT_plx "\n",
                  addr, data);

    AppleOTGState *s = APPLE_OTG(opaque);
    memcpy(s->phy_reg + addr, &data, size);
}

static uint64_t phy_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "OTG: phy reg READ @ 0x" HWADDR_FMT_plx "\n",
                  addr);
    AppleOTGState *s = APPLE_OTG(opaque);
    uint64_t val = 0;

    memcpy(&val, s->phy_reg + addr, size);
    return val;
}

static const MemoryRegionOps phy_reg_ops = {
    .write = phy_reg_write,
    .read = phy_reg_read,
};

static void usbctl_reg_write(void *opaque, hwaddr addr, uint64_t data,
                             unsigned size)
{
    qemu_log_mask(LOG_UNIMP,
                  "OTG: usbctl reg WRITE @ 0x" HWADDR_FMT_plx
                  " value: 0x" HWADDR_FMT_plx "\n",
                  addr, data);
    AppleOTGState *s = APPLE_OTG(opaque);

    memcpy(s->usbctl_reg + addr, &data, size);
}

static uint64_t usbctl_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "OTG: usbctl reg READ @ 0x" HWADDR_FMT_plx "\n",
                  addr);
    AppleOTGState *s = APPLE_OTG(opaque);
    uint64_t val = 0;

    memcpy(&val, s->usbctl_reg + addr, size);
    return val;
}

static const MemoryRegionOps usbctl_reg_ops = {
    .write = usbctl_reg_write,
    .read = usbctl_reg_read,
};

static void widget_reg_write(void *opaque, hwaddr addr, uint64_t data,
                             unsigned size)
{
    AppleOTGState *s = APPLE_OTG(opaque);
    uint32_t value = data;
    bool dma_changed = false;

    qemu_log_mask(LOG_UNIMP,
                  "OTG: widget reg WRITE @ 0x" HWADDR_FMT_plx
                  " value: 0x" HWADDR_FMT_plx "\n",
                  addr, data);
    switch (addr) {
    case REG_AUSB_WIDGET_OTG_ADDR:
        if (value & (1 << 8)) {
            uint64_t high_addr = (uint64_t)(value & 0xf) << 32;
            if (high_addr != s->high_addr) {
                dma_changed = true;
                s->high_addr = high_addr;
            }
        }
        break;
    default:
        break;
    }
    memcpy(s->widget_reg + addr, &data, size);

    if (dma_changed && !s->dart) {
        memory_region_set_alias_offset(s->dma_mr, s->high_addr);
    }
}

static uint64_t widget_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "OTG: widget reg READ @ 0x" HWADDR_FMT_plx "\n",
                  addr);
    AppleOTGState *s = APPLE_OTG(opaque);
    uint64_t val = 0;

    memcpy(&val, s->widget_reg + addr, size);
    return val;
}

static const MemoryRegionOps widget_reg_ops = {
    .write = widget_reg_write,
    .read = widget_reg_read,
};

DeviceState *apple_otg_create(DTBNode *node)
{
    DeviceState *dev;
    SysBusDevice *sbd;
    AppleOTGState *s;
    DTBNode *child;
    DTBProp *prop;

    dev = qdev_new(TYPE_APPLE_OTG);
    sbd = SYS_BUS_DEVICE(dev);
    s = APPLE_OTG(dev);

    memory_region_init_io(&s->phy, OBJECT(dev), &phy_reg_ops, s,
                          TYPE_APPLE_OTG ".phy", sizeof(s->phy_reg));
    sysbus_init_mmio(sbd, &s->phy);
    *(uint32_t *)(s->phy_reg + REG_AUSB_USB20PHY_OTGSIG) |=
        (1 << 8); // cable connected
    memory_region_init_io(&s->usbctl, OBJECT(dev), &usbctl_reg_ops, s,
                          TYPE_APPLE_OTG ".usbctl", sizeof(s->usbctl_reg));
    sysbus_init_mmio(sbd, &s->usbctl);

    child = dtb_get_node(node, "usb-device");
    assert(child);
    prop = dtb_find_prop(child, "reg");
    assert(prop);

    object_initialize_child(OBJECT(dev), "dwc2", &s->dwc2, TYPE_DWC2_USB);
    memory_region_init_alias(
        &s->dwc2_mr, OBJECT(dev), TYPE_APPLE_OTG ".dwc2",
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->dwc2), 0), 0,
        ((uint64_t *)prop->data)[1]);
    sysbus_init_mmio(sbd, &s->dwc2_mr);

    memory_region_init_io(&s->widget, OBJECT(dev), &widget_reg_ops, s,
                          TYPE_APPLE_OTG ".widget", sizeof(s->widget_reg));
    sysbus_init_mmio(sbd, &s->widget);
    return dev;
}

static int apple_otg_post_load(void *opaque, int version_id)
{
    AppleOTGState *s = APPLE_OTG(opaque);

    if (!s->dart) {
        memory_region_set_alias_offset(s->dma_mr, s->high_addr);
    }
    return 0;
}

static Property apple_otg_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription vmstate_apple_otg = {
    .name = "apple_otg",
    .post_load = apple_otg_post_load,
    .fields =
        (VMStateField[]){
            VMSTATE_UINT8_ARRAY(phy_reg, AppleOTGState, 0x20),
            VMSTATE_UINT8_ARRAY(usbctl_reg, AppleOTGState, 0x1000),
            VMSTATE_UINT8_ARRAY(widget_reg, AppleOTGState, 0x100),
            VMSTATE_UINT64(high_addr, AppleOTGState),
            VMSTATE_END_OF_LIST(),
        }
};

static void apple_otg_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = apple_otg_realize;
    device_class_set_legacy_reset(dc, apple_otg_reset);
    dc->desc = "Apple Synopsys USB OTG Controller";
    dc->vmsd = &vmstate_apple_otg;
    device_class_set_props(dc, apple_otg_properties);
}

static const TypeInfo apple_otg_info = {
    .name = TYPE_APPLE_OTG,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AppleOTGState),
    .class_init = apple_otg_class_init,
};

static void apple_otg_register_types(void)
{
    type_register_static(&apple_otg_info);
}

type_init(apple_otg_register_types);
