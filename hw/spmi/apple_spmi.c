#include "qemu/osdep.h"
#include "hw/arm/apple-silicon/dtb.h"
#include "hw/irq.h"
#include "hw/spmi/apple_spmi.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"

// #define DEBUG_SPMI

#define APPLE_SPMI_RESP_IRQ "apple-spmi-resp"

#define SPMI_QUEUE_DEPTH (0x100)

#define SPMI_CONTROL_QUEUE_RESET (0x14)
#define SPMI_CONTROL_QUEUE_RESET_REQ (1 << 0)
#define SPMI_CONTROL_QUEUE_RESET_RSP (1 << 1)

#define SPMI_QUEUE_STATUS (0x0)
#define SPMI_QUEUE_STATUS_REQ_EMPTY (1 << 8)
#define SPMI_QUEUE_STATUS_RSP_EMPTY (1 << 24)

#define SPMI_REQ_QUEUE_PUSH (0x4)
#define SPMI_REQ_ADDR_SHIFT (16)
#define SPMI_REQ_SID_SHIFT (8)
#define SPMI_REQ_SID(_x) (((_x) >> 8) & 0xf)
#define SPMI_OPCODE_SHIFT (0)
#define SPMI_REQ_FINAL (1 << 15)

#define SPMI_RSP_QUEUE_POP (0x8)
#define SPMI_RSP_ACK_MASK (0xff)
#define SPMI_RSP_ACK_SHIFT (16)

#define SPMI_NUM_IRQ_BANK (9)
#define SPMI_NUM_IRQ (9 * 32)
#define SPMI_INT_STATUS_V0(_b) (0x40 + (_b) * 4)
#define SPMI_INT_STATUS_V1(_b) (0x60 + (_b) * 4)
#define SPMI_INT_ENAB(_b) (0x20 + (_b) * 4)

#define SPMI_FAULT_UNMAPPED_WRITE (0x00)
#define SPMI_FAULT_UNSUPPORTED_READ (0x04)
#define SPMI_FAULT_NO_RESPONSE (0x08)
#define SPMI_FAULT_CMD_PARITY_BOM (0x10)
#define SPMI_FAULT_CMD_PARITY_NOT_BOM (0x14)
#define SPMI_FAULT_ADDR_PARITY (0x18)
#define SPMI_FAULT_DATA_PARITY (0x1C)
#define SPMI_FAULT_UNRECOGNIZED_CMD (0x20)
#define SPMI_FAULT_MPL_COLLISION (0x24)
#define SPMI_FAULT_HW_0_REQ_OVFL (0x28)
#define SPMI_FAULT_HW_1_REQ_OVFL (0x2C)
#define SPMI_FAULT_BACKLIGHT_REQ_OVFL (0x30)
#define SPMI_FAULT_SW_0_REQ_OVFL (0x34)
#define SPMI_FAULT_SW_1_REQ_OVFL (0x38)
#define SPMI_FAULT_SW_2_REQ_OVFL (0x3C)
#define SPMI_FAULT_SW_3_REQ_OVFL (0x40)
#define SPMI_FAULT_HW_0_RSP_UDFL (0x44)
#define SPMI_FAULT_HW_1_RSP_UDFL (0x48)
#define SPMI_FAULT_BACKLIGHT_RSP_UDFL (0x4C)
#define SPMI_FAULT_SW_0_RSP_UDFL (0x50)
#define SPMI_FAULT_SW_1_RSP_UDFL (0x54)
#define SPMI_FAULT_SW_2_RSP_UDFL (0x58)
#define SPMI_FAULT_SW_3_RSP_UDFL (0x5C)
#define SPMI_FAULT_FAULT_RSP_UDFL (0x60)

#define SPMI_CONTROL_REGS_START (0x000)
#define SPMI_CONTROL_REGS_END (0x100)
#define SPMI_QUEUE_REGS_START (0x700)
#define SPMI_QUEUE_REGS_END (0x800)
#define SPMI_FAULT_REGS_START (0xd00)
#define SPMI_FAULT_REGS_END (0xe00)
#define SPMI_FAULT_COUNTER_REGS_START (0xe00)
#define SPMI_FAULT_COUNTER_REGS_END (0xe64)

