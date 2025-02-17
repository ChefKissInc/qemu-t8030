#ifndef HW_MISC_APPLE_SILICON_A7IOP_MAILBOX_CORE_H
#define HW_MISC_APPLE_SILICON_A7IOP_MAILBOX_CORE_H

#include "qemu/osdep.h"
#include "hw/misc/apple-silicon/a7iop/base.h"
#include "hw/sysbus.h"
#include "qemu/queue.h"

#define TYPE_APPLE_A7IOP_MAILBOX "apple-a7iop-mailbox"
OBJECT_DECLARE_SIMPLE_TYPE(AppleA7IOPMailbox, APPLE_A7IOP_MAILBOX)

typedef struct AppleA7IOPMessage {
    union QEMU_PACKED {
        uint64_t data[2];
        struct QEMU_PACKED {
            uint64_t msg;
            uint32_t endpoint;
            uint32_t flags;
        };
    };
    QTAILQ_ENTRY(AppleA7IOPMessage) entry;
} AppleA7IOPMessage;

typedef struct AppleA7IOPInterruptStatusMessage {
    union QEMU_PACKED {
        uint32_t status;
    };
    QTAILQ_ENTRY(AppleA7IOPInterruptStatusMessage) entry;
} AppleA7IOPInterruptStatusMessage;

struct QEMU_PACKED sep_message {
    union {
        struct QEMU_PACKED {
            uint8_t             endpoint;
            uint8_t             tag;
            uint8_t             opcode;
            uint8_t             param;
            uint32_t            data;
        };
        uint64_t raw;
    };
};

typedef struct {
    uint8_t ep;
    uint8_t tag;
    uint8_t op;
    uint8_t param;
    uint32_t data;
} QEMU_PACKED SEPMessage;

typedef struct {
    uint8_t ep;
    uint8_t tag;
    uint8_t op;
    uint8_t id;
    uint32_t name;
} QEMU_PACKED EPAdvertisementMessage;

typedef struct {
    uint8_t ep;
    uint8_t tag;
    uint16_t size;
    uint32_t address;
} QEMU_PACKED L4InfoMessage;

typedef struct {
    uint8_t ep;
    uint8_t tag;
    uint8_t op;
    uint8_t id;
    uint32_t data;
} QEMU_PACKED SetOOLMessage;


struct AppleA7IOPMailbox {
    /*< private >*/
    SysBusDevice parent_obj;

    const char *role;
    QemuMutex lock;
    MemoryRegion mmio;
    QEMUBH *bh;
    QTAILQ_HEAD(, AppleA7IOPMessage) inbox;
    QTAILQ_HEAD(, AppleA7IOPInterruptStatusMessage) interrupt_status;
    size_t count;
    AppleA7IOPMailbox *iop_mailbox;
    AppleA7IOPMailbox *ap_mailbox;
    qemu_irq irqs[APPLE_A7IOP_IRQ_MAX];
    qemu_irq iop_irq;
    bool iop_dir_en;
    bool ap_dir_en;
    bool underflow;
    uint32_t int_mask;
    uint8_t iop_recv_reg[16];
    uint8_t ap_recv_reg[16];
    uint8_t iop_send_reg[16];
    uint8_t ap_send_reg[16];
    uint32_t interrupts_enabled[4];
    bool iop_nonempty;
    bool iop_empty;
    bool ap_nonempty;
    bool ap_empty;
    uint64_t last_ool_in_size;
    uint64_t last_ool_in_addr;
    uint64_t last_ool_out_size;
    uint64_t last_ool_out_addr;
    int ool_overlap_priority;
};

void apple_a7iop_mailbox_update_irq_status(AppleA7IOPMailbox *s);
void apple_a7iop_mailbox_update_irq(AppleA7IOPMailbox *s);
bool apple_a7iop_mailbox_is_empty(AppleA7IOPMailbox *s);
void apple_a7iop_mailbox_send_ap(AppleA7IOPMailbox *s, AppleA7IOPMessage *msg);
void apple_a7iop_mailbox_send_iop(AppleA7IOPMailbox *s, AppleA7IOPMessage *msg);
AppleA7IOPMessage *apple_a7iop_inbox_peek(AppleA7IOPMailbox *s);
void apple_a7iop_interrupt_status_push(AppleA7IOPMailbox *s, uint32_t status);
uint32_t apple_a7iop_interrupt_status_pop(AppleA7IOPMailbox *s);
AppleA7IOPMessage *apple_a7iop_mailbox_recv_iop(AppleA7IOPMailbox *s);
AppleA7IOPMessage *apple_a7iop_mailbox_recv_ap(AppleA7IOPMailbox *s);
AppleA7IOPMailbox *apple_a7iop_mailbox_new(const char *role,
                                           AppleA7IOPVersion version,
                                           AppleA7IOPMailbox *iop_mailbox,
                                           AppleA7IOPMailbox *ap_mailbox,
                                           QEMUBH *bh);

#endif /* HW_MISC_APPLE_SILICON_A7IOP_MAILBOX_CORE_H */
