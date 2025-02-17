/*
 * Apple s8000 SoC.
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

#include "qemu/osdep.h"
#include "exec/address-spaces.h"
#include "exec/memory.h"
#include "hw/arm/apple-silicon/a9.h"
#include "hw/arm/apple-silicon/mem.h"
#include "hw/arm/apple-silicon/s8000-config.c.inc"
#include "hw/arm/apple-silicon/s8000.h"
#include "hw/arm/apple-silicon/sep-sim.h"
#include "hw/arm/apple-silicon/xnu_pf.h"
#include "hw/arm/exynos4210.h"
#include "hw/block/apple_nvme_mmu.h"
#include "hw/display/adbe_v2.h"
#include "hw/gpio/apple_gpio.h"
#include "hw/i2c/apple_i2c.h"
#include "hw/intc/apple_aic.h"
#include "hw/irq.h"
#include "hw/misc/apple-silicon/aes.h"
#include "hw/misc/pmu_d2255.h"
#include "hw/nvram/apple_nvram.h"
#include "hw/qdev-core.h"
#include "hw/ssi/apple_spi.h"
#include "hw/ssi/ssi.h"
#include "hw/usb/apple_otg.h"
#include "hw/watchdog/apple_wdt.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/error-report.h"
#include "qemu/guest-random.h"
#include "qemu/units.h"
#include "sysemu/reset.h"
#include "sysemu/runstate.h"
#include "sysemu/sysemu.h"
#include "target/arm/arm-powerctl.h"

#define S8000_SPI0_IRQ 188

#define S8000_GPIO_HOLD_KEY 97
#define S8000_GPIO_MENU_KEY 96
#define S8000_GPIO_SPI0_CS 106
#define S8000_GPIO_FORCE_DFU 123
#define S8000_GPIO_DFU_STATUS 136

#define S8000_DRAM_BASE 0x800000000ull
#define S8000_DRAM_SIZE (2 * GiB)

#define S8000_SPI0_BASE 0xA080000ull

#define S8000_SRAM_BASE 0x180000000ull
#define S8000_SRAM_SIZE 0x400000ull

#define S8000_SEPROM_BASE 0x20D000000ull
#define S8000_SEPROM_SIZE 0x1000000ull

// Carveout region 0xC
#define S8000_PANIC_BASE (S8000_DRAM_BASE + 0x7F374000ull)
#define S8000_PANIC_SIZE 0x80000ull

// Carveout region 0x50
#define S8000_REGION_50_SIZE 0x18000ull
#define S8000_REGION_50_BASE (S8000_PANIC_BASE - S8000_REGION_50_SIZE)

// Carveout region 0xE
#define S8000_DISPLAY_SIZE 0x854000ull
#define S8000_DISPLAY_BASE (S8000_REGION_50_BASE - S8000_DISPLAY_SIZE)

// Carveout region 0x4
#define S8000_TZ0_SIZE 0x1E00000ull
#define S8000_TZ0_BASE (S8000_DISPLAY_BASE - S8000_TZ0_SIZE)

// Carveout region 0x6
#define S8000_TZ1_SIZE 0x80000ull
#define S8000_TZ1_BASE (S8000_TZ0_BASE - S8000_TZ1_SIZE)

// Carveout region 0x18
#define S8000_KERNEL_REGION_BASE S8000_DRAM_BASE
#define S8000_KERNEL_REGION_SIZE \
    ((S8000_TZ1_BASE + ~S8000_KERNEL_REGION_BASE + 0x4000) & -0x4000)

static void s8000_start_cpus(MachineState *machine, uint64_t cpu_mask)
{
    S8000MachineState *s8000_machine = S8000_MACHINE(machine);
    int i;

    for (i = 0; i < machine->smp.cpus; i++) {
        if (test_bit(i, (unsigned long *)&cpu_mask) &&
            apple_a9_cpu_is_powered_off(s8000_machine->cpus[i])) {
            apple_a9_cpu_start(s8000_machine->cpus[i]);
        }
    }
}

static void s8000_create_s3c_uart(const S8000MachineState *s8000_machine,
                                  Chardev *chr)
{
    DeviceState *dev;
    hwaddr base;
    int vector;
    DTBProp *prop;
    hwaddr *uart_offset;
    DTBNode *child;

    child = dtb_get_node(s8000_machine->device_tree, "arm-io/uart0");
    g_assert_nonnull(child);

    g_assert_nonnull(dtb_find_prop(child, "boot-console"));

    prop = dtb_find_prop(child, "reg");
    g_assert_nonnull(prop);

    uart_offset = (hwaddr *)prop->data;
    base = s8000_machine->soc_base_pa + uart_offset[0];

    prop = dtb_find_prop(child, "interrupts");
    g_assert_nonnull(prop);

    vector = *(uint32_t *)prop->data;
    dev = exynos4210_uart_create(
        base, 256, 0, chr,
        qdev_get_gpio_in(DEVICE(s8000_machine->aic), vector));
    g_assert_nonnull(dev);
}

static void s8000_patch_kernel(MachoHeader64 *hdr)
{
    xnu_kpf(hdr);
}

static bool s8000_check_panic(S8000MachineState *s8000_machine)
{
    AppleEmbeddedPanicHeader *panic_info;
    bool ret;

    if (s8000_machine->panic_size == 0) {
        return false;
    }

    panic_info = g_malloc0(s8000_machine->panic_size);

    address_space_rw(&address_space_memory, s8000_machine->panic_base,
                     MEMTXATTRS_UNSPECIFIED, panic_info,
                     s8000_machine->panic_size, false);
    address_space_set(&address_space_memory, s8000_machine->panic_base, 0,
                      s8000_machine->panic_size, MEMTXATTRS_UNSPECIFIED);

    ret = panic_info->magic == EMBEDDED_PANIC_MAGIC;
    g_free(panic_info);
    return ret;
}

static size_t get_kaslr_random(void)
{
    size_t value = 0;
    qemu_guest_getrandom(&value, sizeof(value), NULL);
    return value;
}

#define L2_GRANULE ((0x4000) * (0x4000 / 8))
#define L2_GRANULE_MASK (L2_GRANULE - 1)

static void get_kaslr_slides(S8000MachineState *s8000_machine,
                             hwaddr *phys_slide_out, hwaddr *virt_slide_out)
{
    hwaddr slide_phys = 0, slide_virt = 0;
    const size_t slide_granular = (1 << 21);
    const size_t slide_granular_mask = slide_granular - 1;
    const size_t slide_virt_max = 0x100 * (2 * 1024 * 1024);
    size_t random_value = get_kaslr_random();

    if (s8000_machine->kaslr_off) {
        *phys_slide_out = 0;
        *virt_slide_out = 0;
        return;
    }

    slide_virt = (random_value & ~slide_granular_mask) % slide_virt_max;
    if (slide_virt == 0) {
        slide_virt = slide_virt_max;
    }
    slide_phys = slide_virt & L2_GRANULE_MASK;

    *phys_slide_out = slide_phys;
    *virt_slide_out = slide_virt;
}

static void s8000_load_classic_kc(S8000MachineState *s8000_machine,
                                  const char *cmdline)
{
    MachineState *machine = MACHINE(s8000_machine);
    MachoHeader64 *hdr = s8000_machine->kernel;
    MemoryRegion *sysmem = s8000_machine->sysmem;
    AddressSpace *nsas = &address_space_memory;
    hwaddr virt_low;
    hwaddr virt_end;
    hwaddr dtb_va;
    hwaddr top_of_kernel_data_pa;
    hwaddr phys_ptr;
    AppleBootInfo *info = &s8000_machine->bootinfo;
    hwaddr text_base;
    hwaddr prelink_text_base;
    DTBNode *memory_map =
        dtb_get_node(s8000_machine->device_tree, "/chosen/memory-map");
    hwaddr tz1_virt_low;
    hwaddr tz1_virt_high;

    g_phys_base = (hwaddr)macho_get_buffer(hdr);
    macho_highest_lowest(hdr, &virt_low, &virt_end);
    macho_text_base(hdr, &text_base);
    info->kern_text_off = text_base - virt_low;
    prelink_text_base = macho_get_segment(hdr, "__PRELINK_TEXT")->vmaddr;

    get_kaslr_slides(s8000_machine, &g_phys_slide, &g_virt_slide);

    g_phys_base = phys_ptr = S8000_KERNEL_REGION_BASE;
    phys_ptr += g_phys_slide;
    g_virt_base += g_virt_slide - g_phys_slide;

    // TrustCache
    info->trustcache_addr =
        vtop_static(prelink_text_base + g_virt_slide) - info->trustcache_size;
    info_report("__PRELINK_TEXT = " HWADDR_FMT_plx " + " HWADDR_FMT_plx,
                prelink_text_base, g_virt_slide);
    info_report("info->trustcache_pa = " HWADDR_FMT_plx, info->trustcache_addr);

    macho_load_trustcache(s8000_machine->trustcache, info->trustcache_size,
                          nsas, sysmem, info->trustcache_addr);

    info->kern_entry =
        arm_load_macho(hdr, nsas, sysmem, memory_map, phys_ptr, g_virt_slide);
    info_report("Kernel virtual base: 0x" TARGET_FMT_lx, g_virt_base);
    info_report("Kernel physical base: 0x" TARGET_FMT_lx, g_phys_base);
    info_report("Kernel text off: 0x" TARGET_FMT_lx, info->kern_text_off);
    info_report("Kernel virtual slide: 0x" TARGET_FMT_lx, g_virt_slide);
    info_report("Kernel physical slide: 0x" TARGET_FMT_lx, g_phys_slide);
    info_report("Kernel entry point: 0x" TARGET_FMT_lx, info->kern_entry);

    virt_end += g_virt_slide;
    phys_ptr = vtop_static(ROUND_UP_16K(virt_end));

    // Device tree
    info->device_tree_addr = phys_ptr;
    dtb_va = ptov_static(info->device_tree_addr);
    phys_ptr += info->device_tree_size;

    // RAM disk
    if (machine->initrd_filename) {
        info->ramdisk_addr = phys_ptr;
        macho_load_ramdisk(machine->initrd_filename, nsas, sysmem,
                           info->ramdisk_addr, &info->ramdisk_size);
        info->ramdisk_size = ROUND_UP_16K(info->ramdisk_size);
        phys_ptr += info->ramdisk_size;
    }

    info->sep_fw_addr = phys_ptr;
    if (s8000_machine->sep_fw_filename) {
        macho_load_raw_file(s8000_machine->sep_fw_filename, nsas, sysmem,
                            "sepfw", info->sep_fw_addr, &info->sep_fw_size);
    }
    info->sep_fw_size = ROUND_UP_16K(8 * MiB);
    phys_ptr += info->sep_fw_size;

    // Kernel boot args
    info->kern_boot_args_addr = phys_ptr;
    info->kern_boot_args_size = 0x4000;
    phys_ptr += info->kern_boot_args_size;

    macho_load_dtb(s8000_machine->device_tree, nsas, sysmem, "DeviceTree",
                   info);

    top_of_kernel_data_pa = (ROUND_UP_16K(phys_ptr) + 0x3000ull) & ~0x3FFFull;

    info_report("Boot args: [%s]", cmdline);
    macho_setup_bootargs("BootArgs", nsas, sysmem, info->kern_boot_args_addr,
                         g_virt_base, g_phys_base, S8000_KERNEL_REGION_SIZE,
                         top_of_kernel_data_pa, dtb_va, info->device_tree_size,
                         s8000_machine->video, cmdline);
    g_virt_base = virt_low;

    macho_highest_lowest(s8000_machine->secure_monitor, &tz1_virt_low,
                         &tz1_virt_high);
    info_report("TrustZone 1 virtual address low: " TARGET_FMT_lx,
                tz1_virt_low);
    info_report("TrustZone 1 virtual address high: " TARGET_FMT_lx,
                tz1_virt_high);
    AddressSpace *sas =
        cpu_get_address_space(CPU(s8000_machine->cpus[0]), ARMASIdx_S);
    g_assert_nonnull(sas);
    hwaddr tz1_entry =
        arm_load_macho(s8000_machine->secure_monitor, sas,
                       s8000_machine->sysmem, NULL, S8000_TZ1_BASE, 0);
    info_report("TrustZone 1 entry: " TARGET_FMT_lx, tz1_entry);
    hwaddr tz1_boot_args_pa =
        S8000_TZ1_BASE + (S8000_TZ1_SIZE - sizeof(AppleMonitorBootArgs));
    info_report("TrustZone 1 boot args address: " TARGET_FMT_lx,
                tz1_boot_args_pa);
    apple_monitor_setup_boot_args(
        "TZ1_BOOTARGS", sas, s8000_machine->sysmem, tz1_boot_args_pa,
        tz1_virt_low, S8000_TZ1_BASE, S8000_TZ1_SIZE,
        s8000_machine->bootinfo.kern_boot_args_addr,
        s8000_machine->bootinfo.kern_entry, g_phys_base, g_phys_slide,
        g_virt_slide, info->kern_text_off);
    s8000_machine->bootinfo.tz1_entry = tz1_entry;
    s8000_machine->bootinfo.tz1_boot_args_pa = tz1_boot_args_pa;
}

static void s8000_memory_setup(MachineState *machine)
{
    S8000MachineState *s8000_machine = S8000_MACHINE(machine);
    AppleBootInfo *info = &s8000_machine->bootinfo;
    AppleNvramState *nvram;
    char *cmdline;
    MachoHeader64 *hdr;
    DTBNode *memory_map;

    memory_map = dtb_get_node(s8000_machine->device_tree, "/chosen/memory-map");

    if (s8000_check_panic(s8000_machine)) {
        qemu_system_guest_panicked(NULL);
        return;
    }

    info->dram_base = S8000_DRAM_BASE;
    info->dram_size = S8000_DRAM_SIZE;

    nvram = APPLE_NVRAM(qdev_find_recursive(sysbus_get_default(), "nvram"));
    if (!nvram) {
        error_setg(&error_abort, "Failed to find NVRAM device");
        return;
    };
    apple_nvram_load(nvram);

    info_report("Boot mode: %u", s8000_machine->boot_mode);
    switch (s8000_machine->boot_mode) {
    case kBootModeEnterRecovery:
        env_set(nvram, "auto-boot", "false", 0);
        s8000_machine->boot_mode = kBootModeAuto;
        break;
    case kBootModeExitRecovery:
        env_set(nvram, "auto-boot", "true", 0);
        s8000_machine->boot_mode = kBootModeAuto;
        break;
    default:
        break;
    }

    info_report("auto-boot=%s",
                env_get_bool(nvram, "auto-boot", false) ? "true" : "false");

    if (s8000_machine->boot_mode == kBootModeAuto &&
        !env_get_bool(nvram, "auto-boot", false)) {
        cmdline = g_strconcat("-restore rd=md0 nand-enable-reformat=1 ",
                              machine->kernel_cmdline, NULL);
    } else {
        cmdline = g_strdup(machine->kernel_cmdline);
    }

    apple_nvram_save(nvram);

    info->nvram_size = nvram->len;

    if (info->nvram_size > XNU_MAX_NVRAM_SIZE) {
        info->nvram_size = XNU_MAX_NVRAM_SIZE;
    }
    if (apple_nvram_serialize(nvram, info->nvram_data,
                              sizeof(info->nvram_data)) < 0) {
        error_report("Failed to read NVRAM");
    }

    if (s8000_machine->ticket_filename) {
        if (!g_file_get_contents(s8000_machine->ticket_filename,
                                 &info->ticket_data,
                                 (gsize *)&info->ticket_length, NULL)) {
            error_report("Failed to read ticket from file %s",
                         s8000_machine->ticket_filename);
        }
    }

    DTBNode *chosen = dtb_get_node(s8000_machine->device_tree, "chosen");
    if (xnu_contains_boot_arg(cmdline, "-restore", false)) {
        // HACK: Use DEV Hardware model to restore without FDR errors
        dtb_set_prop(s8000_machine->device_tree, "compatible", 26,
                     "N66DEV\0iPhone8,2\0AppleARM");
    } else {
        dtb_set_prop(s8000_machine->device_tree, "compatible", 25,
                     "N66AP\0iPhone8,2\0AppleARM");
    }

    if (!xnu_contains_boot_arg(cmdline, "rd=", true)) {
        DTBProp *prop = dtb_find_prop(chosen, "root-matching");

        if (prop) {
            snprintf((char *)prop->data, prop->length,
                     "<dict><key>IOProviderClass</key><string>IOMedia</"
                     "string><key>IOPropertyMatch</key><dict><key>Partition "
                     "ID</key><integer>1</integer></dict></dict>");
        }
    }

    DTBNode *pram = dtb_get_node(s8000_machine->device_tree, "pram");
    if (pram) {
        uint64_t panic_reg[2] = { 0 };
        uint64_t panic_base = S8000_PANIC_BASE;
        uint64_t panic_size = S8000_PANIC_SIZE;

        panic_reg[0] = panic_base;
        panic_reg[1] = panic_size;

        dtb_set_prop(pram, "reg", sizeof(panic_reg), &panic_reg);
        dtb_set_prop_u64(chosen, "embedded-panic-log-size", panic_size);
        s8000_machine->panic_base = panic_base;
        s8000_machine->panic_size = panic_size;
    }

    DTBNode *vram = dtb_get_node(s8000_machine->device_tree, "vram");
    if (vram) {
        uint64_t vram_reg[2] = { 0 };
        uint64_t vram_base = S8000_DISPLAY_BASE;
        uint64_t vram_size = S8000_DISPLAY_SIZE;
        vram_reg[0] = vram_base;
        vram_reg[1] = vram_size;
        dtb_set_prop(vram, "reg", sizeof(vram_reg), &vram_reg);
    }

    hdr = s8000_machine->kernel;
    g_assert_nonnull(hdr);

    macho_allocate_segment_records(memory_map, hdr);

    macho_populate_dtb(s8000_machine->device_tree, info);

    switch (hdr->file_type) {
    case MH_EXECUTE:
        s8000_load_classic_kc(s8000_machine, cmdline);
        break;
    default:
        error_setg(&error_abort, "Unsupported kernelcache type: 0x%x\n",
                   hdr->file_type);
        break;
    }

    g_free(cmdline);
}

static void pmgr_unk_reg_write(void *opaque, hwaddr addr, uint64_t data,
                               unsigned size)
{
#if 0
    hwaddr base = (hwaddr)opaque;
    qemu_log_mask(LOG_UNIMP,
                  "PMGR reg WRITE unk @ 0x" TARGET_FMT_lx
                  " base: 0x" TARGET_FMT_lx " value: 0x" TARGET_FMT_lx "\n",
                  base + addr, base, data);
#endif
}

static uint64_t pmgr_unk_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    hwaddr base = (hwaddr)opaque;

    switch (base + addr) {
    case 0x102BC000: // CFG_FUSE0
        return (1 << 2);
    case 0x102BC200: // CFG_FUSE0_RAW
        return 0x0;
    case 0x102BC080: // ECID_LO
        return 0x13371337;
    case 0x102BC084: // ECID_HI
        return 0xDEADBEEF;
    case 0x102E8000: // ????
        return 0x4;
    case 0x102BC104: // ???? bit 24 => is fresh boot?
        return (1 << 24) | (1 << 25);
    default:
#if 0
        qemu_log_mask(LOG_UNIMP,
                      "PMGR reg READ unk @ 0x" TARGET_FMT_lx
                      " base: 0x" TARGET_FMT_lx "\n",
                      base + addr, base);
#endif
        break;
    }
    return 0;
}

static const MemoryRegionOps pmgr_unk_reg_ops = {
    .write = pmgr_unk_reg_write,
    .read = pmgr_unk_reg_read,
};

static void pmgr_reg_write(void *opaque, hwaddr addr, uint64_t data,
                           unsigned size)
{
    MachineState *machine = MACHINE(opaque);
    S8000MachineState *s8000_machine = S8000_MACHINE(opaque);
    uint32_t value = data;

#if 0
    qemu_log_mask(LOG_UNIMP,
                  "PMGR reg WRITE @ 0x" TARGET_FMT_lx " value: 0x" TARGET_FMT_lx
                  "\n",
                  addr, data);
#endif

    if (addr >= 0x80000 && addr <= 0x88010) {
        value = (value & 0xf) << 4 | (value & 0xf);
    }

    switch (addr) {
    case 0x80400: // SEP Power State, Manual & Actual: Run Max
        value = 0xFF;
        break;
    case 0xD4004:
        s8000_start_cpus(machine, data);
    default:
        break;
    }
    memcpy(s8000_machine->pmgr_reg + addr, &value, size);
}

static uint64_t pmgr_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    S8000MachineState *s8000_machine = S8000_MACHINE(opaque);
    uint64_t result = 0;
#if 0
    qemu_log_mask(LOG_UNIMP, "PMGR reg READ @ 0x" TARGET_FMT_lx "\n", addr);
#endif

    memcpy(&result, s8000_machine->pmgr_reg + addr, size);
    return result;
}

static const MemoryRegionOps pmgr_reg_ops = {
    .write = pmgr_reg_write,
    .read = pmgr_reg_read,
};

static void s8000_cpu_setup(S8000MachineState *s8000_machine)
{
    unsigned int i;
    DTBNode *root;
    MachineState *machine = MACHINE(s8000_machine);
    GList *iter;
    GList *next = NULL;

    root = dtb_get_node(s8000_machine->device_tree, "cpus");
    g_assert_nonnull(root);
    object_initialize_child(OBJECT(s8000_machine), "cluster",
                            &s8000_machine->cluster, TYPE_CPU_CLUSTER);
    qdev_prop_set_uint32(DEVICE(&s8000_machine->cluster), "cluster-id", 0);

    for (iter = root->children, i = 0; iter; iter = next, i++) {
        DTBNode *node;

        next = iter->next;
        node = (DTBNode *)iter->data;
        if (i >= machine->smp.cpus) {
            dtb_remove_node(root, node);
            continue;
        }

        s8000_machine->cpus[i] = apple_a9_create(node, NULL, 0, 0);

        object_property_add_child(OBJECT(&s8000_machine->cluster),
                                  DEVICE(s8000_machine->cpus[i])->id,
                                  OBJECT(s8000_machine->cpus[i]));

        qdev_realize(DEVICE(s8000_machine->cpus[i]), NULL, &error_fatal);
    }
    qdev_realize(DEVICE(&s8000_machine->cluster), NULL, &error_fatal);
}

static void s8000_create_aic(S8000MachineState *s8000_machine)
{
    unsigned int i;
    hwaddr *reg;
    DTBProp *prop;
    MachineState *machine = MACHINE(s8000_machine);
    DTBNode *soc = dtb_get_node(s8000_machine->device_tree, "arm-io");
    DTBNode *child;
    DTBNode *timebase;

    g_assert_nonnull(soc);
    child = dtb_get_node(soc, "aic");
    g_assert_nonnull(child);
    timebase = dtb_get_node(soc, "aic-timebase");
    g_assert_nonnull(timebase);

    s8000_machine->aic = apple_aic_create(machine->smp.cpus, child, timebase);
    object_property_add_child(OBJECT(s8000_machine), "aic",
                              OBJECT(s8000_machine->aic));
    g_assert_nonnull(s8000_machine->aic);
    sysbus_realize(s8000_machine->aic, &error_fatal);

    prop = dtb_find_prop(child, "reg");
    g_assert_nonnull(prop);

    reg = (hwaddr *)prop->data;

    for (i = 0; i < machine->smp.cpus; i++) {
        memory_region_add_subregion_overlap(
            &s8000_machine->cpus[i]->memory,
            s8000_machine->soc_base_pa + reg[0],
            sysbus_mmio_get_region(s8000_machine->aic, i), 0);
        sysbus_connect_irq(
            s8000_machine->aic, i,
            qdev_get_gpio_in(DEVICE(s8000_machine->cpus[i]), ARM_CPU_IRQ));
    }
}

static void s8000_pmgr_setup(S8000MachineState *s8000_machine)
{
    uint64_t *reg;
    int i;
    char name[32];
    DTBProp *prop;
    DTBNode *child;

    child = dtb_get_node(s8000_machine->device_tree, "arm-io/pmgr");
    g_assert_nonnull(child);

    prop = dtb_find_prop(child, "reg");
    g_assert_nonnull(prop);

    reg = (uint64_t *)prop->data;

    for (i = 0; i < prop->length / 8; i += 2) {
        MemoryRegion *mem = g_new(MemoryRegion, 1);
        if (i == 0) {
            memory_region_init_io(mem, OBJECT(s8000_machine), &pmgr_reg_ops,
                                  s8000_machine, "pmgr-reg", reg[i + 1]);
        } else {
            snprintf(name, sizeof(name), "pmgr-unk-reg-%d", i);
            memory_region_init_io(mem, OBJECT(s8000_machine), &pmgr_unk_reg_ops,
                                  (void *)reg[i], name, reg[i + 1]);
        }
        memory_region_add_subregion_overlap(
            s8000_machine->sysmem,
            reg[i] + reg[i + 1] < s8000_machine->soc_size ?
                s8000_machine->soc_base_pa + reg[i] :
                reg[i],
            mem, -1);
    }

    dtb_set_prop(child, "voltage-states1", sizeof(s8000_voltage_states1),
                 s8000_voltage_states1);
}

static void s8000_create_nvme(S8000MachineState *s8000_machine)
{
    uint32_t *ints;
    DTBProp *prop;
    uint64_t *reg;
    SysBusDevice *nvme;
    DTBNode *child;

    child = dtb_get_node(s8000_machine->device_tree, "arm-io/nvme-mmu0");
    g_assert_nonnull(child);

    nvme = apple_nvme_mmu_create(child);
    g_assert_nonnull(nvme);

    prop = dtb_find_prop(child, "reg");
    g_assert_nonnull(prop);
    reg = (uint64_t *)prop->data;

    sysbus_mmio_map(nvme, 0, s8000_machine->soc_base_pa + reg[0]);

    object_property_add_child(OBJECT(s8000_machine), "nvme", OBJECT(nvme));

    prop = dtb_find_prop(child, "interrupts");
    g_assert_nonnull(prop);
    g_assert_cmpuint(prop->length, ==, 4);
    ints = (uint32_t *)prop->data;

    sysbus_connect_irq(nvme, 0,
                       qdev_get_gpio_in(DEVICE(s8000_machine->aic), ints[0]));

    sysbus_realize_and_unref(nvme, &error_fatal);
}

static void s8000_create_gpio(S8000MachineState *s8000_machine,
                              const char *name)
{
    DeviceState *gpio = NULL;
    DTBProp *prop;
    uint64_t *reg;
    uint32_t *ints;
    int i;
    DTBNode *child = dtb_get_node(s8000_machine->device_tree, "arm-io");

    child = dtb_get_node(child, name);
    g_assert_nonnull(child);
    gpio = apple_gpio_create(child);
    g_assert_nonnull(gpio);
    object_property_add_child(OBJECT(s8000_machine), name, OBJECT(gpio));

    prop = dtb_find_prop(child, "reg");
    g_assert_nonnull(prop);
    reg = (uint64_t *)prop->data;
    sysbus_mmio_map(SYS_BUS_DEVICE(gpio), 0,
                    s8000_machine->soc_base_pa + reg[0]);
    prop = dtb_find_prop(child, "interrupts");
    g_assert_nonnull(prop);

    ints = (uint32_t *)prop->data;

    for (i = 0; i < prop->length / sizeof(uint32_t); i++) {
        sysbus_connect_irq(
            SYS_BUS_DEVICE(gpio), i,
            qdev_get_gpio_in(DEVICE(s8000_machine->aic), ints[i]));
    }

    sysbus_realize_and_unref(SYS_BUS_DEVICE(gpio), &error_fatal);
}

static void s8000_create_i2c(S8000MachineState *s8000_machine, const char *name)
{
    SysBusDevice *i2c;
    DTBProp *prop;
    uint64_t *reg;
    uint32_t *ints;
    int i;
    DTBNode *child = dtb_get_node(s8000_machine->device_tree, "arm-io");

    child = dtb_get_node(child, name);
    g_assert_nonnull(child);
    i2c = apple_i2c_create(name);
    g_assert_nonnull(i2c);
    object_property_add_child(OBJECT(s8000_machine), name, OBJECT(i2c));

    prop = dtb_find_prop(child, "reg");
    g_assert_nonnull(prop);
    reg = (uint64_t *)prop->data;
    sysbus_mmio_map(i2c, 0, s8000_machine->soc_base_pa + reg[0]);
    prop = dtb_find_prop(child, "interrupts");
    g_assert_nonnull(prop);

    ints = (uint32_t *)prop->data;

    for (i = 0; i < prop->length / sizeof(uint32_t); i++) {
        sysbus_connect_irq(
            i2c, i, qdev_get_gpio_in(DEVICE(s8000_machine->aic), ints[i]));
    }

    sysbus_realize_and_unref(i2c, &error_fatal);
}

static void s8000_create_spi0(S8000MachineState *s8000_machine)
{
    DeviceState *spi = NULL;
    DeviceState *gpio = NULL;
    const char *name = "spi0";

    spi = qdev_new(TYPE_APPLE_SPI);
    g_assert_nonnull(spi);
    DEVICE(spi)->id = g_strdup(name);

    object_property_add_child(OBJECT(s8000_machine), name, OBJECT(spi));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(spi), &error_fatal);

    sysbus_mmio_map(SYS_BUS_DEVICE(spi), 0,
                    s8000_machine->soc_base_pa + S8000_SPI0_BASE);

    sysbus_connect_irq(
        SYS_BUS_DEVICE(spi), 0,
        qdev_get_gpio_in(DEVICE(s8000_machine->aic), S8000_SPI0_IRQ));
    // The second sysbus IRQ is the cs line
    gpio = DEVICE(
        object_property_get_link(OBJECT(s8000_machine), "gpio", &error_fatal));
    qdev_connect_gpio_out(gpio, S8000_GPIO_SPI0_CS,
                          qdev_get_gpio_in_named(spi, SSI_GPIO_CS, 0));
}

static void s8000_create_spi(S8000MachineState *s8000_machine, const char *name)
{
    SysBusDevice *spi = NULL;
    DTBProp *prop;
    uint64_t *reg;
    uint32_t *ints;
    DTBNode *child = dtb_get_node(s8000_machine->device_tree, "arm-io");

    child = dtb_get_node(child, name);
    g_assert_nonnull(child);
    spi = apple_spi_create(child);
    g_assert_nonnull(spi);
    object_property_add_child(OBJECT(s8000_machine), name, OBJECT(spi));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(spi), &error_fatal);

    prop = dtb_find_prop(child, "reg");
    g_assert_nonnull(prop);
    reg = (uint64_t *)prop->data;
    sysbus_mmio_map(SYS_BUS_DEVICE(spi), 0,
                    s8000_machine->soc_base_pa + reg[0]);
    prop = dtb_find_prop(child, "interrupts");
    g_assert_nonnull(prop);

    // The second sysbus IRQ is the cs line
    // TODO: Connect this to gpio over spi_cs0?
    ints = (uint32_t *)prop->data;
    sysbus_connect_irq(SYS_BUS_DEVICE(spi), 0,
                       qdev_get_gpio_in(DEVICE(s8000_machine->aic), ints[0]));
}

static void s8000_create_usb(S8000MachineState *s8000_machine)
{
    DTBNode *child = dtb_get_node(s8000_machine->device_tree, "arm-io");
    DTBNode *phy, *complex, *device;
    DTBProp *prop;
    DeviceState *otg;

    phy = dtb_get_node(child, "otgphyctrl");
    g_assert_nonnull(phy);

    complex = dtb_get_node(child, "usb-complex");
    g_assert_nonnull(complex);

    device = dtb_get_node(complex, "usb-device");
    g_assert_nonnull(device);

    otg = apple_otg_create(complex);
    object_property_add_child(OBJECT(s8000_machine), "otg", OBJECT(otg));
    prop = dtb_find_prop(phy, "reg");
    g_assert_nonnull(prop);
    sysbus_mmio_map(SYS_BUS_DEVICE(otg), 0,
                    s8000_machine->soc_base_pa + ((uint64_t *)prop->data)[0]);
    sysbus_mmio_map(SYS_BUS_DEVICE(otg), 1,
                    s8000_machine->soc_base_pa + ((uint64_t *)prop->data)[2]);
    sysbus_mmio_map(
        SYS_BUS_DEVICE(otg), 2,
        s8000_machine->soc_base_pa +
            ((uint64_t *)dtb_find_prop(complex, "ranges")->data)[1] +
            ((uint64_t *)dtb_find_prop(device, "reg")->data)[0]);

    prop = dtb_find_prop(complex, "reg");
    if (prop) {
        sysbus_mmio_map(SYS_BUS_DEVICE(otg), 3,
                        s8000_machine->soc_base_pa +
                            ((uint64_t *)prop->data)[0]);
    }

    sysbus_realize_and_unref(SYS_BUS_DEVICE(otg), &error_fatal);

    prop = dtb_find_prop(device, "interrupts");
    g_assert_nonnull(prop);
    sysbus_connect_irq(SYS_BUS_DEVICE(otg), 0,
                       qdev_get_gpio_in(DEVICE(s8000_machine->aic),
                                        ((uint32_t *)prop->data)[0]));
}

static void s8000_create_wdt(S8000MachineState *s8000_machine)
{
    int i;
    uint32_t *ints;
    DTBProp *prop;
    uint64_t *reg;
    SysBusDevice *wdt;
    DTBNode *child = dtb_get_node(s8000_machine->device_tree, "arm-io");

    g_assert_nonnull(child);
    child = dtb_get_node(child, "wdt");
    g_assert_nonnull(child);

    wdt = apple_wdt_create(child);
    g_assert_nonnull(wdt);

    object_property_add_child(OBJECT(s8000_machine), "wdt", OBJECT(wdt));
    prop = dtb_find_prop(child, "reg");
    g_assert_nonnull(prop);
    reg = (uint64_t *)prop->data;

    sysbus_mmio_map(wdt, 0, s8000_machine->soc_base_pa + reg[0]);
    sysbus_mmio_map(wdt, 1, s8000_machine->soc_base_pa + reg[2]);

    prop = dtb_find_prop(child, "interrupts");
    g_assert_nonnull(prop);
    ints = (uint32_t *)prop->data;

    for (i = 0; i < prop->length / sizeof(uint32_t); i++) {
        sysbus_connect_irq(
            wdt, i, qdev_get_gpio_in(DEVICE(s8000_machine->aic), ints[i]));
    }

    // TODO: MCC
    dtb_remove_prop_named(child, "function-panic_flush_helper");
    dtb_remove_prop_named(child, "function-panic_halt_helper");

    dtb_set_prop_u32(child, "no-pmu", 1);

    sysbus_realize_and_unref(wdt, &error_fatal);
}

static void s8000_create_aes(S8000MachineState *s8000_machine)
{
    DTBNode *child;
    SysBusDevice *aes;
    DTBProp *prop;
    uint64_t *reg;
    uint32_t *ints;

    child = dtb_get_node(s8000_machine->device_tree, "arm-io");
    g_assert_nonnull(child);
    child = dtb_get_node(child, "aes");
    g_assert_nonnull(child);

    aes = apple_aes_create(child, s8000_machine->board_id);
    g_assert_nonnull(aes);

    object_property_add_child(OBJECT(s8000_machine), "aes", OBJECT(aes));
    prop = dtb_find_prop(child, "reg");
    g_assert_nonnull(prop);
    reg = (uint64_t *)prop->data;

    sysbus_mmio_map(aes, 0, s8000_machine->soc_base_pa + reg[0]);
    sysbus_mmio_map(aes, 1, s8000_machine->soc_base_pa + reg[2]);

    prop = dtb_find_prop(child, "interrupts");
    g_assert_nonnull(prop);
    g_assert_cmpuint(prop->length, ==, 4);
    ints = (uint32_t *)prop->data;

    sysbus_connect_irq(aes, 0,
                       qdev_get_gpio_in(DEVICE(s8000_machine->aic), *ints));

    g_assert_nonnull(object_property_add_const_link(
        OBJECT(aes), "dma-mr", OBJECT(s8000_machine->sysmem)));

    sysbus_realize_and_unref(aes, &error_fatal);
}

static void s8000_create_sep(S8000MachineState *s8000_machine)
{
    DTBNode *child;
    DTBProp *prop;
    uint64_t *reg;
    uint32_t *ints;
    int i;

    child = dtb_get_node(s8000_machine->device_tree, "arm-io");
    g_assert_nonnull(child);
    child = dtb_get_node(child, "sep");
    g_assert_nonnull(child);

    s8000_machine->sep = SYS_BUS_DEVICE(apple_sep_sim_create(child, false));
    g_assert_nonnull(s8000_machine->sep);

    object_property_add_child(OBJECT(s8000_machine), "sep",
                              OBJECT(s8000_machine->sep));
    prop = dtb_find_prop(child, "reg");
    g_assert_nonnull(prop);
    reg = (uint64_t *)prop->data;

    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s8000_machine->sep), 0,
                            s8000_machine->soc_base_pa + reg[0], 2);

    prop = dtb_find_prop(child, "interrupts");
    g_assert_nonnull(prop);
    ints = (uint32_t *)prop->data;

    for (i = 0; i < prop->length / sizeof(uint32_t); i++) {
        sysbus_connect_irq(
            SYS_BUS_DEVICE(s8000_machine->sep), i,
            qdev_get_gpio_in(DEVICE(s8000_machine->aic), ints[i]));
    }

    g_assert_nonnull(object_property_add_const_link(
        OBJECT(s8000_machine->sep), "dma-mr", OBJECT(s8000_machine->sysmem)));

    sysbus_realize_and_unref(SYS_BUS_DEVICE(s8000_machine->sep), &error_fatal);
}

static void s8000_create_pmu(S8000MachineState *s8000_machine)
{
    AppleI2CState *i2c;
    DTBNode *child;
    DTBProp *prop;
    DeviceState *dev;
    DeviceState *gpio;
    uint32_t *ints;

    i2c = APPLE_I2C(
        object_property_get_link(OBJECT(s8000_machine), "i2c0", &error_fatal));

    child = dtb_get_node(s8000_machine->device_tree, "arm-io/i2c0/pmu");
    g_assert_nonnull(child);

    prop = dtb_find_prop(child, "reg");
    g_assert_nonnull(prop);

    dev = DEVICE(i2c_slave_create_simple(i2c->bus, TYPE_PMU_D2255,
                                         *(uint32_t *)prop->data));

    prop = dtb_find_prop(child, "interrupts");
    g_assert_nonnull(prop);
    ints = (uint32_t *)prop->data;

    gpio = DEVICE(
        object_property_get_link(OBJECT(s8000_machine), "gpio", &error_fatal));
    qdev_connect_gpio_out(dev, 0, qdev_get_gpio_in(gpio, ints[0]));
}

static void s8000_display_create(S8000MachineState *s8000_machine)
{
    MachineState *machine;
    ADBEV2 *s;
    SysBusDevice *sbd;
    DTBNode *child;
    uint64_t *reg;
    DTBProp *prop;

    machine = MACHINE(s8000_machine);

    child = dtb_get_node(s8000_machine->device_tree, "arm-io/disp0");
    g_assert_nonnull(child);

    dtb_remove_prop_named(child, "iommu-parent");

    s = adbe_v2_create(child);
    sbd = SYS_BUS_DEVICE(s);
    s8000_machine->video.base_addr = S8000_DISPLAY_BASE;
    s8000_machine->video.row_bytes = s->width * 4;
    s8000_machine->video.width = s->width;
    s8000_machine->video.height = s->height;
    s8000_machine->video.depth = 32 | ((2 - 1) << 16);
    s8000_machine->video.display = 1;
    if (xnu_contains_boot_arg(machine->kernel_cmdline, "-s", false) ||
        xnu_contains_boot_arg(machine->kernel_cmdline, "-v", false)) {
        s8000_machine->video.display = 0;
    }

    prop = dtb_find_prop(child, "reg");
    g_assert_nonnull(prop);
    reg = (uint64_t *)prop->data;

    sysbus_mmio_map(sbd, 0, s8000_machine->soc_base_pa + reg[0]);
    sysbus_mmio_map(sbd, 1, s8000_machine->soc_base_pa + reg[2]);
    sysbus_mmio_map(sbd, 2, s8000_machine->soc_base_pa + reg[4]);
    sysbus_mmio_map(sbd, 3, s8000_machine->soc_base_pa + reg[6]);
    sysbus_mmio_map(sbd, 4, s8000_machine->soc_base_pa + reg[8]);
    sysbus_mmio_map(sbd, 5, s8000_machine->soc_base_pa + reg[10]);

    prop = dtb_find_prop(child, "interrupts");
    g_assert_nonnull(prop);
    uint32_t *ints = (uint32_t *)prop->data;

    for (size_t i = 0; i < prop->length / sizeof(uint32_t); i++) {
        sysbus_init_irq(sbd, &s->irqs[i]);
        sysbus_connect_irq(
            sbd, i, qdev_get_gpio_in(DEVICE(s8000_machine->aic), ints[i]));
    }

    // AppleDARTState *dart = APPLE_DART(
    //     object_property_get_link(OBJECT(machine), "dart-disp0",
    //     &error_fatal));
    // g_assert_nonnull(dart);
    // child = find_dtb_node(s8000_machine->device_tree,
    // "arm-io/dart-disp0/mapper-disp0"); g_assert_nonnull(child); prop =
    // find_dtb_prop(child, "reg"); g_assert_nonnull(prop); s->dma_mr =
    //     MEMORY_REGION(apple_dart_iommu_mr(dart, *(uint32_t *)prop->value));
    // g_assert_nonnull(s->dma_mr);
    // g_assert_nonnull(object_property_add_const_link(OBJECT(sbd), "dma_mr",
    //                                         OBJECT(s->dma_mr)));
    // address_space_init(&s->dma_as, s->dma_mr, "disp0.dma");

    memory_region_init_ram(&s->vram, OBJECT(sbd), "vram", S8000_DISPLAY_SIZE,
                           &error_fatal);
    memory_region_add_subregion_overlap(
        s8000_machine->sysmem, s8000_machine->video.base_addr, &s->vram, 1);
    object_property_add_const_link(OBJECT(sbd), "vram", OBJECT(&s->vram));
    object_property_add_child(OBJECT(machine), "disp0", OBJECT(sbd));

    sysbus_realize_and_unref(sbd, &error_fatal);
}

static void s8000_cpu_reset_work(CPUState *cpu, run_on_cpu_data data)
{
    S8000MachineState *s8000_machine;

    s8000_machine = data.host_ptr;
    cpu_reset(cpu);
    ARM_CPU(cpu)->env.xregs[0] = s8000_machine->bootinfo.tz1_boot_args_pa;
    cpu_set_pc(cpu, s8000_machine->bootinfo.tz1_entry);
}

static void apple_a9_reset(S8000MachineState *s8000_machine)
{
    CPUState *cpu;
    AppleA9State *tcpu;

    CPU_FOREACH (cpu) {
        tcpu = APPLE_A9(cpu);
        object_property_set_int(OBJECT(cpu), "rvbar", S8000_TZ1_BASE,
                                &error_abort);
        if (tcpu->cpu_id == 0) {
            async_run_on_cpu(cpu, s8000_cpu_reset_work,
                             RUN_ON_CPU_HOST_PTR(s8000_machine));
            continue;
        }
        if (ARM_CPU(cpu)->power_state != PSCI_OFF) {
            arm_reset_cpu(tcpu->mpidr);
        }
    }
}

static void s8000_machine_reset(MachineState *machine, ResetType type)
{
    S8000MachineState *s8000_machine = S8000_MACHINE(machine);
    DeviceState *gpio = NULL;

    qemu_devices_reset(type);

    if (!runstate_check(RUN_STATE_RESTORE_VM) &&
        !runstate_check(RUN_STATE_PRELAUNCH)) {
        s8000_memory_setup(MACHINE(s8000_machine));
    }

    apple_a9_reset(s8000_machine);

    gpio =
        DEVICE(object_property_get_link(OBJECT(machine), "gpio", &error_fatal));

    qemu_set_irq(qdev_get_gpio_in(gpio, S8000_GPIO_FORCE_DFU),
                 s8000_machine->force_dfu);
}

static void s8000_machine_init_done(Notifier *notifier, void *data)
{
    S8000MachineState *s8000_machine =
        container_of(notifier, S8000MachineState, init_done_notifier);
    s8000_memory_setup(MACHINE(s8000_machine));
}

static void s8000_machine_init(MachineState *machine)
{
    S8000MachineState *s8000_machine = S8000_MACHINE(machine);
    DTBNode *child;
    DTBProp *prop;
    hwaddr *ranges;
    MachoHeader64 *hdr, *secure_monitor = NULL;
    uint32_t build_version;
    uint64_t kernel_low, kernel_high;
    uint8_t buffer[0x40];

    s8000_machine->sysmem = get_system_memory();
    allocate_ram(s8000_machine->sysmem, "SRAM", S8000_SRAM_BASE,
                 S8000_SRAM_SIZE, 0);
    allocate_ram(s8000_machine->sysmem, "DRAM", S8000_DRAM_BASE,
                 S8000_DRAM_SIZE, 0);
    allocate_ram(s8000_machine->sysmem, "SEPROM", S8000_SEPROM_BASE,
                 S8000_SEPROM_SIZE, 0);
    MemoryRegion *mr = g_new0(MemoryRegion, 1);
    memory_region_init_alias(mr, OBJECT(s8000_machine), "s8000.seprom.alias",
                             s8000_machine->sysmem, S8000_SEPROM_BASE,
                             S8000_SEPROM_SIZE);
    memory_region_add_subregion_overlap(s8000_machine->sysmem, 0, mr, 1);

    hdr = macho_load_file(machine->kernel_filename, &secure_monitor);
    g_assert_nonnull(hdr);
    g_assert_nonnull(secure_monitor);
    s8000_machine->kernel = hdr;
    s8000_machine->secure_monitor = secure_monitor;
    build_version = macho_build_version(hdr);
    info_report("Loading %s %u.%u...", macho_platform_string(hdr),
                BUILD_VERSION_MAJOR(build_version),
                BUILD_VERSION_MINOR(build_version));
    s8000_machine->build_version = build_version;

    macho_highest_lowest(hdr, &kernel_low, &kernel_high);
    info_report("Kernel virtual low: 0x" TARGET_FMT_lx, kernel_low);
    info_report("Kernel virtual high: 0x" TARGET_FMT_lx, kernel_high);

    g_virt_base = kernel_low;
    g_phys_base = (hwaddr)macho_get_buffer(hdr);

    s8000_patch_kernel(hdr);

    s8000_machine->device_tree = load_dtb_from_file(machine->dtb);
    s8000_machine->trustcache =
        load_trustcache_from_file(s8000_machine->trustcache_filename,
                                  &s8000_machine->bootinfo.trustcache_size);
    dtb_set_prop_u32(s8000_machine->device_tree, "clock-frequency", 24000000);
    child = dtb_get_node(s8000_machine->device_tree, "arm-io");
    g_assert_nonnull(child);

    dtb_set_prop_u32(child, "chip-revision", 0);

    dtb_set_prop(child, "clock-frequencies", sizeof(s8000_clock_frequencies),
                 s8000_clock_frequencies);

    prop = dtb_find_prop(child, "ranges");
    g_assert_nonnull(prop);

    ranges = (hwaddr *)prop->data;
    s8000_machine->soc_base_pa = ranges[1];
    s8000_machine->soc_size = ranges[2];

    memset(buffer, 0, sizeof(buffer));
    memcpy(buffer, "s8000", 5);
    dtb_set_prop(s8000_machine->device_tree, "platform-name", 32, buffer);
    memset(buffer, 0, sizeof(buffer));
    memcpy(buffer, "MWL72", 5);
    dtb_set_prop(s8000_machine->device_tree, "model-number", 32, buffer);
    memset(buffer, 0, sizeof(buffer));
    memcpy(buffer, "LL/A", 4);
    dtb_set_prop(s8000_machine->device_tree, "region-info", 32, buffer);
    memset(buffer, 0, sizeof(buffer));
    dtb_set_prop(s8000_machine->device_tree, "config-number", 0x40, buffer);
    memset(buffer, 0, sizeof(buffer));
    memcpy(buffer, "C39ZRMDEN72J", 12);
    dtb_set_prop(s8000_machine->device_tree, "serial-number", 32, buffer);
    memset(buffer, 0, sizeof(buffer));
    memcpy(buffer, "C39948108J9N72J1F", 17);
    dtb_set_prop(s8000_machine->device_tree, "mlb-serial-number", 32, buffer);
    memset(buffer, 0, sizeof(buffer));
    memcpy(buffer, "A2111", 5);
    dtb_set_prop(s8000_machine->device_tree, "regulatory-model-number", 32,
                 buffer);

    child = dtb_get_node(s8000_machine->device_tree, "chosen");
    dtb_set_prop_u32(child, "chip-id", 0x8000);
    s8000_machine->board_id = 1; // Match with apple_aes.c
    dtb_set_prop_u32(child, "board-id", s8000_machine->board_id);

    if (s8000_machine->ecid == 0) {
        s8000_machine->ecid = 0x1122334455667788;
    }
    dtb_set_prop_u64(child, "unique-chip-id", s8000_machine->ecid);

    // Update the display parameters
    dtb_set_prop_u32(child, "display-rotation", 0);
    dtb_set_prop_u32(child, "display-scale", 2);

    child = dtb_get_node(s8000_machine->device_tree, "product");

    dtb_set_prop_u32(child, "oled-display", 1);
    dtb_set_prop(child, "graphics-featureset-class", 1, "");
    dtb_set_prop(child, "graphics-featureset-fallbacks", 1, "");
    dtb_set_prop_u32(child, "device-color-policy", 0);

    s8000_cpu_setup(s8000_machine);

    s8000_create_aic(s8000_machine);

    s8000_create_s3c_uart(s8000_machine, serial_hd(0));

    s8000_pmgr_setup(s8000_machine);

    s8000_create_nvme(s8000_machine);

    s8000_create_gpio(s8000_machine, "gpio");
    s8000_create_gpio(s8000_machine, "aop-gpio");

    s8000_create_i2c(s8000_machine, "i2c0");
    s8000_create_i2c(s8000_machine, "i2c1");
    s8000_create_i2c(s8000_machine, "i2c2");

    s8000_create_usb(s8000_machine);

    s8000_create_wdt(s8000_machine);

    s8000_create_aes(s8000_machine);

    s8000_create_spi0(s8000_machine);
    s8000_create_spi(s8000_machine, "spi1");
    s8000_create_spi(s8000_machine, "spi2");
    s8000_create_spi(s8000_machine, "spi3");

    s8000_create_sep(s8000_machine);

    s8000_create_pmu(s8000_machine);

    s8000_display_create(s8000_machine);

    s8000_machine->init_done_notifier.notify = s8000_machine_init_done;
    qemu_add_machine_init_done_notifier(&s8000_machine->init_done_notifier);
}

static void s8000_set_kaslr_off(Object *obj, bool value, Error **errp)
{
    S8000MachineState *s8000_machine;

    s8000_machine = S8000_MACHINE(obj);
    s8000_machine->kaslr_off = value;
}

static bool s8000_get_kaslr_off(Object *obj, Error **errp)
{
    S8000MachineState *s8000_machine;

    s8000_machine = S8000_MACHINE(obj);
    return s8000_machine->kaslr_off;
}

static ram_addr_t s8000_machine_fixup_ram_size(ram_addr_t size)
{
    g_assert_cmpuint(size, ==, S8000_DRAM_SIZE);
    return size;
}

static void s8000_set_trustcache_filename(Object *obj, const char *value,
                                          Error **errp)
{
    S8000MachineState *s8000_machine;

    s8000_machine = S8000_MACHINE(obj);
    g_free(s8000_machine->trustcache_filename);
    s8000_machine->trustcache_filename = g_strdup(value);
}

static char *s8000_get_trustcache_filename(Object *obj, Error **errp)
{
    S8000MachineState *s8000_machine;

    s8000_machine = S8000_MACHINE(obj);
    return g_strdup(s8000_machine->trustcache_filename);
}

static void s8000_set_ticket_filename(Object *obj, const char *value,
                                      Error **errp)
{
    S8000MachineState *s8000_machine;

    s8000_machine = S8000_MACHINE(obj);
    g_free(s8000_machine->ticket_filename);
    s8000_machine->ticket_filename = g_strdup(value);
}

static char *s8000_get_ticket_filename(Object *obj, Error **errp)
{
    S8000MachineState *s8000_machine;

    s8000_machine = S8000_MACHINE(obj);
    return g_strdup(s8000_machine->ticket_filename);
}

static void s8000_set_seprom_filename(Object *obj, const char *value,
                                      Error **errp)
{
    S8000MachineState *s8000_machine;

    s8000_machine = S8000_MACHINE(obj);
    g_free(s8000_machine->seprom_filename);
    s8000_machine->seprom_filename = g_strdup(value);
}

static char *s8000_get_seprom_filename(Object *obj, Error **errp)
{
    S8000MachineState *s8000_machine;

    s8000_machine = S8000_MACHINE(obj);
    return g_strdup(s8000_machine->seprom_filename);
}

static void s8000_set_sepfw_filename(Object *obj, const char *value,
                                     Error **errp)
{
    S8000MachineState *s8000_machine;

    s8000_machine = S8000_MACHINE(obj);
    g_free(s8000_machine->sep_fw_filename);
    s8000_machine->sep_fw_filename = g_strdup(value);
}

static char *s8000_get_sepfw_filename(Object *obj, Error **errp)
{
    S8000MachineState *s8000_machine;

    s8000_machine = S8000_MACHINE(obj);
    return g_strdup(s8000_machine->sep_fw_filename);
}

static void s8000_set_boot_mode(Object *obj, const char *value, Error **errp)
{
    S8000MachineState *s8000_machine;

    s8000_machine = S8000_MACHINE(obj);
    if (g_str_equal(value, "auto")) {
        s8000_machine->boot_mode = kBootModeAuto;
    } else if (g_str_equal(value, "manual")) {
        s8000_machine->boot_mode = kBootModeManual;
    } else if (g_str_equal(value, "enter_recovery")) {
        s8000_machine->boot_mode = kBootModeEnterRecovery;
    } else if (g_str_equal(value, "exit_recovery")) {
        s8000_machine->boot_mode = kBootModeExitRecovery;
    } else {
        s8000_machine->boot_mode = kBootModeAuto;
        error_setg(errp, "Invalid boot mode: %s", value);
    }
}

static char *s8000_get_boot_mode(Object *obj, Error **errp)
{
    S8000MachineState *s8000_machine;

    s8000_machine = S8000_MACHINE(obj);
    switch (s8000_machine->boot_mode) {
    case kBootModeManual:
        return g_strdup("manual");
    case kBootModeEnterRecovery:
        return g_strdup("enter_recovery");
    case kBootModeExitRecovery:
        return g_strdup("exit_recovery");
    case kBootModeAuto:
        QEMU_FALLTHROUGH;
    default:
        return g_strdup("auto");
    }
}

static void s8000_get_ecid(Object *obj, Visitor *v, const char *name,
                           void *opaque, Error **errp)
{
    S8000MachineState *s8000_machine;
    int64_t value;

    s8000_machine = S8000_MACHINE(obj);
    value = s8000_machine->ecid;
    visit_type_int(v, name, &value, errp);
}

static void s8000_set_ecid(Object *obj, Visitor *v, const char *name,
                           void *opaque, Error **errp)
{
    S8000MachineState *s8000_machine;
    int64_t value;

    s8000_machine = S8000_MACHINE(obj);

    if (!visit_type_int(v, name, &value, errp)) {
        return;
    }

    s8000_machine->ecid = value;
}

static void s8000_set_force_dfu(Object *obj, bool value, Error **errp)
{
    S8000MachineState *s8000_machine;

    s8000_machine = S8000_MACHINE(obj);
    s8000_machine->force_dfu = value;
}

static bool s8000_get_force_dfu(Object *obj, Error **errp)
{
    S8000MachineState *s8000_machine;

    s8000_machine = S8000_MACHINE(obj);
    return s8000_machine->force_dfu;
}

static void s8000_machine_class_init(ObjectClass *klass, void *data)
{
    MachineClass *mc;

    mc = MACHINE_CLASS(klass);
    mc->desc = "S8000";
    mc->init = s8000_machine_init;
    mc->reset = s8000_machine_reset;
    mc->max_cpus = A9_MAX_CPU;
    mc->no_sdcard = 1;
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
    mc->no_parallel = 1;
    mc->default_cpu_type = TYPE_APPLE_A9;
    mc->minimum_page_bits = 14;
    mc->default_ram_size = S8000_DRAM_SIZE;
    mc->fixup_ram_size = s8000_machine_fixup_ram_size;

    object_class_property_add_str(klass, "trustcache",
                                  s8000_get_trustcache_filename,
                                  s8000_set_trustcache_filename);
    object_class_property_set_description(klass, "trustcache",
                                          "Trustcache to be loaded");
    object_class_property_add_str(klass, "ticket", s8000_get_ticket_filename,
                                  s8000_set_ticket_filename);
    object_class_property_set_description(klass, "ticket",
                                          "APTicket to be loaded");
    object_class_property_add_str(klass, "seprom", s8000_get_seprom_filename,
                                  s8000_set_seprom_filename);
    object_class_property_set_description(klass, "seprom",
                                          "SEPROM to be loaded");
    object_class_property_add_str(klass, "sepfw", s8000_get_sepfw_filename,
                                  s8000_set_sepfw_filename);
    object_class_property_set_description(klass, "sepfw", "SEPFW to be loaded");
    object_class_property_add_str(klass, "boot-mode", s8000_get_boot_mode,
                                  s8000_set_boot_mode);
    object_class_property_set_description(klass, "boot-mode",
                                          "Boot mode of the machine");
    object_class_property_add_bool(klass, "kaslr-off", s8000_get_kaslr_off,
                                   s8000_set_kaslr_off);
    object_class_property_set_description(klass, "kaslr-off", "Disable KASLR");
    object_class_property_add(klass, "ecid", "uint64", s8000_get_ecid,
                              s8000_set_ecid, NULL, NULL);
    object_class_property_add_bool(klass, "force-dfu", s8000_get_force_dfu,
                                   s8000_set_force_dfu);
    object_class_property_set_description(klass, "force-dfu", "Force DFU");
}

static const TypeInfo s8000_machine_info = {
    .name = TYPE_S8000_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(S8000MachineState),
    .class_size = sizeof(S8000MachineClass),
    .class_init = s8000_machine_class_init,
};

static void s8000_machine_types(void)
{
    type_register_static(&s8000_machine_info);
}

type_init(s8000_machine_types)