#define SPMI_RESP_IRQ (0xc0)

static inline int spmi_opcode(uint32_t spmi_request)
{
    switch ((spmi_request >> 4) & 0xf) {
    case 1:
        return spmi_request & 0xff;
    default:
        return spmi_request & 0xf8;
    }
}

static inline int spmi_data_length(uint32_t spmi_request)
{
    switch ((spmi_request >> 4) & 0xf) {
    case 1:
        return 0;
    default:
        return (spmi_request & 0x7) + 1;
    }
}

static inline int spmi_address(uint32_t spmi_request)
{
    switch (spmi_opcode(spmi_request)) {
    case SPMI_CMD_EXT_WRITE:
    case SPMI_CMD_EXT_READ:
    case SPMI_CMD_EXT_WRITEL:
    case SPMI_CMD_EXT_READL:
        return (spmi_request >> SPMI_REQ_ADDR_SHIFT) & 0xffff;
    default:
        return (spmi_request >> SPMI_REQ_ADDR_SHIFT) & 0xff;
    }
}

static void apple_spmi_update_irq(AppleSPMIState *s)
{
    int level = false;
    for (int i = 0; i < SPMI_NUM_IRQ_BANK && !level; i++) {
        uint32_t status = 0;
        switch (s->reg_vers) {
        case 0:
            status = s->queue_reg[SPMI_INT_STATUS_V0(i) >> 2];
            break;
        case 1:
            status = s->queue_reg[SPMI_INT_STATUS_V1(i) >> 2];
            break;
        default:
            g_assert_not_reached();
            break;
        }
        if (status & s->queue_reg[SPMI_INT_ENAB(i) >> 2]) {
            level = 1;
        }
    }

    qemu_set_irq(s->irq, level);
}

static void apple_spmi_set_irq(void *opaque, int irq, int level)
{
    AppleSPMIState *s = APPLE_SPMI(opaque);
    uint32_t *status = NULL;
    switch (s->reg_vers) {
    case 0:
        status = &s->queue_reg[SPMI_INT_STATUS_V0(irq >> 5) >> 2];
        break;
    case 1:
        status = &s->queue_reg[SPMI_INT_STATUS_V1(irq >> 5) >> 2];
        break;
    default:
        g_assert_not_reached();
        break;
    }
    if (level) {
        *status |= (1 << (irq & 31));
    } else {
        *status &= ~(1 << (irq & 31));
    }
    apple_spmi_update_irq(s);
}

static void apple_spmi_update_queues_status(AppleSPMIState *s)
{
    if (!fifo32_is_empty(&s->resp_fifo)) {
        qemu_irq_raise(s->resp_irq);
    } else {
        qemu_irq_lower(s->resp_irq);
    }
}

