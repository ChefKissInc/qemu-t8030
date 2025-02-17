#ifndef HW_MISC_APPLE_SILICON_SMC_H
#define HW_MISC_APPLE_SILICON_SMC_H

#include "qemu/osdep.h"
#include "hw/arm/apple-silicon/dtb.h"
#include "hw/misc/apple-silicon/a7iop/base.h"
#include "hw/sysbus.h"

SysBusDevice *apple_smc_create(DTBNode *node, AppleA7IOPVersion version,
                               uint32_t protocol_version);

#endif /* HW_MISC_APPLE_SILICON_SMC_H */
