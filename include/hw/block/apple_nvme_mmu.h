/*
 * Apple NVMe MMU Controller.
 *
 * Copyright (c) 2023-2025 Visual Ehrmanntraut (VisualEhrmanntraut).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef HW_BLOCK_APPLE_NVME_MMU_H
#define HW_BLOCK_APPLE_NVME_MMU_H

#include "hw/arm/apple-silicon/dtb.h"
#include "hw/nvme/nvme.h"
#include "hw/pci/pcie_host.h"

#define TYPE_APPLE_NVME_MMU "apple.nvme-mmu"
OBJECT_DECLARE_SIMPLE_TYPE(AppleNVMeMMUState, APPLE_NVME_MMU)

struct AppleNVMeMMUState {
    /*< private >*/
    PCIExpressHost parent_obj;

    /*< public >*/
    MemoryRegion io_mmio;
    MemoryRegion io_ioport;
    qemu_irq irq;
    NvmeCtrl nvme;
};

SysBusDevice *apple_nvme_mmu_create(DTBNode *node);

#endif /* HW_BLOCK_APPLE_NVME_MMU_H */