static void apple_spmi_queue_reg_write(void *opaque, hwaddr addr, uint64_t data,
                                       unsigned size)
{
    AppleSPMIState *s = APPLE_SPMI(opaque);
    uint32_t value = data;
    uint32_t *mmio = &s->queue_reg[addr >> 2];
    bool iflg = false;
    bool qflg = false;
#ifdef DEBUG_SPMI
    qemu_log_mask(LOG_UNIMP,
                  "%s: %s @ 0x" HWADDR_FMT_plx " value: 0x" HWADDR_FMT_plx "\n",
                  DEVICE(s)->id, __func__, addr, data);
#endif

    switch (addr) {
    case SPMI_REQ_QUEUE_PUSH: {
        if (s->data == NULL) {
            uint8_t sid = SPMI_REQ_SID(value);
            uint8_t opc = spmi_opcode(value);
            uint32_t addr2 = spmi_address(value);
            bool parity = !(value & SPMI_REQ_FINAL);
            uint32_t len = spmi_data_length(value);

            s->command = value;
#ifdef DEBUG_SPMI
            qemu_log_mask(LOG_UNIMP,
                          "%s: sid: 0x%x opc: 0x%x addr: 0x%x len: 0x%x\n",
                          DEVICE(s)->id, sid, opc, addr2, len);
#endif

            if (opc == SPMI_CMD_EXT_WRITE || opc == SPMI_CMD_EXT_WRITEL) {
                s->data_length = (len + 3) / 4;
                s->data_filled = 0;
                s->data = g_new0(uint32_t, s->data_length);
            }
            if (spmi_start_transfer(s->bus, sid, opc, addr2)) {
                return;
            }
            if (s->data == NULL && len) {
                assert(opc == SPMI_CMD_EXT_READ || opc == SPMI_CMD_EXT_READL);
                g_autofree uint32_t *data2 = g_malloc0(len + 3);
                int count = spmi_recv(s->bus, (uint8_t *)data2, len);
                uint8_t ack = 0;
                value &= 0xFFF;
                if (count > 0) {
                    ack = ~(-1 << count);
                }
                value |= (ack << SPMI_RSP_ACK_SHIFT);
                fifo32_push(&s->resp_fifo, value);
                for (int i = 0; i < (len + 3) / 4; i++) {
                    fifo32_push(&s->resp_fifo, data2[i]);
                }
            }
            if (s->data == NULL && !parity) {
                spmi_end_transfer(s->bus);
                qflg = 1;
                iflg = 1;
            }
        } else {
            s->data[s->data_filled++] = value;
            if (s->data_filled >= s->data_length) {
                uint32_t requested_len = spmi_data_length(s->command);
                uint32_t count =
                    spmi_send(s->bus, (uint8_t *)s->data, requested_len);
                fifo32_push(&s->resp_fifo,
                            (s->command & 0xFFF) |
                                ((count == requested_len) << 15));
                g_free(s->data);
                s->data = NULL;
                s->data_length = 0;
                if (s->command & SPMI_REQ_FINAL) {
                    spmi_end_transfer(s->bus);
                    qflg = 1;
                    iflg = 1;
                }
            }
        }
        break;
    }
    case SPMI_INT_ENAB(0)... SPMI_INT_ENAB(SPMI_NUM_IRQ_BANK - 1):
        iflg = true;
        break;
    case SPMI_INT_STATUS_V1(0)... SPMI_INT_STATUS_V1(SPMI_NUM_IRQ_BANK - 1):
        value = (*mmio) & (~value);
        iflg = true;
        break;
    default:
        break;
    }
    *mmio = value;
    if (qflg) {
        apple_spmi_update_queues_status(s);
    }
    if (iflg) {
        apple_spmi_update_irq(s);
    }
}

static uint64_t apple_spmi_queue_reg_read(void *opaque, hwaddr addr,
                                          unsigned size)
{
    AppleSPMIState *s = APPLE_SPMI(opaque);
    bool qflg = false;
    bool iflg = false;
    uint32_t value = 0;
    value = s->queue_reg[addr >> 2];

    switch (addr) {
    case SPMI_RSP_QUEUE_POP:
        if (fifo32_is_empty(&s->resp_fifo)) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: rsp queue empty\n",
                          DEVICE(s)->id);
            value = 0;
        } else {
            value = fifo32_pop(&s->resp_fifo);
        }
        qflg = true;
        iflg = true;
        break;
    case SPMI_QUEUE_STATUS:
        value &= ~(SPMI_QUEUE_STATUS_REQ_EMPTY | SPMI_QUEUE_STATUS_RSP_EMPTY);
        value |= SPMI_QUEUE_STATUS_REQ_EMPTY;
        if (fifo32_is_empty(&s->resp_fifo)) {
            value |= SPMI_QUEUE_STATUS_RSP_EMPTY;
        }
        break;
    default:
        break;
    }

    if (qflg) {
        apple_spmi_update_queues_status(s);
        iflg = true;
    }
    if (iflg) {
        apple_spmi_update_irq(s);
    }
