#include "qemu/osdep.h"
#include "exec/address-spaces.h"
#include "qapi/error.h"
#include "qemu/lockable.h"
#include "qemu/log.h"
#include "private.h"

// #define IOP_DEBUG

#ifdef IOP_DEBUG
#define IOP_LOG_MSG(s, t, msg)                                                \
    do {                                                                      \
        qemu_log_mask(LOG_GUEST_ERROR,                                        \
                      "%s: %s message (msg->endpoint: 0x%X "                  \
                      "msg->data[0]: 0x" HWADDR_FMT_plx                       \
                      " msg->data[1]: 0x" HWADDR_FMT_plx ")\n",               \
                      s->role, t, msg->endpoint, msg->data[0], msg->data[1]); \
    } while (0)
#else
#define IOP_LOG_MSG(s, t, msg) \
    do {                       \
    } while (0)
#endif


#define REG_INT_MASK_SET 0x000
#define REG_INT_MASK_CLR 0x004
#define REG_IOP_CTRL 0x008
#define REG_AP_CTRL 0x00C
#define REG_IOP_SEND0 0x700
#define REG_IOP_SEND1 0x704
#define REG_IOP_SEND2 0x708
#define REG_IOP_SEND3 0x70C
#define REG_IOP_RECV0 0x710
#define REG_IOP_RECV1 0x714
#define REG_IOP_RECV2 0x718
#define REG_IOP_RECV3 0x71C
#define REG_AP_SEND0 0x720
#define REG_AP_SEND1 0x724
#define REG_AP_SEND2 0x728
#define REG_AP_SEND3 0x72C
#define REG_AP_RECV0 0x730
#define REG_AP_RECV1 0x734
#define REG_AP_RECV2 0x738
#define REG_AP_RECV3 0x73C

static void apple_a7iop_mailbox_reg_write_v4(void *opaque, hwaddr addr,
                                             const uint64_t data, unsigned size)
{
    AppleA7IOPMailbox *s;
    AppleA7IOPMessage *msg;
    struct sep_message sep_msg = { 0 };

    s = APPLE_A7IOP_MAILBOX(opaque);

    switch (addr) {
    case REG_INT_MASK_SET:
        apple_a7iop_mailbox_set_int_mask(s, (uint32_t)data);
        break;
    case REG_INT_MASK_CLR:
        apple_a7iop_mailbox_clear_int_mask(s, (uint32_t)data);
        break;
    case REG_IOP_CTRL:
        apple_a7iop_mailbox_set_iop_ctrl(s, (uint32_t)data);
        break;
    case REG_AP_CTRL:
        apple_a7iop_mailbox_set_ap_ctrl(s, (uint32_t)data);
        break;
    case REG_IOP_SEND0:
        QEMU_FALLTHROUGH;
    case REG_IOP_SEND1:
        QEMU_FALLTHROUGH;
    case REG_IOP_SEND2:
        QEMU_FALLTHROUGH;
    case REG_IOP_SEND3:
        qemu_mutex_lock(&s->lock);
        memcpy(s->iop_send_reg + (addr - REG_IOP_SEND0), &data, size);
        if (addr + size == REG_IOP_SEND3 + 4) {
            msg = g_new0(AppleA7IOPMessage, 1);
            memcpy(msg->data, s->iop_send_reg, sizeof(msg->data));
            if (!strncmp(s->role, "SEP", 3)) {
                memcpy(&sep_msg.raw, msg->data, 8);
                qemu_log_mask(LOG_UNIMP,
                              "%s: REG_IOP_SEND3: ep=0x%02x, tag=0x%02x, "
                              "opcode=0x%02x(%u), param=0x%02x, data=0x%08x\n",
                              s->role, sep_msg.endpoint, sep_msg.tag,
                              sep_msg.opcode, sep_msg.opcode, sep_msg.param,
                              sep_msg.data);
#if 1
                if (sep_msg.endpoint == 0xfe) {
                    L4InfoMessage l4_msg = { 0 };
                    memcpy(&l4_msg, msg->data, 8);
                    uint64_t shmbuf_addr = l4_msg.address << 12;
                    uint64_t shmbuf_size =
                        l4_msg.size << 12; // size << 12 is needed for L4
                                           // messages, but not for OOL messages
                    qemu_log_mask(LOG_UNIMP,
                                  "%s: REG_IOP_SEND3: L4_INFO_MSG: ep=0x%02x, "
                                  "tag=0x%02x, address=0x%" PRIx64
                                  ", size=0x%" PRIx64 "\n",
                                  s->role, sep_msg.endpoint, sep_msg.tag,
                                  shmbuf_addr, shmbuf_size);
                }
#endif
            }
            qemu_mutex_unlock(&s->lock);
            apple_a7iop_mailbox_send_iop(s, msg);
            if (strncmp(s->role, "SMC", 3) != 0) {
                IOP_LOG_MSG(s, "AP sent", msg);
            }
        } else {
            qemu_mutex_unlock(&s->lock);
        }
        break;
    case REG_AP_SEND0:
        QEMU_FALLTHROUGH;
    case REG_AP_SEND1:
        QEMU_FALLTHROUGH;
    case REG_AP_SEND2:
        QEMU_FALLTHROUGH;
    case REG_AP_SEND3:
        qemu_mutex_lock(&s->lock);
        memcpy(s->ap_send_reg + (addr - REG_AP_SEND0), &data, size);
        if (addr + size == REG_AP_SEND3 + 4) {
            msg = g_new0(AppleA7IOPMessage, 1);
            memcpy(msg->data, s->ap_send_reg, sizeof(msg->data));
            if (!strncmp(s->role, "SEP", 3)) {
                memcpy(&sep_msg.raw, msg->data, 8);
                qemu_log_mask(LOG_UNIMP,
                              "%s: REG_AP_SEND3: ep=0x%02x, tag=0x%02x, "
                              "opcode=0x%02x(%u), param=0x%02x, data=0x%08x\n",
                              s->role, sep_msg.endpoint, sep_msg.tag,
                              sep_msg.opcode, sep_msg.opcode, sep_msg.param,
                              sep_msg.data);
            }
            qemu_mutex_unlock(&s->lock);
            apple_a7iop_mailbox_send_ap(s, msg);
            IOP_LOG_MSG(s, "IOP sent", msg);
        } else {
            qemu_mutex_unlock(&s->lock);
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s unknown @ 0x" HWADDR_FMT_plx " value 0x%" PRIx64 "\n",
                      __FUNCTION__, addr, data);
        break;
    }
}

