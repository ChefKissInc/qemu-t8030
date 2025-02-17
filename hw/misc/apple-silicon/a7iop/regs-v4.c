#include "qemu/osdep.h"
#include "exec/hwaddr.h"
#include "exec/memory.h"
#include "hw/misc/apple-silicon/a7iop/core.h"
#include "hw/sysbus.h"
#include "qemu/log.h"
#include "private.h"

#define REG_CPU_CTRL 0x0044
#define REG_CPU_STATUS 0x0048

#define REG_V3_UNKNOWN0 (0x004c)
#define REG_V3_UNKNOWN1 (0x0818)
#define REG_V3_INTERRUPT_STATUS (0x081c) // "akf: READ IRQ %x"
#define REG_V3_UNKNOWN3 (0x080c)
#define REG_V3_UNKNOWN4 (0xC04) // NMI?
#define REG_V3_UNKNOWN5 (0xC10) // NMI?
#define REG_V3_UNKNOWN6 (0xC18) // NMI?
#define REG_V3_UNKNOWN7 (0x0040)
#define REG_V3_UNKNOWN8 (0x0004) // SEPROM: BIT0 Maybe a panic signal.

#define REG_SEP_AKF_DISABLE_INTERRUPT_BASE (0xa00)
#define REG_SEP_AKF_ENABLE_INTERRUPT_BASE (0xa80)

#define REG_NMI0 0xC04 // ??
#define REG_NMI1 0xC14 // ??
#define REG_AKF_CONFIG 0x2043 // ??
#define AKF_MAILBOX_OFF 0x100

static void apple_a7iop_reg_write(void *opaque, hwaddr addr,
                                  const uint64_t data, unsigned size)
{
    AppleA7IOP *s = APPLE_A7IOP(opaque);
    uint32_t interrupt_index = 0;

    switch (addr) {
    case REG_CPU_CTRL:
        apple_a7iop_set_cpu_ctrl(s, (uint32_t)data);
        break;
    case REG_SEP_AKF_DISABLE_INTERRUPT_BASE + 0x00: // group 0
    case REG_SEP_AKF_DISABLE_INTERRUPT_BASE + 0x04: // group 1
    case REG_SEP_AKF_DISABLE_INTERRUPT_BASE + 0x08: // group 2
    case REG_SEP_AKF_DISABLE_INTERRUPT_BASE + 0x0c: // group 3
#if 0
        qemu_log_mask(
            LOG_UNIMP,
            "%s AKF: SEP AKF DISABLE INTERRUPT write to 0x" HWADDR_FMT_plx
            " of value 0x" HWADDR_FMT_plx " lowest_bit_position: %u \n",
            s->role, addr, data, __builtin_ctzl(data));
#endif
        interrupt_index = (addr - REG_SEP_AKF_DISABLE_INTERRUPT_BASE) >> 2;
        s->iop_mailbox->interrupts_enabled[interrupt_index] &= ~data;

        break;
    case REG_SEP_AKF_ENABLE_INTERRUPT_BASE + 0x00: // group 0
    case REG_SEP_AKF_ENABLE_INTERRUPT_BASE + 0x04: // group 1
    case REG_SEP_AKF_ENABLE_INTERRUPT_BASE + 0x08: // group 2
    case REG_SEP_AKF_ENABLE_INTERRUPT_BASE + 0x0c: // group 3
#if 0
        qemu_log_mask(
            LOG_UNIMP,
            "%s AKF: SEP AKF ENABLE INTERRUPT write to 0x" HWADDR_FMT_plx
            " of value 0x" HWADDR_FMT_plx " lowest_bit_position: %u \n",
            s->role, addr, data, __builtin_ctzl(data));
#endif
        interrupt_index = (addr - REG_SEP_AKF_ENABLE_INTERRUPT_BASE) >> 2;
        s->iop_mailbox->interrupts_enabled[interrupt_index] |= data;
#if 0
        ap_update_irq(s);
        iop_update_irq(s);
#endif
        apple_a7iop_mailbox_update_irq(s->iop_mailbox);
        break;
    case REG_NMI1:
        break;
    case REG_V3_UNKNOWN6:
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "A7IOP(%s): Unknown write to 0x" HWADDR_FMT_plx
                      " of value 0x" HWADDR_FMT_plx "\n",
                      s->role, addr, data);
        break;
    }
}

static uint64_t apple_a7iop_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    AppleA7IOP *s = APPLE_A7IOP(opaque);
    uint64_t ret = 0;

    switch (addr) {
    case REG_CPU_CTRL:
        return apple_a7iop_get_cpu_ctrl(s);
    case REG_CPU_STATUS:
        return apple_a7iop_get_cpu_status(s);
    case REG_V3_UNKNOWN0:
        ret = 1;
        // TODO: response not interrupt available, but something with
        // REG_V3_CPU_CTRL?
        break;
    case REG_V3_UNKNOWN1:
        break;
    case REG_V3_INTERRUPT_STATUS: {
        AppleA7IOPMailbox *a7iop_mbox = s->iop_mailbox;
        uint32_t interrupt_status =
            apple_a7iop_interrupt_status_pop(a7iop_mbox);
        apple_a7iop_mailbox_update_irq_status(a7iop_mbox);
        if (interrupt_status) {
            ret = interrupt_status;
            qemu_log_mask(LOG_UNIMP,
                          "%s: REG_V3_INTERRUPT_STATUS: returning "
                          "interrupt_status: 0x%05" PRIX64 "\n",
                          s->role, ret);
        } else if (a7iop_mbox->iop_nonempty) {
            ret = 0x40000;
        } else if (a7iop_mbox->iop_empty) {
            ret = 0x40001;
        } else if (a7iop_mbox->ap_nonempty) {
            ret = 0x40002;
        } else if (a7iop_mbox->ap_empty) {
            ret = 0x40003;
        } else {
            ret = 0x70001;
        }
        break;
    }
    default:
        qemu_log_mask(LOG_UNIMP,
                      "A7IOP(%s): Unknown read from 0x" HWADDR_FMT_plx "\n",
                      s->role, addr);
        break;
    }

    return ret;
}

static const MemoryRegionOps apple_a7iop_reg_ops = {
    .write = apple_a7iop_reg_write,
    .read = apple_a7iop_reg_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 8,
    .impl.min_access_size = 4,
    .impl.max_access_size = 8,
    .valid.unaligned = false,
};

void apple_a7iop_init_mmio_v4(AppleA7IOP *s, uint64_t mmio_size)
{
    SysBusDevice *sbd;
    char name[32];

    sbd = SYS_BUS_DEVICE(s);

    snprintf(name, sizeof(name), TYPE_APPLE_A7IOP ".%s.regs", s->role);
    memory_region_init_io(&s->mmio, OBJECT(s), &apple_a7iop_reg_ops, s, name,
                          mmio_size);
    sysbus_init_mmio(sbd, &s->mmio);

    memory_region_add_subregion_overlap(&s->mmio, AKF_STRIDE + AKF_MAILBOX_OFF,
                                        &s->iop_mailbox->mmio, 1);
    memory_region_add_subregion_overlap(
        &s->mmio, AKF_STRIDE * 2 + AKF_MAILBOX_OFF, &s->ap_mailbox->mmio, 1);
}