#ifdef DEBUG_SPMI
    qemu_log_mask(LOG_UNIMP, "%s: %s @ 0x" HWADDR_FMT_plx ": 0x%x\n",
                  DEVICE(s)->id, __func__, addr, value);
#endif
    return value;
}

static const MemoryRegionOps apple_spmi_queue_reg_ops = {
    .write = apple_spmi_queue_reg_write,
    .read = apple_spmi_queue_reg_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned = false,
};

static uint64_t apple_spmi_control_read(void *opaque, hwaddr addr,
                                        unsigned size)
{
    AppleSPMIState *s = APPLE_SPMI(opaque);
    bool qflg = false;
    bool iflg = false;
    uint32_t value = 0;
    value = s->control_reg[addr >> 2];

    switch (addr) {
    default:
        break;
    }

    if (qflg) {
        apple_spmi_update_queues_status(s);
    }
    if (iflg) {
        apple_spmi_update_irq(s);
    }
#ifdef DEBUG_SPMI
    qemu_log_mask(LOG_UNIMP, "%s: %s @ 0x" HWADDR_FMT_plx ": 0x%x\n", __func__,
                  DEVICE(s)->id, addr, value);
#endif
    return value;
}

static void apple_spmi_control_write(void *opaque, hwaddr addr, uint64_t data,
                                     unsigned size)
{
    AppleSPMIState *s = APPLE_SPMI(opaque);
    uint32_t value = data;
    uint32_t *mmio = &s->control_reg[addr >> 2];
    bool iflg = false;
    bool qflg = false;
#ifdef DEBUG_SPMI
    qemu_log_mask(LOG_UNIMP,
                  "%s: %s @ 0x" HWADDR_FMT_plx " value: 0x" HWADDR_FMT_plx "\n",
                  DEVICE(s)->id, __func__, addr, data);
#endif

    switch (addr) {
    case SPMI_CONTROL_QUEUE_RESET:
        if (value & SPMI_CONTROL_QUEUE_RESET_RSP) {
            fifo32_reset(&s->resp_fifo);
            value &= ~SPMI_CONTROL_QUEUE_RESET_RSP;
        }
        value &= ~SPMI_CONTROL_QUEUE_RESET_REQ;
        qflg = true;
        break;
    default:
        break;
    }

    *mmio = value;
    if (qflg) {
        apple_spmi_update_queues_status(s);
    }
    if (iflg) {
        apple_spmi_update_irq(s);
    }
}