static uint64_t apple_a7iop_mailbox_reg_read_v4(void *opaque, hwaddr addr,
                                                unsigned size)
{
    AppleA7IOPMailbox *s;
    AppleA7IOPMessage *msg;
    struct sep_message sep_msg = { 0 };
    uint64_t ret = 0;

    s = APPLE_A7IOP_MAILBOX(opaque);

    switch (addr) {
    case REG_INT_MASK_SET:
        return apple_a7iop_mailbox_get_int_mask(s);
    case REG_INT_MASK_CLR:
        return ~apple_a7iop_mailbox_get_int_mask(s);
    case REG_IOP_CTRL:
        return apple_a7iop_mailbox_get_iop_ctrl(s);
    case REG_AP_CTRL:
        return apple_a7iop_mailbox_get_ap_ctrl(s);
    case REG_IOP_RECV0:
        msg = apple_a7iop_mailbox_recv_iop(s);
        WITH_QEMU_LOCK_GUARD(&s->lock)
        {
            if (msg) {
                memcpy(s->iop_recv_reg, msg->data, sizeof(s->iop_recv_reg));
                IOP_LOG_MSG(s, "IOP received", msg);
                if (!strncmp(s->role, "SEP", 3)) {
                    memcpy(&sep_msg.raw, msg->data, 8);
                    #if 0
                    qemu_log_mask(
                        LOG_UNIMP,
                        "%s: REG_IOP_RECV0: ep=0x%02x, tag=0x%02x, "
                        "opcode=0x%02x(%u), param=0x%02x, data=0x%08x\n",
                        s->role, sep_msg.endpoint, sep_msg.tag, sep_msg.opcode,
                        sep_msg.opcode, sep_msg.param, sep_msg.data);
                    #endif
                }
                g_free(msg);
            } else {
                memset(s->iop_recv_reg, 0, sizeof(s->iop_recv_reg));
            }
        }
        QEMU_FALLTHROUGH;
    case REG_IOP_RECV1:
        QEMU_FALLTHROUGH;
    case REG_IOP_RECV2:
        QEMU_FALLTHROUGH;
    case REG_IOP_RECV3:
        WITH_QEMU_LOCK_GUARD(&s->lock)
        {
            memcpy(&ret, s->iop_recv_reg + (addr - REG_IOP_RECV0), size);
        }
        break;
    case REG_AP_RECV0:
        msg = apple_a7iop_mailbox_recv_ap(s);
        WITH_QEMU_LOCK_GUARD(&s->lock)
        {
            if (msg) {
                memcpy(s->ap_recv_reg, msg->data, sizeof(s->ap_recv_reg));
                if (strncmp(s->role, "SMC", 3) != 0) {
                    IOP_LOG_MSG(s, "AP received", msg);
                }
                if (!strncmp(s->role, "SEP", 3)) {
                    memcpy(&sep_msg.raw, msg->data, 8);
                    qemu_log_mask(
                        LOG_UNIMP,
                        "%s: REG_AP_RECV0: ep=0x%02x, tag=0x%02x, "
                        "opcode=0x%02x(%u), param=0x%02x, data=0x%08x\n",
                        s->role, sep_msg.endpoint, sep_msg.tag, sep_msg.opcode,
                        sep_msg.opcode, sep_msg.param, sep_msg.data);
                }
                g_free(msg);
            } else {
                memset(s->ap_recv_reg, 0, sizeof(s->ap_recv_reg));
            }
        }
        QEMU_FALLTHROUGH;
    case REG_AP_RECV1:
        QEMU_FALLTHROUGH;
    case REG_AP_RECV2:
        QEMU_FALLTHROUGH;
    case REG_AP_RECV3:
        WITH_QEMU_LOCK_GUARD(&s->lock)
        {
            memcpy(&ret, s->ap_recv_reg + (addr - REG_AP_RECV0), size);
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s unknown @ 0x" HWADDR_FMT_plx "\n",
                      __FUNCTION__, addr);
        break;
    }

    return ret;
}

static const MemoryRegionOps apple_a7iop_mailbox_reg_ops_v4 = {
    .write = apple_a7iop_mailbox_reg_write_v4,
    .read = apple_a7iop_mailbox_reg_read_v4,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 8,
    .impl.min_access_size = 4,
    .impl.max_access_size = 8,
    .valid.unaligned = false,
};

void apple_a7iop_mailbox_init_mmio_v4(AppleA7IOPMailbox *s, const char *name)
{
    memory_region_init_io(&s->mmio, OBJECT(s), &apple_a7iop_mailbox_reg_ops_v4,
                          s, name, REG_AP_RECV3 + 4);
}
