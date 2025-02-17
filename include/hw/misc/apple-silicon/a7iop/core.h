#ifndef HW_MISC_APPLE_SILICON_A7IOP_CORE_H
#define HW_MISC_APPLE_SILICON_A7IOP_CORE_H

#include "qemu/osdep.h"
#include "hw/misc/apple-silicon/a7iop/base.h"
#include "hw/misc/apple-silicon/a7iop/mailbox/core.h"
#include "hw/sysbus.h"

#define CPU_CTRL_RUN BIT(4)
#define SEP_BOOT_MONITOR_RUN BIT(16)

#define TYPE_APPLE_A7IOP "apple-a7iop"
OBJECT_DECLARE_SIMPLE_TYPE(AppleA7IOP, APPLE_A7IOP)

typedef struct {
    void (*start)(AppleA7IOP *s);
    void (*wakeup)(AppleA7IOP *s);
} AppleA7IOPOps;

struct AppleA7IOP {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    AppleA7IOPVersion version;
    const char *role;
    MemoryRegion mmio;
    const AppleA7IOPOps *ops;
    QemuMutex lock;
    // qemu_irq iop_irq;
    // QTAILQ_HEAD(, AppleA7IOPInterruptStatusMessage) interrupt_status;
    AppleA7IOPMailbox *ap_mailbox;
    AppleA7IOPMailbox *iop_mailbox;
    uint32_t cpu_status;
    uint32_t cpu_ctrl;
};

void apple_a7iop_send_ap(AppleA7IOP *s, AppleA7IOPMessage *msg);
AppleA7IOPMessage *apple_a7iop_recv_ap(AppleA7IOP *s);
void apple_a7iop_send_iop(AppleA7IOP *s, AppleA7IOPMessage *msg);
AppleA7IOPMessage *apple_a7iop_recv_iop(AppleA7IOP *s);
void apple_a7iop_cpu_start(AppleA7IOP *s, bool wake);
uint32_t apple_a7iop_get_cpu_status(AppleA7IOP *s);
void apple_a7iop_set_cpu_status(AppleA7IOP *s, uint32_t value);
uint32_t apple_a7iop_get_cpu_ctrl(AppleA7IOP *s);
void apple_a7iop_set_cpu_ctrl(AppleA7IOP *s, uint32_t value);
// void apple_a7iop_interrupt_status_push(AppleA7IOP *s, uint32_t status);
void apple_a7iop_init(AppleA7IOP *s, const char *role, uint64_t mmio_size,
                      AppleA7IOPVersion version, const AppleA7IOPOps *ops,
                      QEMUBH *iop_bh);

#endif /* HW_MISC_APPLE_SILICON_A7IOP_CORE_H */
