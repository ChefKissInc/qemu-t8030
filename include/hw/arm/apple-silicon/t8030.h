/*
 * Apple t8030 SoC.
 *
 * Copyright (c) 2023-2025 Visual Ehrmanntraut (VisualEhrmanntraut).
 * Copyright (c) 2023-2025 Christian Inci (chris-pcguy).
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

#ifndef HW_ARM_APPLE_SILICON_T8030_H
#define HW_ARM_APPLE_SILICON_T8030_H

#include "qemu/osdep.h"
#include "exec/hwaddr.h"
#include "hw/arm/apple-silicon/a13.h"
#include "hw/arm/apple-silicon/boot.h"
#include "hw/boards.h"
#include "hw/sysbus.h"
#include "sysemu/kvm.h"

#define TYPE_T8030 "t8030"

#define TYPE_T8030_MACHINE MACHINE_TYPE_NAME(TYPE_T8030)

#define T8030_MACHINE(obj) \
    OBJECT_CHECK(T8030MachineState, (obj), TYPE_T8030_MACHINE)

typedef struct {
    MachineClass parent;
} T8030MachineClass;

typedef enum {
    kBootModeAuto = 0,
    kBootModeManual,
    kBootModeEnterRecovery,
    kBootModeExitRecovery,
} BootMode;

typedef struct {
    MachineState parent;
    hwaddr soc_base_pa;
    hwaddr soc_size;

    unsigned long dram_size;
    AppleA13State *cpus[A13_MAX_CPU];
    AppleA13Cluster clusters[A13_MAX_CLUSTER];
    SysBusDevice *aic;
    MemoryRegion *sysmem;
    MachoHeader64 *kernel;
    DTBNode *device_tree;
    uint8_t *trustcache;
    AppleBootInfo bootinfo;
    AppleVideoArgs video_args;
    char *trustcache_filename;
    char *ticket_filename;
    char *seprom_filename;
    char *sep_fw_filename;
    BootMode boot_mode;
    uint32_t rtkit_protocol_ver;
    uint32_t build_version;
    uint64_t ecid;
    Notifier init_done_notifier;
    hwaddr panic_base;
    hwaddr panic_size;
    uint8_t pmgr_reg[0x100000];
    MemoryRegion amcc;
    uint8_t amcc_reg[0x100000];
    bool kaslr_off;
    bool force_dfu;
    uint32_t board_id;
} T8030MachineState;

#endif /* HW_ARM_APPLE_SILICON_T8030_H */