static const MemoryRegionOps apple_spmi_control_ops = {
    .read = apple_spmi_control_read,
    .write = apple_spmi_control_write,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static uint64_t apple_spmi_fault_read(void *opaque, hwaddr addr, unsigned size)
{
    AppleSPMIState *s = APPLE_SPMI(opaque);
    bool qflg = false;
    bool iflg = false;
    uint32_t value = 0;
    value = s->fault_reg[addr >> 2];

    switch (addr) {
    case SPMI_QUEUE_STATUS:
        value &= ~(SPMI_QUEUE_STATUS_REQ_EMPTY | SPMI_QUEUE_STATUS_RSP_EMPTY);
        value |= SPMI_QUEUE_STATUS_REQ_EMPTY;
        value |= SPMI_QUEUE_STATUS_RSP_EMPTY;
        break;
    default:
        break;
    }

    if (qflg) {
        apple_spmi_update_queues_status(s);
    }
    if (iflg) {
        apple_spmi_update_irq(s);
    }
#ifdef DEBUG_SPMI
    qemu_log_mask(LOG_UNIMP, "%s: %s @ 0x" HWADDR_FMT_plx ": 0x%x\n", __func__,
                  DEVICE(s)->id, addr, value);
#endif
    return value;
}

static void apple_spmi_fault_write(void *opaque, hwaddr addr, uint64_t data,
                                   unsigned size)
{
    AppleSPMIState *s = APPLE_SPMI(opaque);
    uint32_t value = data;
    uint32_t *mmio = &s->fault_reg[addr >> 2];
    bool iflg = false;
    bool qflg = false;
#ifdef DEBUG_SPMI
    qemu_log_mask(LOG_UNIMP,
                  "%s: %s @ 0x" HWADDR_FMT_plx " value: 0x" HWADDR_FMT_plx "\n",
                  DEVICE(s)->id, __func__, addr, data);
#endif

    switch (addr) {
    default:
        break;
    }

    *mmio = value;
    if (qflg) {
        apple_spmi_update_queues_status(s);
    }
    if (iflg) {
        apple_spmi_update_irq(s);
    }
}

static const MemoryRegionOps apple_spmi_fault_ops = {
    .read = apple_spmi_fault_read,
    .write = apple_spmi_fault_write,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void apple_spmi_reset_enter(Object *obj, ResetType type)
{
    AppleSPMIState *s = APPLE_SPMI(obj);
    AppleSPMIClass *c = APPLE_SPMI_GET_CLASS(obj);

    if (c->parent_phases.enter) {
        c->parent_phases.enter(obj, type);
    }
    memset(s->control_reg, 0, sizeof(s->control_reg));
    memset(s->queue_reg, 0, sizeof(s->queue_reg));
    memset(s->fault_reg, 0, sizeof(s->fault_reg));
    memset(s->fault_counter_reg, 0, sizeof(s->fault_counter_reg));
    fifo32_reset(&s->resp_fifo);
    if (s->data) {
        g_free(s->data);
    }
    s->data = NULL;
    s->data_length = 0;
}

static void apple_spmi_reset_exit(Object *obj, ResetType type)
{
    AppleSPMIState *s = APPLE_SPMI(obj);
    AppleSPMIClass *c = APPLE_SPMI_GET_CLASS(obj);

    if (c->parent_phases.exit != NULL) {
        c->parent_phases.exit(obj, type);
    }
    apple_spmi_update_queues_status(s);
    apple_spmi_update_irq(s);
}

static void apple_spmi_realize(DeviceState *dev, Error **errp)
{
    AppleSPMIState *s = APPLE_SPMI(dev);
    char bus_name[32] = { 0 };

    snprintf(bus_name, sizeof(bus_name), "%s.bus", dev->id);
    s->bus = spmi_init_bus(dev, (const char *)bus_name);

    qdev_connect_gpio_out_named(dev, APPLE_SPMI_RESP_IRQ, 0,
                                qdev_get_gpio_in(dev, s->resp_intr_index));
}

static void apple_spmi_init(Object *obj)
{
    AppleSPMIState *s = APPLE_SPMI(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    DeviceState *dev = DEVICE(obj);

    s->reg_vers = 1;
    s->resp_intr_index = SPMI_RESP_IRQ;

    fifo32_create(&s->resp_fifo, SPMI_QUEUE_DEPTH);

    memory_region_init_io(&s->iomems[0], obj, &apple_spmi_queue_reg_ops, s,
                          TYPE_APPLE_SPMI ".queue_reg", sizeof(s->queue_reg));

    memory_region_init_io(&s->iomems[1], obj, &apple_spmi_fault_ops, s,
                          TYPE_APPLE_SPMI ".fault_reg", sizeof(s->fault_reg));

    memory_region_init_ram_device_ptr(
        &s->iomems[2], obj, TYPE_APPLE_SPMI ".fault_counter_reg",
        sizeof(s->fault_counter_reg), &s->fault_counter_reg);

    memory_region_init_io(&s->iomems[3], obj, &apple_spmi_control_ops, s,
                          TYPE_APPLE_SPMI ".control_reg",
                          sizeof(s->control_reg));

    memory_region_init(&s->container, obj, TYPE_APPLE_SPMI ".reg",
                       APPLE_SPMI_MMIO_SIZE);

    memory_region_add_subregion(&s->container, SPMI_QUEUE_REGS_START,
                                &s->iomems[0]);
    memory_region_add_subregion(&s->container, SPMI_FAULT_REGS_START,
                                &s->iomems[1]);
    memory_region_add_subregion(&s->container, SPMI_FAULT_COUNTER_REGS_START,
                                &s->iomems[2]);
    memory_region_add_subregion(&s->container, SPMI_CONTROL_REGS_START,
                                &s->iomems[3]);
    sysbus_init_mmio(sbd, &s->container);

    sysbus_init_irq(sbd, &s->irq);

    qdev_init_gpio_in(dev, apple_spmi_set_irq, SPMI_NUM_IRQ);

    qdev_init_gpio_out_named(dev, &s->resp_irq, APPLE_SPMI_RESP_IRQ, 1);
}

SysBusDevice *apple_spmi_create(DTBNode *node)
{
    DeviceState *dev;
    AppleSPMIState *s;
    SysBusDevice *sbd;
    DTBProp *prop;
    uint32_t phandle;

    dev = qdev_new(TYPE_APPLE_SPMI);
    s = APPLE_SPMI(dev);
    sbd = SYS_BUS_DEVICE(dev);

    prop = dtb_find_prop(node, "name");
    dev->id = g_strdup((const char *)prop->data);

    prop = dtb_find_prop(node, "reg-vers");
    if (prop) {
        s->reg_vers = *(uint32_t *)prop->data;
    }

    /* XXX: There is a register overlapping issue (STS and ENAB) with reg v0 */
    assert(s->reg_vers != 0);

    prop = dtb_find_prop(node, "AAPL,phandle");

    phandle = *(uint32_t *)prop->data;

    prop = dtb_find_prop(node, "interrupts");

    s->resp_intr_index = *(uint32_t *)prop->data;

    prop = dtb_find_prop(node, "interrupt-parent");
    /* The first interrupt in list (response) should be self-wired */
    assert(*(uint32_t *)prop->data == phandle);

    return sbd;
}

static const VMStateDescription vmstate_apple_spmi = {
    .name = "apple_spmi",
    .fields =
        (VMStateField[]){
            VMSTATE_FIFO32(resp_fifo, AppleSPMIState),
            VMSTATE_UINT32_ARRAY(control_reg, AppleSPMIState,
                                 0x100 / sizeof(uint32_t)),
            VMSTATE_UINT32_ARRAY(queue_reg, AppleSPMIState,
                                 0x100 / sizeof(uint32_t)),
            VMSTATE_UINT32_ARRAY(fault_reg, AppleSPMIState,
                                 0x100 / sizeof(uint32_t)),
            VMSTATE_UINT32_ARRAY(fault_counter_reg, AppleSPMIState,
                                 0x64 / sizeof(uint32_t)),
            VMSTATE_UINT32(data_length, AppleSPMIState),
            VMSTATE_UINT32(data_filled, AppleSPMIState),
            VMSTATE_UINT32(command, AppleSPMIState),
            VMSTATE_VARRAY_UINT32_ALLOC(data, AppleSPMIState, data_length, 0,
                                        vmstate_info_uint32, uint32_t),
            VMSTATE_END_OF_LIST(),
        }
};

static void apple_spmi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    AppleSPMIClass *c = APPLE_SPMI_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = apple_spmi_realize;
    dc->desc = "Apple SPMI Controller";
    dc->vmsd = &vmstate_apple_spmi;
    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
    resettable_class_set_parent_phases(rc, apple_spmi_reset_enter, NULL,
                                       apple_spmi_reset_exit,
                                       &c->parent_phases);
}

static const TypeInfo apple_spmi_info = {
    .name = TYPE_APPLE_SPMI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AppleSPMIState),
    .instance_init = apple_spmi_init,
    .class_size = sizeof(AppleSPMIClass),
    .class_init = apple_spmi_class_init,
};

static void apple_spmi_register_types(void)
{
    type_register_static(&apple_spmi_info);
}

type_init(apple_spmi_register_types);
