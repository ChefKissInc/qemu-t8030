#include "qemu/osdep.h"
#include "hw/misc/apple-silicon/a7iop/core.h"
#include "hw/misc/apple-silicon/a7iop/mailbox/core.h"
#include "hw/misc/apple-silicon/a7iop/private.h"
#include "qemu/bitops.h"
#include "qemu/lockable.h"

void apple_a7iop_send_ap(AppleA7IOP *s, AppleA7IOPMessage *msg)
{
    apple_a7iop_mailbox_send_ap(s->iop_mailbox, msg);
}

AppleA7IOPMessage *apple_a7iop_recv_ap(AppleA7IOP *s)
{
    return apple_a7iop_mailbox_recv_ap(s->iop_mailbox);
}

void apple_a7iop_send_iop(AppleA7IOP *s, AppleA7IOPMessage *msg)
{
    apple_a7iop_mailbox_send_iop(s->ap_mailbox, msg);
}

AppleA7IOPMessage *apple_a7iop_recv_iop(AppleA7IOP *s)
{
    return apple_a7iop_mailbox_recv_iop(s->ap_mailbox);
}

void apple_a7iop_cpu_start(AppleA7IOP *s, bool wake)
{
    if (s->ops == NULL) {
        return;
    }

    if (wake) {
        if (s->ops->wakeup) {
            s->ops->wakeup(s);
        }
    } else if (s->ops->start) {
        s->ops->start(s);
    }
}

uint32_t apple_a7iop_get_cpu_status(AppleA7IOP *s)
{
    QEMU_LOCK_GUARD(&s->lock);
    return s->cpu_status;
}

void apple_a7iop_set_cpu_status(AppleA7IOP *s, uint32_t value)
{
    QEMU_LOCK_GUARD(&s->lock);
    s->cpu_status = value;
}

uint32_t apple_a7iop_get_cpu_ctrl(AppleA7IOP *s)
{
    QEMU_LOCK_GUARD(&s->lock);
    return s->cpu_ctrl;
}

void apple_a7iop_set_cpu_ctrl(AppleA7IOP *s, uint32_t value)
{
    WITH_QEMU_LOCK_GUARD(&s->lock)
    {
        s->cpu_ctrl = value;
    }
    if ((value & (CPU_CTRL_RUN | SEP_BOOT_MONITOR_RUN)) != 0) {
        apple_a7iop_cpu_start(s, false);
    }
}

void apple_a7iop_init(AppleA7IOP *s, const char *role, uint64_t mmio_size,
                      AppleA7IOPVersion version, const AppleA7IOPOps *ops,
                      QEMUBH *iop_bh)
{
    DeviceState *dev;
    SysBusDevice *sbd;
    char name[32];

    dev = DEVICE(s);
    sbd = SYS_BUS_DEVICE(dev);

    s->role = g_strdup(role);
    s->ops = ops;

    qemu_mutex_init(&s->lock);

    snprintf(name, sizeof(name), "%s-iop", s->role);
    s->iop_mailbox = apple_a7iop_mailbox_new(name, version, NULL, NULL, iop_bh);
    object_property_add_child(OBJECT(dev), "iop-mailbox",
                              OBJECT(s->iop_mailbox));

    snprintf(name, sizeof(name), "%s-ap", s->role);
    s->ap_mailbox =
        apple_a7iop_mailbox_new(name, version, s->iop_mailbox, NULL, NULL);
    s->iop_mailbox->ap_mailbox = s->ap_mailbox;
    object_property_add_child(OBJECT(dev), "ap-mailbox", OBJECT(s->ap_mailbox));

    switch (version) {
    case APPLE_A7IOP_V2:
        apple_a7iop_init_mmio_v2(s, mmio_size);
        break;
    case APPLE_A7IOP_V4:
        apple_a7iop_init_mmio_v4(s, mmio_size);
        break;
    }
    s->version = version;

    sysbus_pass_irq(sbd, SYS_BUS_DEVICE(s->ap_mailbox));

    // qdev_init_gpio_out_named(dev, &s->iop_irq, APPLE_A7IOP_IOP_IRQ, 1);
}

static void apple_a7iop_reset(DeviceState *opaque)
{
    AppleA7IOP *s;

    s = APPLE_A7IOP(opaque);

    QEMU_LOCK_GUARD(&s->lock);
    s->cpu_status = 0x00000000;
}

static void apple_a7iop_realize(DeviceState *opaque, Error **errp)
{
    AppleA7IOP *s;

    s = APPLE_A7IOP(opaque);

    sysbus_realize(SYS_BUS_DEVICE(s->iop_mailbox), errp);
    sysbus_realize(SYS_BUS_DEVICE(s->ap_mailbox), errp);
}

static void apple_a7iop_unrealize(DeviceState *opaque)
{
    AppleA7IOP *s;

    s = APPLE_A7IOP(opaque);

    qdev_unrealize(DEVICE(s->iop_mailbox));
    qdev_unrealize(DEVICE(s->ap_mailbox));
}

static void apple_a7iop_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc;

    dc = DEVICE_CLASS(oc);
    device_class_set_legacy_reset(dc, apple_a7iop_reset);
    dc->realize = apple_a7iop_realize;
    dc->unrealize = apple_a7iop_unrealize;
    dc->desc = "Apple A7IOP";
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo apple_a7iop_info = {
    .name = TYPE_APPLE_A7IOP,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AppleA7IOP),
    .class_init = apple_a7iop_class_init,
};

static void apple_a7iop_register_types(void)
{
    type_register_static(&apple_a7iop_info);
}

type_init(apple_a7iop_register_types);
