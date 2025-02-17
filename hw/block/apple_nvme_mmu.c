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

#include "qemu/osdep.h"
#include "hw/block/apple_nvme_mmu.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "qapi/error.h"

static void apple_nvme_mmu_set_irq(void *opaque, int irq_num, int level)
{
    AppleNVMeMMUState *s = APPLE_NVME_MMU(opaque);
    qemu_set_irq(s->irq, level);
}

SysBusDevice *apple_nvme_mmu_create(DTBNode *node)
{
    DeviceState *dev;
    AppleNVMeMMUState *s;
    PCIHostState *pci;
    SysBusDevice *sbd;
    PCIExpressHost *pex;
    DTBProp *prop;
    uint64_t *reg;

    dev = qdev_new(TYPE_APPLE_NVME_MMU);
    s = APPLE_NVME_MMU(dev);
    pci = PCI_HOST_BRIDGE(dev);
    sbd = SYS_BUS_DEVICE(dev);
    pex = PCIE_HOST_BRIDGE(dev);

    object_initialize_child(OBJECT(dev), "nvme", &s->nvme, TYPE_NVME);

    object_property_set_str(OBJECT(&s->nvme), "serial", "ChefKiss-NVMeMMU",
                            &error_fatal);
    object_property_set_uint(OBJECT(&s->nvme), "max_ioqpairs", 7, &error_fatal);
    object_property_set_uint(OBJECT(&s->nvme), "mdts", 8, &error_fatal);
    object_property_set_uint(OBJECT(&s->nvme), "logical_block_size", 4096,
                             &error_fatal);
    object_property_set_uint(OBJECT(&s->nvme), "physical_block_size", 4096,
                             &error_fatal);

    pcie_host_mmcfg_init(pex, PCIE_MMCFG_SIZE_MAX);

    prop = dtb_find_prop(node, "reg");
    assert(prop);

    reg = (uint64_t *)prop->data;

    memory_region_init(&s->io_mmio, OBJECT(s), "nvme_mmu_pci_mmio", reg[1]);
    sysbus_init_mmio(sbd, &s->io_mmio);
    memory_region_init(&s->io_ioport, OBJECT(s), "nvme_mmu_pci_ioport",
                       64 * 1024);

    pci->bus = pci_register_root_bus(
        dev, "nvme_mmu_pcie.0", apple_nvme_mmu_set_irq, pci_swizzle_map_irq_fn,
        s, &s->io_mmio, &s->io_ioport, 0, 4, TYPE_PCIE_BUS);
    sysbus_init_irq(sbd, &s->irq);

    return sbd;
}

static void apple_nvme_mmu_realize(DeviceState *dev, Error **errp)
{
    AppleNVMeMMUState *s = APPLE_NVME_MMU(dev);
    PCIHostState *pci = PCI_HOST_BRIDGE(dev);

    pci_realize_and_unref(PCI_DEVICE(&s->nvme), pci->bus, &error_fatal);
}

static void apple_nvme_mmu_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = apple_nvme_mmu_realize;
    dc->desc = "Apple NVMe MMU";
    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
    dc->fw_name = "pci";
}

static const TypeInfo apple_nvme_mmu_info = {
    .name = TYPE_APPLE_NVME_MMU,
    .parent = TYPE_PCIE_HOST_BRIDGE,
    .instance_size = sizeof(AppleNVMeMMUState),
    .class_init = apple_nvme_mmu_class_init,
};

static void apple_nvme_mmu_register_types(void)
{
    type_register_static(&apple_nvme_mmu_info);
}

type_init(apple_nvme_mmu_register_types);
