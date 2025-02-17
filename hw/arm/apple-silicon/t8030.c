/*
 * Apple T8030 SoC.
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
#include "exec/memattrs.h"
#include "exec/memory.h"
#include "hw/arm/apple-silicon/a13.h"
#include "hw/arm/apple-silicon/dart.h"
#include "hw/arm/apple-silicon/dtb.h"
#include "hw/arm/apple-silicon/lm-backlight.h"
#include "hw/arm/apple-silicon/mem.h"
#include "hw/arm/apple-silicon/mt-spi.h"
#include "hw/arm/apple-silicon/sart.h"
#include "hw/arm/apple-silicon/sep-sim.h"
#include "hw/arm/apple-silicon/sep.h"
#include "hw/arm/apple-silicon/t8030-config.c.inc"
#include "hw/arm/apple-silicon/t8030.h"
#include "hw/arm/apple-silicon/xnu_pf.h"
#include "hw/block/apple_ans.h"
#include "hw/char/apple_uart.h"
#include "hw/display/apple_displaypipe_v4.h"
#include "hw/dma/apple_sio.h"
#include "hw/gpio/apple_gpio.h"
#include "hw/i2c/apple_i2c.h"
#include "hw/intc/apple_aic.h"
#include "hw/irq.h"
#include "hw/misc/apple-silicon/aes.h"
#include "hw/misc/apple-silicon/roswell.h"
#include "hw/misc/apple-silicon/smc.h"
#include "hw/misc/apple-silicon/spmi-pmu.h"
#include "hw/nvram/apple_nvram.h"
#include "hw/spmi/apple_spmi.h"
#include "hw/ssi/apple_spi.h"
#include "hw/ssi/ssi.h"
#include "hw/usb/apple_typec.h"
#include "hw/watchdog/apple_wdt.h"
#include "qapi/visitor.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qemu/guest-random.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "sysemu/reset.h"
#include "sysemu/runstate.h"
#include "sysemu/sysemu.h"

#define T8030_SROM_BASE (0x100000000)
#define T8030_SROM_SIZE (0x80000)

#define T8030_SRAM_BASE (0x19C000000)
#define T8030_SRAM_SIZE (0x400000)

#define T8030_DRAM_BASE (0x800000000)

#define T8030_SEPROM_BASE (0x240000000)
#define T8030_SEPROM_SIZE (0x4000000)

#define T8030_GPIO_FORCE_DFU (161)

#define T8030_DISPLAY_SIZE (67 * MiB)

#define T8030_DWC2_IRQ (495)

#define T8030_NUM_UARTS (9)

#define T8030_ANS_TEXT_SIZE (0x124000)
#define T8030_ANS_DATA_SIZE (0x3C00000)
#define T8030_SMC_SRAM_SIZE (0x4000)
#define T8030_SIO_TEXT_SIZE (0x1C000)
#define T8030_SIO_DATA_SIZE (0xF8000)
#define T8030_PANIC_SIZE (0x100000)

#define T8030_AMCC_BASE (0x200000000)
#define T8030_AMCC_SIZE (0x100000)
#define AMCC_PLANE_COUNT (4)
#define AMCC_PLANE_STRIDE (0x40000)
#define AMCC_LOWER(_p) (0x680 + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_UPPER(_p) (0x684 + (_p) * AMCC_PLANE_STRIDE)
#define AMCC_REG(_tms, _x) *(uint32_t *)(&t8030_machine->amcc_reg[_x])

static size_t t8030_real_cpu_count(T8030MachineState *t8030_machine)
{
    MachineState *machine;

    machine = MACHINE(t8030_machine);

    return (t8030_machine->seprom_filename || t8030_machine->sep_fw_filename) ?
               machine->smp.cpus - 1 :
               machine->smp.cpus;
}

static void t8030_start_cpus(T8030MachineState *t8030_machine,
                             uint64_t cpu_mask)
{
    int i;

    for (i = 0; i < t8030_real_cpu_count(t8030_machine); i++) {
        if (test_bit(i, (unsigned long *)&cpu_mask) &&
            apple_a13_cpu_is_powered_off(t8030_machine->cpus[i])) {
            apple_a13_cpu_start(t8030_machine->cpus[i]);
        }
    }
}

static void t8030_create_s3c_uart(const T8030MachineState *t8030_machine,
                                  uint32_t port, Chardev *chr)
{
    DeviceState *dev;
    hwaddr base;
    // first fetch the uart mmio address
    int vector;
    DTBProp *prop;
    hwaddr *uart_offset;
    DTBNode *child = dtb_get_node(t8030_machine->device_tree, "arm-io/uart0");
    char name[32] = { 0 };

    g_assert_cmpuint(port, <, T8030_NUM_UARTS);

    g_assert_nonnull(child);
    snprintf(name, sizeof(name), "uart%d", port);

    prop = dtb_find_prop(child, "reg");
    g_assert_nonnull(prop);

    uart_offset = (hwaddr *)prop->data;
    base = t8030_machine->soc_base_pa + uart_offset[0] + uart_offset[1] * port;

    prop = dtb_find_prop(child, "interrupts");
    g_assert_nonnull(prop);

    vector = *(uint32_t *)prop->data + port;
    dev = apple_uart_create(
        base, 15, 0, chr, qdev_get_gpio_in(DEVICE(t8030_machine->aic), vector));
    g_assert_nonnull(dev);
    dev->id = g_strdup(name);
}

static void t8030_patch_kernel(MachoHeader64 *hdr)
{
    const uint32_t nop = cpu_to_le32(0xD503201F);

    //! disable_kprintf_output = 0;
    *(uint32_t *)vtop_slid(0xFFFFFFF0077142C8) = 0;

    //! AppleSEPManager::_initTimeoutMultiplier 'sim' -> '  m'
    *(uint32_t *)vtop_slid(0xFFFFFFF008B569E0) = cpu_to_le32(0x52840408);

    // Disable AMX
    // _gAMXVersion = 0
    // __cpu_capabilities | 0x800 (ucnormal)
    *(uint32_t *)vtop_slid(0xFFFFFFF007B64494) = cpu_to_le32(0x5280000A);
    *(uint32_t *)vtop_slid(0xFFFFFFF007B644A4) = cpu_to_le32(0x52810009);

#ifndef ENABLE_SEP
    // Make all AppleSEPKeyStoreUserClient requests do nothing but return
    // success
    *(uint32_t *)vtop_slid(0xFFFFFFF008F6F774) = cpu_to_le32(0x52800000);
    *(uint32_t *)vtop_slid(0xFFFFFFF008F6F778) = cpu_to_le32(0xD65F0FFF);
#else
    // re-enable it in case that the kernel is already patched
    *(uint32_t *)vtop_slid(0xFFFFFFF008F6F774) = cpu_to_le32(0xD10783FF);
    *(uint32_t *)vtop_slid(0xFFFFFFF008F6F778) = cpu_to_le32(0xA9186FFC);

    // `AppleSEPManager::_tracingEnabled`: disable
    // `PE_i_can_has_debugger` check, only rely on sep_tracing
    *(uint32_t *)vtop_slid(0xFFFFFFF008B576B4) = nop;

    // `AppleSEPManager::_bootSEP`: disable `PE_i_can_has_debugger`
    // check, so it won't skip reading bootarg sep-trace-size
    *(uint32_t *)vtop_slid(0xFFFFFFF008B57B28) = nop;

    // `AppleSEPManager::_loadChannelObjectEntries`:
    // use SCOT as TRAC, thus making it bigger: 0x10000 bytes instead of 0x20.
    *(uint32_t *)vtop_slid(0xFFFFFFF008B58030) = cpu_to_le32(0x52A00028);
#endif

    // com.apple.os.update- -> xom.apple.os.update-
    *(char *)vtop_slid(0xFFFFFFF0075085FC) = 'x';

    // Disable check for `PE_i_can_has_debugger` in MTSPI/MTDevice code
    // so we can use the boot arguments `mt-strings=1 mt-bytes=1`.
    *(uint32_t *)vtop_slid(0xFFFFFFF0085D70E4) = nop;
    *(uint32_t *)vtop_slid(0xFFFFFFF0087A5854) = nop;

    // Enable all AppleImage4 logs.
    // *(uint32_t *)vtop_slid(0xFFFFFFF008387A28) = nop;

    // Force call to `Img4DecodePerformTrustEvaluationWithCallbacks` return
    // value check to pass.
    *(uint32_t *)vtop_slid(0xFFFFFFF00838030C) = cpu_to_le32(0x52800000);
    *(uint32_t *)vtop_slid(0xFFFFFFF008380310) = nop;

    // this will tell launchd this is an internal build,
    // and that way we can get hactivation without bypassing
    // or patching the activation procedure.
    // This is NOT an iCloud bypass. This is utilising code that ALREADY exists
    // in the activation daemon. This is essentially telling iOS, it's a
    // development kernel/device, NOT the real product sold on market. IF you
    // decide to use this knowledge to BYPASS technological countermeasures
    // or any other intellectual theft or crime, YOU are responsible in full,
    // AND SHOULD BE PROSECUTED TO THE FULL EXTENT OF THE LAW.
    // We do NOT endorse nor approve the theft of property.
    memcpy((char *)vtop_slid(0xFFFFFFF00703884E), "profile", 8);

    xnu_kpf(hdr);
}

static bool t8030_check_panic(T8030MachineState *t8030_machine)
{
    AppleEmbeddedPanicHeader *panic_info;
    bool ret;

    if (t8030_machine->panic_size == 0) {
        return false;
    }

    panic_info = g_malloc0(t8030_machine->panic_size);

    address_space_rw(&address_space_memory, t8030_machine->panic_base,
                     MEMTXATTRS_UNSPECIFIED, panic_info,
                     t8030_machine->panic_size, false);
    address_space_set(&address_space_memory, t8030_machine->panic_base, 0,
                      t8030_machine->panic_size, MEMTXATTRS_UNSPECIFIED);

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

static void get_kaslr_slides(T8030MachineState *t8030_machine,
                             hwaddr *phys_slide_out, hwaddr *virt_slide_out)
{
    hwaddr slide_phys = 0, slide_virt = 0;
    const size_t slide_granular = (1 << 21);
    const size_t slide_granular_mask = slide_granular - 1;
    const size_t slide_virt_max = 0x100 * (2 * 1024 * 1024);
    size_t random_value = get_kaslr_random();

    if (t8030_machine->kaslr_off) {
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

static void t8030_load_classic_kc(T8030MachineState *t8030_machine,
                                  const char *cmdline, CarveoutAllocator *ca)
{
    MachineState *machine = MACHINE(t8030_machine);
    MachoHeader64 *hdr = t8030_machine->kernel;
    MemoryRegion *sysmem = t8030_machine->sysmem;
    AddressSpace *nsas = &address_space_memory;
    hwaddr virt_low;
    hwaddr virt_end;
    hwaddr dtb_va;
    hwaddr mem_size;
    hwaddr phys_ptr;
    hwaddr amcc_lower;
    hwaddr amcc_upper;
    AppleBootInfo *info = &t8030_machine->bootinfo;
    hwaddr last_base;
    MachoSegmentCommand64 *last_seg;
    hwaddr text_base;
    DTBNode *memory_map =
        dtb_get_node(t8030_machine->device_tree, "/chosen/memory-map");

    g_phys_base = (hwaddr)macho_get_buffer(hdr);
    macho_highest_lowest(hdr, &virt_low, &virt_end);
    g_virt_base = virt_low;

    get_kaslr_slides(t8030_machine, &g_phys_slide, &g_virt_slide);

    last_seg = macho_get_segment(hdr, "__LAST");
    last_base = last_seg->vmaddr;
    text_base = macho_get_segment(hdr, "__TEXT")->vmaddr;

    g_phys_base = T8030_DRAM_BASE;
    g_virt_base += g_virt_slide - g_phys_slide;

    // TrustCache
    info->trustcache_addr = vtop_slid(text_base) - info->trustcache_size;

    macho_load_trustcache(t8030_machine->trustcache, info->trustcache_size,
                          nsas, sysmem, info->trustcache_addr);

    info->kern_entry = arm_load_macho(hdr, nsas, sysmem, memory_map,
                                      g_phys_base + g_phys_slide, g_virt_slide);
    info_report("Kernel virtual base: 0x" TARGET_FMT_lx, g_virt_base);
    info_report("Kernel physical base: 0x" TARGET_FMT_lx, g_phys_base);
    info_report("Kernel virtual slide: 0x" TARGET_FMT_lx, g_virt_slide);
    info_report("Kernel physical slide: 0x" TARGET_FMT_lx, g_phys_slide);
    info_report("Kernel entry point: 0x" TARGET_FMT_lx, info->kern_entry);

    virt_end += g_virt_slide;
    phys_ptr = vtop_static(ROUND_UP_16K(virt_end));

    amcc_lower = info->trustcache_addr;
    amcc_upper = vtop_slid(last_base) + last_seg->vmsize - 1;
    for (int i = 0; i < 4; i++) {
        AMCC_REG(t8030_machine, AMCC_LOWER(i)) =
            (amcc_lower - T8030_DRAM_BASE) >> 14;
        AMCC_REG(t8030_machine, AMCC_UPPER(i)) =
            (amcc_upper - T8030_DRAM_BASE) >> 14;
    }

    // RAM Disk
    if (machine->initrd_filename != NULL) {
        info->ramdisk_addr = phys_ptr;
        macho_load_ramdisk(machine->initrd_filename, nsas, sysmem,
                           info->ramdisk_addr, &info->ramdisk_size);
        info->ramdisk_size = ROUND_UP_16K(info->ramdisk_size);
        phys_ptr += info->ramdisk_size;
    }

    // SEPFW
    info->sep_fw_addr = phys_ptr;
    if (t8030_machine->sep_fw_filename != NULL) {
        macho_load_raw_file(t8030_machine->sep_fw_filename, nsas, sysmem,
                            "sepfw", info->sep_fw_addr, &info->sep_fw_size);
        AppleSEPState *sep = APPLE_SEP(object_property_get_link(
            OBJECT(t8030_machine), "sep", &error_fatal));
        sep->sep_fw_addr = info->sep_fw_addr;
        sep->sep_fw_size = info->sep_fw_size;
        g_file_get_contents(t8030_machine->sep_fw_filename, &sep->sepfw_data,
                            NULL, NULL);
    }
    info->sep_fw_size = SEPFW_MAPPING_SIZE;
    phys_ptr += info->sep_fw_size;

    // Kernel boot args
    info->kern_boot_args_addr = phys_ptr;
    info->kern_boot_args_size = 0x4000;
    phys_ptr += info->kern_boot_args_size;

    // Device tree
    info->device_tree_addr = phys_ptr;
    dtb_va = ptov_static(info->device_tree_addr);
    phys_ptr += info->device_tree_size;
    info_report("Device tree physical base: 0x" TARGET_FMT_lx,
                info->device_tree_addr);
    info_report("Device tree virtual base: 0x" TARGET_FMT_lx, dtb_va);
    info_report("Device tree size: 0x" TARGET_FMT_lx, info->device_tree_size);

    mem_size = carveout_alloc_finalise(ca);
    info_report("mem_size: 0x%" PRIx64 "", mem_size);

    macho_load_dtb(t8030_machine->device_tree, nsas, sysmem, "DeviceTree",
                   info);

    info->top_of_kernel_data_pa = ROUND_UP_16K(phys_ptr);

    info_report("Boot args: [%s]", cmdline);
    macho_setup_bootargs(
        "BootArgs", nsas, sysmem, info->kern_boot_args_addr, g_virt_base,
        g_phys_base, mem_size, info->top_of_kernel_data_pa, dtb_va,
        info->device_tree_size, t8030_machine->video_args, cmdline);
    g_virt_base = virt_low;
}

static void t8030_load_fileset_kc(T8030MachineState *t8030_machine,
                                  const char *cmdline, CarveoutAllocator *ca)
{
    MachineState *machine = MACHINE(t8030_machine);
    MachoHeader64 *hdr = t8030_machine->kernel;
    MemoryRegion *sysmem = t8030_machine->sysmem;
    AddressSpace *nsas = &address_space_memory;
    hwaddr virt_low;
    hwaddr virt_end;
    hwaddr dtb_va;
    hwaddr mem_size;
    hwaddr phys_ptr;
    hwaddr amcc_lower;
    hwaddr amcc_upper;
    AppleBootInfo *info = &t8030_machine->bootinfo;
    uint64_t extradata_size;
    uint64_t l2_remaining;
    MachoSegmentCommand64 *prelink_info_seg;
    DTBNode *memory_map =
        dtb_get_node(t8030_machine->device_tree, "/chosen/memory-map");

    g_phys_base = (hwaddr)macho_get_buffer(hdr);
    macho_highest_lowest(hdr, &virt_low, &virt_end);
    g_virt_base = virt_low;

    prelink_info_seg = macho_get_segment(hdr, "__PRELINK_INFO");

    extradata_size =
        ROUND_UP_16K(info->device_tree_size + info->trustcache_size);
    g_assert_cmpuint(extradata_size, <, L2_GRANULE);

    get_kaslr_slides(t8030_machine, &g_phys_slide, &g_virt_slide);

    l2_remaining = (virt_low + g_virt_slide) & L2_GRANULE_MASK;

    if (extradata_size >= l2_remaining) {
        uint64_t grown_slide = ROUND_UP_16K(extradata_size - l2_remaining);
        g_phys_slide += grown_slide;
        g_virt_slide += grown_slide;
    }

    g_phys_base = phys_ptr = T8030_DRAM_BASE;
    phys_ptr |= (virt_low & L2_GRANULE_MASK);
    phys_ptr += g_phys_slide;
    phys_ptr -= extradata_size;

    // Device tree
    info->device_tree_addr = phys_ptr;
    phys_ptr += info->device_tree_size;

    // TrustCache
    info->trustcache_addr = phys_ptr;
    macho_load_trustcache(t8030_machine->trustcache, info->trustcache_size,
                          nsas, sysmem, info->trustcache_addr);
    phys_ptr += ROUND_UP_16K(info->trustcache_size);

    g_virt_base += g_virt_slide;
    g_virt_base -= phys_ptr - g_phys_base;
    info->kern_entry =
        arm_load_macho(hdr, nsas, sysmem, memory_map, phys_ptr, g_virt_slide);
    info_report("Kernel virtual base: 0x" TARGET_FMT_lx, g_virt_base);
    info_report("Kernel physical base: 0x" TARGET_FMT_lx, g_phys_base);
    info_report("Kernel virtual slide: 0x" TARGET_FMT_lx, g_virt_slide);
    info_report("Kernel physical slide: 0x" TARGET_FMT_lx, g_phys_slide);
    info_report("Kernel entry point: 0x" TARGET_FMT_lx, info->kern_entry);

    virt_end += g_virt_slide;
    phys_ptr = vtop_static(ROUND_UP_16K(virt_end));

    amcc_lower = info->device_tree_addr;
    amcc_upper =
        vtop_slid(prelink_info_seg->vmaddr) + prelink_info_seg->vmsize - 1;
    for (int i = 0; i < 4; i++) {
        AMCC_REG(t8030_machine, AMCC_LOWER(i)) =
            (amcc_lower - T8030_DRAM_BASE) >> 14;
        AMCC_REG(t8030_machine, AMCC_UPPER(i)) =
            (amcc_upper - T8030_DRAM_BASE) >> 14;
    }

    dtb_va = ptov_static(info->device_tree_addr);

    if (machine->initrd_filename != NULL) {
        info->ramdisk_addr = phys_ptr;
        macho_load_ramdisk(machine->initrd_filename, nsas, sysmem,
                           info->ramdisk_addr, &info->ramdisk_size);
        info->ramdisk_size = ROUND_UP_16K(info->ramdisk_size);
        phys_ptr += info->ramdisk_size;
    }

    // SEPFW
    info->sep_fw_addr = phys_ptr;
    if (t8030_machine->sep_fw_filename != NULL) {
        macho_load_raw_file(t8030_machine->sep_fw_filename, nsas, sysmem,
                            "sepfw", info->sep_fw_addr, &info->sep_fw_size);
        AppleSEPState *sep = APPLE_SEP(object_property_get_link(
            OBJECT(t8030_machine), "sep", &error_fatal));
        sep->sep_fw_addr = info->sep_fw_addr;
        sep->sep_fw_size = info->sep_fw_size;
        g_file_get_contents(t8030_machine->sep_fw_filename, &sep->sepfw_data,
                            NULL, NULL);
    }
    info->sep_fw_size = SEPFW_MAPPING_SIZE;
    phys_ptr += info->sep_fw_size;

    // Kernel boot args
    info->kern_boot_args_addr = phys_ptr;
    info->kern_boot_args_size = 0x4000;
    phys_ptr += info->kern_boot_args_size;

    mem_size = carveout_alloc_finalise(ca);

    macho_load_dtb(t8030_machine->device_tree, nsas, sysmem, "DeviceTree",
                   info);

    info->top_of_kernel_data_pa = ROUND_UP_16K(phys_ptr);

    info_report("Boot args: [%s]", cmdline);
    macho_setup_bootargs(
        "BootArgs", nsas, sysmem, info->kern_boot_args_addr, g_virt_base,
        g_phys_base, mem_size, info->top_of_kernel_data_pa, dtb_va,
        info->device_tree_size, t8030_machine->video_args, cmdline);
    g_virt_base = virt_low;
}

static void t8030_rtkit_mem_setup(T8030MachineState *t8030_machine,
                                  CarveoutAllocator *ca, const char *name,
                                  const char *nub_name, hwaddr text_size,
                                  hwaddr data_size)
{
    DTBNode *child;
    AppleIopSegmentRange segranges[2];

    child = dtb_get_node(t8030_machine->device_tree, "arm-io");
    g_assert_nonnull(child);
    child = dtb_get_node(child, name);
    g_assert_nonnull(child);
    child = dtb_get_node(child, nub_name);
    g_assert_nonnull(child);

    segranges[0].phys = carveout_alloc_mem(ca, text_size + data_size);
    segranges[0].virt = 0x0;
    segranges[0].remap = segranges[0].phys;
    segranges[0].size = text_size;
    segranges[0].flag = 0x1;

    segranges[1].phys = segranges[0].phys + T8030_SIO_TEXT_SIZE;
    segranges[1].virt = segranges[0].virt + text_size;
    segranges[1].remap = segranges[1].phys;
    segranges[1].size = data_size;
    segranges[1].flag = 0x0;

    dtb_set_prop_u64(child, "region-base", segranges[0].phys);
    dtb_set_prop_u64(child, "region-size", text_size + data_size);
    dtb_set_prop(child, "segment-ranges", sizeof(segranges), segranges);
}

static void t8030_memory_setup(T8030MachineState *t8030_machine)
{
    MachineState *machine;
    MachoHeader64 *hdr;
    AppleNvramState *nvram;
    AppleBootInfo *info;
    DTBNode *memory_map;
    AddressSpace *nsas;
    char *cmdline;
    char *seprom;
    gsize fsize;
    CarveoutAllocator *ca;

    DTBNode *carveout_memory_map =
        dtb_get_node(t8030_machine->device_tree, "/chosen/carveout-memory-map");
    if (carveout_memory_map == NULL) {
        fprintf(stderr,
                "%s: warning: carveout-memory-map unavailable? iOS 13?\n",
                __func__);
        DTBNode *chosen = dtb_get_node(t8030_machine->device_tree, "chosen");
        carveout_memory_map = dtb_create_node(chosen, "carveout-memory-map");
    }

    machine = MACHINE(t8030_machine);
    info = &t8030_machine->bootinfo;
    memory_map = dtb_get_node(t8030_machine->device_tree, "/chosen/memory-map");
    nsas = &address_space_memory;

    if (t8030_check_panic(t8030_machine)) {
        qemu_system_guest_panicked(NULL);
        return;
    }

    info->dram_base = T8030_DRAM_BASE;
    info->dram_size = machine->maxram_size;

    ca = carveout_alloc_new(carveout_memory_map, info->dram_base,
                            info->dram_size, 16 * KiB);

    t8030_rtkit_mem_setup(t8030_machine, ca, "sio", "iop-sio-nub",
                          T8030_SIO_TEXT_SIZE, T8030_SIO_DATA_SIZE);
    t8030_rtkit_mem_setup(t8030_machine, ca, "ans", "iop-ans-nub",
                          T8030_ANS_TEXT_SIZE, T8030_ANS_DATA_SIZE);

    if (t8030_machine->seprom_filename) {
        if (!g_file_get_contents(t8030_machine->seprom_filename, &seprom,
                                 &fsize, NULL)) {
            error_setg(&error_fatal, "Could not load data from file '%s'",
                       t8030_machine->seprom_filename);
            return;
        }
        // Apparently needed because of a bug occurring on XNU
        address_space_set(nsas, 0x300000000ULL, 0, 0x8000000ULL,
                          MEMTXATTRS_UNSPECIFIED);
        address_space_set(nsas, 0x340000000ULL, 0, 0x2000000ULL,
                          MEMTXATTRS_UNSPECIFIED);
        address_space_rw(nsas, T8030_SEPROM_BASE, MEMTXATTRS_UNSPECIFIED,
                         (uint8_t *)seprom, fsize, true);

        g_free(seprom);

        uint64_t value = 0x8000000000000000;
        uint32_t value32_mov_x0_1 = 0xD2800020; // mov x0, #0x1
        uint32_t value32_mov_x0_0 = 0xD2800000; // mov x0, #0x0
        uint32_t value32_nop = 0xD503201F; // nop
        // mov w0, #0x10000000
        // uint32_t value32_mov_w0_0x10000000 = 0x52a20000;
        // uint32_t value32_mov_w0_0 = 0x52800000; // mov w0, #0x0
        // uint32_t value32_mov_w8_0x1000 = 0x52820008;
#if 1 // for T8030 SEPROM
      // _entry: prevent busy-loop (data section):
      // 240000024: data_242140108 = 0x4 should set
      // (data_242140108 & 0x8000000000000000) != 0
        address_space_write(nsas, t8030_machine->soc_base_pa + 0x42140108,
                            MEMTXATTRS_UNSPECIFIED, &value, sizeof(value));

        // image4_validate_property_callback: skip AMNM
        // address_space_write(nsas, T8030_SEPROM_BASE + 0x0D2C8,
        //                     MEMTXATTRS_UNSPECIFIED, &value32_nop,
        //                     sizeof(value32_nop));

        // maybe_Img4DecodeEvaluateTrust: Skip RSA verification result.
        // not actually stuck, it just takes a while to complete while GDB is in
        // use.
        // address_space_write(nsas, T8030_SEPROM_BASE + 0x1130c,
        //                     MEMTXATTRS_UNSPECIFIED, &value32_nop,
        //                     sizeof(value32_nop));

        // maybe_Img4DecodeEvaluateTrust: payload_raw
        // hashing stuck, nop'ing
        address_space_write(nsas, T8030_SEPROM_BASE + 0x113b0,
                            MEMTXATTRS_UNSPECIFIED, &value32_nop,
                            sizeof(value32_nop));

        // maybe_Img4DecodeEvaluateTrust: nop'ing
        // result of payload_raw hashing
        address_space_write(nsas, T8030_SEPROM_BASE + 0x113b4,
                            MEMTXATTRS_UNSPECIFIED, &value32_nop,
                            sizeof(value32_nop));

        // memcmp_validstrs30: fake success
        address_space_write(nsas, T8030_SEPROM_BASE + 0x0963c,
                            MEMTXATTRS_UNSPECIFIED, &value32_mov_x0_0,
                            sizeof(value32_mov_x0_0));

        // memcmp_validstrs14: fake success; for nvram bypass?
        address_space_write(nsas, T8030_SEPROM_BASE + 0x0b574,
                            MEMTXATTRS_UNSPECIFIED, &value32_mov_x0_0,
                            sizeof(value32_mov_x0_0));

        // load_sepos: jump over
        // img4_compare_verified_values_true_on_success
        address_space_write(nsas, T8030_SEPROM_BASE + 0x06234,
                            MEMTXATTRS_UNSPECIFIED, &value32_mov_x0_1,
                            sizeof(value32_mov_x0_1));

        // maybe_verify_rsa_signature: return
        // fake return value
        address_space_write(nsas, T8030_SEPROM_BASE + 0x11630,
                            MEMTXATTRS_UNSPECIFIED, &value32_mov_x0_0,
                            sizeof(value32_mov_x0_0));

        // `AppleSEPBooter::getBootTimeout`:
        // increase timeout for debugging (GDB tracing). The
        // `_initTimeoutMultiplier` change is preferred compared to this.
        // *(uint32_t *)vtop_slid(0xFFFFFFF008B4E018) =
        // value32_mov_w0_0x10000000;

        // `AppleSEPManager::_tracingEnabled`: Don't require
        // `PE_i_can_has_debugger`.
        //*(uint32_t *)vtop_slid(0xFFFFFFF008B576B4) = value32_nop;

        // `AppleSEPManager::_bootSEP`: Don't require `PE_i_can_has_debugger`.
        // *(uint32_t *)vtop_slid(0xFFFFFFF008B57AD4) = value32_mov_x0_1;

        // `AppleSEPManager::_initPMControl`: Don't require
        // `PE_i_can_has_debugger`.
        // *(uint32_t *)vtop_slid(0xFFFFFFF008B56B18) = value32_nop;

        // _kern_config_is_development
        // *(uint32_t *)vtop_slid(0xFFFFFFF007A231D8) = value32_mov_x0_1;

        // `AppleSEPManager::_initTimeoutMultiplier`:
        // Increasing the FastSIM timeout. Conflicts with getBootTimeout.
        // *(uint32_t *)vtop_slid(0xFFFFFFF008B56AA4) = value32_mov_w8_0x1000;

        // `SEP::_kern_register_coredump_helper`: Needed for
        // the bootSEP patch.
        // *(uint32_t *)vtop_slid(0xFFFFFFF0079F95A8) = value32_mov_w0_0;

        // `ApplePMGR::_cpuIdle`: skip time check
        // *(uint32_t *)vtop_slid(0xFFFFFFF008794F24) = value32_mov_x0_0;

        // memcmp_validstrs20: fake success
        // address_space_write(nsas, T8030_SEPROM_BASE + 0x02A04,
        // MEMTXATTRS_UNSPECIFIED, &value32_mov_x0_0, sizeof(value32_mov_x0_0));
#endif // for T8030 SEPROM
    }

    nvram = APPLE_NVRAM(qdev_find_recursive(sysbus_get_default(), "nvram"));
    if (nvram == NULL) {
        error_setg(&error_abort, "Failed to find NVRAM device");
        return;
    };
    apple_nvram_load(nvram);

    info_report("boot_mode: %u", t8030_machine->boot_mode);
    switch (t8030_machine->boot_mode) {
    case kBootModeEnterRecovery:
        env_set(nvram, "auto-boot", "false", 0);
        t8030_machine->boot_mode = kBootModeAuto;
        break;
    case kBootModeExitRecovery:
        env_set(nvram, "auto-boot", "true", 0);
        t8030_machine->boot_mode = kBootModeAuto;
        break;
    default:
        break;
    }

    info_report("auto-boot=%s",
                env_get_bool(nvram, "auto-boot", false) ? "true" : "false");

    if (t8030_machine->boot_mode == kBootModeAuto &&
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

    if (t8030_machine->ticket_filename) {
        if (!g_file_get_contents(t8030_machine->ticket_filename,
                                 &info->ticket_data,
                                 (gsize *)&info->ticket_length, NULL)) {
            error_report("`%s` file read failed.",
                         t8030_machine->ticket_filename);
        }
    }

    DTBNode *chosen = dtb_get_node(t8030_machine->device_tree, "chosen");
    if (xnu_contains_boot_arg(cmdline, "-restore", false)) {
        // HACK: Use DEV model to restore without FDR errors
        dtb_set_prop(t8030_machine->device_tree, "compatible", 28,
                     "N104DEV\0iPhone12,1\0AppleARM");
    } else {
        dtb_set_prop(t8030_machine->device_tree, "compatible", 27,
                     "N104AP\0iPhone12,1\0AppleARM");
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

    DTBNode *pram = dtb_get_node(t8030_machine->device_tree, "pram");
    if (pram) {
        uint64_t panic_reg[2];
        panic_reg[0] = carveout_alloc_mem(ca, T8030_PANIC_SIZE);
        panic_reg[1] = T8030_PANIC_SIZE;
        dtb_set_prop(pram, "reg", sizeof(panic_reg), &panic_reg);
        dtb_set_prop_u64(chosen, "embedded-panic-log-size", panic_reg[1]);
        t8030_machine->panic_base = panic_reg[0];
        t8030_machine->panic_size = panic_reg[1];
    }

    DTBNode *vram = dtb_get_node(t8030_machine->device_tree, "vram");
    if (vram) {
        uint64_t vram_reg[2];
        vram_reg[0] = carveout_alloc_mem(ca, T8030_DISPLAY_SIZE);
        vram_reg[1] = T8030_DISPLAY_SIZE;
        dtb_set_prop(vram, "reg", sizeof(vram_reg), &vram_reg);
        t8030_machine->video_args.base_addr = vram_reg[0];
        AppleDisplayPipeV4State *display =
            APPLE_DISPLAY_PIPE_V4(object_property_get_link(
                OBJECT(t8030_machine), "disp0", &error_abort));
        if (memory_region_is_mapped(&display->vram)) {
            memory_region_del_subregion(t8030_machine->sysmem, &display->vram);
        }
        memory_region_add_subregion_overlap(t8030_machine->sysmem,
                                            t8030_machine->video_args.base_addr,
                                            &display->vram, 1);
    }

    hdr = t8030_machine->kernel;
    g_assert_nonnull(hdr);

    macho_allocate_segment_records(memory_map, hdr);

    macho_populate_dtb(t8030_machine->device_tree, info);

    switch (hdr->file_type) {
    case MH_EXECUTE:
        t8030_load_classic_kc(t8030_machine, cmdline, ca);
        break;
    case MH_FILESET:
        t8030_load_fileset_kc(t8030_machine, cmdline, ca);
        break;
    default:
        error_setg(&error_abort, "Unsupported kernelcache type: 0x%x\n",
                   hdr->file_type);
        break;
    }

    g_free(cmdline);
}

static uint64_t pmgr_unk_e4800 = 0;
static uint32_t pmgr_unk_e4000[0x180 / 4] = { 0 };

static void pmgr_unk_reg_write(void *opaque, hwaddr addr, uint64_t data,
                               unsigned size)
{
    hwaddr base = (hwaddr)opaque;
    switch (base + addr) {
    case 0x3D2E4800: // ???? 0x240002c00 and 0x2400037a4
        pmgr_unk_e4800 = data; // 0x240002c00 and 0x2400037a4
        break;
    case 0x3D2E4000 ... 0x3D2E417f: // ???? 0x24000377c
        pmgr_unk_e4000[((base + addr) - 0x3D2E4000) / 4] = data; // 0x24000377c
        break;
    default:
        break;
    }
#if 0
    qemu_log_mask(LOG_UNIMP,
                  "PMGR reg WRITE unk @ 0x" TARGET_FMT_lx
                  " base: 0x" TARGET_FMT_lx " value: 0x" TARGET_FMT_lx "\n",
                  base + addr, base, data);
#endif
}

static uint64_t pmgr_unk_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    T8030MachineState *t8030_machine = T8030_MACHINE(qdev_get_machine());
    AppleSEPState *sep;
    hwaddr base = (hwaddr)opaque;
    sep = (AppleSEPState *)object_dynamic_cast(
        object_property_get_link(OBJECT(t8030_machine), "sep", &error_fatal),
        TYPE_APPLE_SEP);

#if 0
    if ((((base + addr) & 0xfffffffb) != 0x10E20020) &&
        (((base + addr) & 0xfffffffb) != 0x11E20020)) {
        qemu_log_mask(LOG_UNIMP,
                      "PMGR reg READ unk @ 0x" TARGET_FMT_lx
                      " base: 0x" TARGET_FMT_lx "\n",
                      base + addr, base);
    }
#endif
    uint32_t chip_revision;
    uint64_t board_id;
    uint64_t security_epoch; // On IMG4: Security Epoch ; On IMG3: Minimum
                             // Epoch, verified on SecureROM s5l8955xsi
    int current_prod = 1;
    int current_secure_mode = 1; // T8015 SEPOS Kernel also requires this.
    int security_domain = 1;
    int raw_prod = 1;
    int raw_secure_mode = 1;
    // sep_bit30_current_value should stay zero on raw and only change current.
    int sep_bit30_current_value = 0;
    // chip_revision = 0x01;
    chip_revision = 0x11;
    // board_id = 0x4;
    board_id = t8030_machine->board_id;
    security_epoch = 0x1;
    // current_prod = raw_prod = current_secure_mode = raw_secure_mode = 0;
    switch (base + addr) {
    case 0x3D280088: // PMGR_AON
        return 0xFF;
    case 0x3D2BC000: // CURRENT_PROD?
    case 0x3D2BC400: // ??? maybe T8030 current_prod???
        // if 0xBC404 returns 1==0xA55AC33C, this will get ignored
        if (current_prod == 1)
            return 0xA55AC33C; // IBFL | 0x10 // CPFM | 0x03 ; IBFL_base == 0x0C
        return 0xA050C030; // IBFL | 0x00 // CPFM | 0x00 ; IBFL_base == 0x04
    case 0x3D2BC200: // RAW_PROD T8020 AP/SEP
        // if (sep->pmgr_base_regs[0x68] != 0) // if T8020 AP/SEP current_prod
        // and raw_prod are disabled, scrd sets a flag
        //     return 0xA050C030;
        // return 0xA55AC33C; // IBFL | 0x10
        // return 0xA050C030; // not prod. breaks SEPROM
        if (raw_prod == 1)
            return 0xA55AC33C; // IBFL | 0x10
        return 0xA050C030; // IBFL | 0x00
    case 0x3D2BC004: // Current Secure Mode SEP T8020
        // handle SEP DSEC demotion
        if (sep != NULL && sep->pmgr_fuse_changer_bit1_was_set)
            return 0xA050C030; // SEP DSEC img4 tag demotion active
        QEMU_FALLTHROUGH;
    case 0x3D2BC404: // maybe T8030 current secure mode???
        if (current_secure_mode)
            return 0xA55AC33C; // CPFM | 0x01 ; IBFL_base == 0x0C
        return 0xA050C030; // CPFM | 0x00 ; IBFL_base == 0x04
    case 0x3D2BC204: // Raw Secure Mode AP/SEP T8020
    case 0x3D2BC604: //? maybe also raw secure mode for T8030???
        if (raw_secure_mode)
            return 0xA55AC33C; // CPFM | 0x01 ; IBFL_base == 0x0C
        return 0xA050C030;
    case 0x3D2BC008:
    case 0x3D2BC208: // Security (raw?) Domain BIT0 T8020 SEP
        if ((security_domain & (1 << 0)) != 0)
            return 0xA55AC33C; // security domain | 0x1
        return 0xA050C030;
    case 0x3D2BC00C:
    case 0x3D2BC20C: // Security Domain (raw?) BIT1 T8020 SEP
        if ((security_domain & (1 << 1)) != 0)
            return 0xA55AC33C; // security domain | 0x2
        return 0xA050C030; // security domain | 0x0
    case 0x3D2BC010:
        sep_bit30_current_value =
            ((sep == NULL) ? 0 : (sep->pmgr_fuse_changer_bit0_was_set << 30));
        QEMU_FALLTHROUGH;
    case 0x3D2BC210: // (raw?) board id/minimum epoch? //CEPO? SEPO?
                     // AppleSEPROM-A12-D331pAP
        return ((board_id >> 5) & 0x7) | ((security_epoch & 0x7f) << 5) |
               sep_bit30_current_value | (1 << 31);
        // (security epoch & 0x7F) << 5 ;; (sep_bit30 << 30)
        // for SEP | (1 << 31) for SEP and AP
    case 0x3D2BC020: // T8030 iBSS: FUN_19c07feac_return_value_causes_crash;
                     // same address on T8020 iBoot, but possibly different
                     // handling
        if (1)
            return 0xA55AC33C;
        return 0xA050C030; // causes panic, so does a invalid value
    case 0x3D2BC02c: // Unknown SEP T8020
        // return 0xc000ce71;
        // return 0 << 30; // no panic
        // return (1 << 31) | (1 << 30); // panic
        return (0 << 31) | (1 << 30); // // bit31 causes a panic
    case 0x3D2BC030: // CPRV (Chip Revision) T8030 T8020
        // return 0xa6016242;
        // return ((chip_revision & 0x7) << 6) | (((chip_revision & 0x70) >> 4)
        // << 5); // LOW&HIGH NIBBLE T8030, T8020 and AppleSEPROM-S4-S5-B1 //
        // maybe 0x1c0 instead of 0x7 return ((chip_revision & 0x7) << 6) |
        // (((chip_revision & 0x70) >> 4) << 5) | (1 << 1); // LOW&HIGH NIBBLE
        // T8030, T8020 and AppleSEPROM-S4-S5-B1 // maybe 0x1c0 instead of 0x7
        return ((chip_revision & 0x7) << 6) |
               (((chip_revision & 0x70) >> 4) << 5) |
               (0 << 1); // LOW&HIGH NIBBLE T8030, T8020 and
                         // AppleSEPROM-S4-S5-B1 // maybe 0x1c0 instead of 0x7 ;
                         // bit1 being set causes kernel data abort
    case 0x3D2BC100: // ECID LOW T8020
    case 0x3D2BC300: // TODO
        return t8030_machine->ecid & 0xffffffff; // ECID lower
    case 0x3D2BC104: // ECID HIGH T8020
    case 0x3D2BC304: // TODO
        return t8030_machine->ecid >> 32; // ECID upper
    case 0x3D2BC10C: // T8020 SEP Chip Revision
    case 0x3D2BC30C: // T8030 SEP Chip Revision
        // case 0x3D2BC30c: // Maybe the T8030 SEP equivalent?
        //  1 vs. not 1: TRNG/Monitor
        //  0 vs. not 0: Monitor
        //  2 vs. not 2: ARTM
        //  Production SARS doesn't like value (0 << 28) in combination with
        //  kbkdf_index being 0
        // return 0 << 28; // 0 ; might be the production value
        // return 2 << 28; // 1
        // return 3 << 28; // 1
        return 8 << 28; // 2 ; might be a development value, skips a few checks
                        // (lynx and others)
    case 0x3D2E8000: // ????
        // return 0x32B3; // memory encryption AMK (Authentication Master Key)
        // disabled
        return 0xC2E9; // memory encryption AMK (Authentication Master Key)
                       // enabled
    case 0x3D2E4800: // ???? 0x240002c00 and 0x2400037a4
        return pmgr_unk_e4800; // 0x240002c00 and 0x2400037a4
    case 0x3D2E4000 ... 0x3D2E417f: // ???? 0x24000377c
        return pmgr_unk_e4000[((base + addr) - 0x3D2E4000) / 4]; // 0x24000377c
    ///
    case 0x3c100c4c: // Could that also have been for T8015? No idea anymore.
        return 0x1;
    ///
    default:
        if (((base + addr) & 0x10E70000) == 0x10E70000) {
            return (108 << 4) | 0x200000; //?
        }
        return 0;
    }
}

static const MemoryRegionOps pmgr_unk_reg_ops = {
    .write = pmgr_unk_reg_write,
    .read = pmgr_unk_reg_read,
};

static void pmgr_reg_write(void *opaque, hwaddr addr, uint64_t data,
                           unsigned size)
{
    T8030MachineState *t8030_machine = T8030_MACHINE(opaque);
    AppleSEPState *sep;
    uint32_t value = data;

    if (addr >= 0x80000 && addr <= 0x8C000) {
        value = (value & 0xF) << 4 | (value & 0xF);
    }
#if 0
    qemu_log_mask(LOG_UNIMP,
                  "PMGR reg WRITE @ 0x" TARGET_FMT_lx " value: 0x" TARGET_FMT_lx
                  "\n",
                  addr, data);
#endif
    switch (addr) {
    case 0xD4004:
        t8030_start_cpus(t8030_machine, data);
        return;
    case 0x80C00:
        // case 0x80400: // T8015
        sep = (AppleSEPState *)object_dynamic_cast(
            object_property_get_link(OBJECT(t8030_machine), "sep",
                                     &error_fatal),
            TYPE_APPLE_SEP);

        if (sep != NULL) {
            if (data & (1 << 31)) {
                apple_a13_cpu_reset(APPLE_A13(sep->cpu));
            } else if (data & (1 << 10)) {
                apple_a13_cpu_off(APPLE_A13(sep->cpu));
            } else {
                if (apple_a13_cpu_is_powered_off(APPLE_A13(sep->cpu))) {
                    apple_a13_cpu_start(APPLE_A13(sep->cpu));
                }
            }
        }
        break;
    }
    memcpy(t8030_machine->pmgr_reg + addr, &value, size);
}

static uint64_t pmgr_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    T8030MachineState *t8030_machine = T8030_MACHINE(opaque);
    uint64_t result = 0;
    switch (addr) {
    case 0xF0010: // AppleT8030PMGR::commonSramCheck
        result = 0x5000;
        break;
#if 0
    case 0x80C00: // SEP Power State, Manual & Actual: Run Max
        result = 0xFF;
        break;
#endif
#if 0
    case 0xBC008:
        result = 0xFFFFFFFF;
        break;
    case 0xBC00C:
        result = 0xFFFFFFFF;
        break;
#endif
    default:
        memcpy(&result, t8030_machine->pmgr_reg + addr, size);
        break;
    }
#if 0
    qemu_log_mask(LOG_UNIMP,
                  "PMGR reg READ @ 0x" TARGET_FMT_lx " value: 0x" TARGET_FMT_lx
                  "\n",
                  addr, result);
#endif
    return result;
}

static const MemoryRegionOps pmgr_reg_ops = {
    .write = pmgr_reg_write,
    .read = pmgr_reg_read,
};

static void amcc_reg_write(void *opaque, hwaddr addr, uint64_t data,
                           unsigned size)
{
    T8030MachineState *t8030_machine = T8030_MACHINE(opaque);
    uint32_t value = data;

    memcpy(t8030_machine->amcc_reg + addr, &value, size);
}

static uint64_t amcc_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    T8030MachineState *t8030_machine = T8030_MACHINE(opaque);
    uint64_t result = 0;
    uint64_t base =
        t8030_machine->bootinfo.top_of_kernel_data_pa - T8030_DRAM_BASE;
    uint64_t amcc_size = 0xf000000;
    switch (addr) {
    case 0x6A0:
    case 0x406A0:
    case 0x806A0:
    case 0xC06A0:
        result = base >> 12;
        break;
    case 0x6A4:
    case 0x406A4:
    case 0x806A4:
    case 0xC06A4:
        result = ((amcc_size + base) - 1) >> 12;
        break;
    case 0x6A8:
    case 0x406A8:
    case 0x806A8:
    case 0xC06A8:
        result = 0x1;
        break;
    case 0x6B8:
    case 0x406B8:
    case 0x806B8:
    case 0xC06B8:
        result = 0x1;
        break;
    case 0x4:
    case 0x40004:
    case 0x80004:
    case 0xc0004:
        result = 0x2f;
        break;
    default: {
        memcpy(&result, t8030_machine->amcc_reg + addr, size);
        break;
    }
    }
#if 0
    qemu_log_mask(LOG_UNIMP,
                  "AMCC reg READ @ 0x" TARGET_FMT_lx " value: 0x" TARGET_FMT_lx
                  "\n",
                  orig_addr, result);
#endif
    return result;
}

static const MemoryRegionOps amcc_reg_ops = {
    .write = amcc_reg_write,
    .read = amcc_reg_read,
};

static void t8030_cluster_setup(T8030MachineState *t8030_machine)
{
    for (int i = 0; i < A13_MAX_CLUSTER; i++) {
        char *name = NULL;

        name = g_strdup_printf("cluster%d", i);
        object_initialize_child(OBJECT(t8030_machine), name,
                                &t8030_machine->clusters[i],
                                TYPE_APPLE_A13_CLUSTER);
        g_free(name);
        qdev_prop_set_uint32(DEVICE(&t8030_machine->clusters[i]), "cluster-id",
                             i);
    }
}

static void t8030_cluster_realize(T8030MachineState *t8030_machine)
{
    for (int i = 0; i < A13_MAX_CLUSTER; i++) {
        qdev_realize(DEVICE(&t8030_machine->clusters[i]), NULL, &error_fatal);
        if (t8030_machine->clusters[i].base) {
            memory_region_add_subregion(t8030_machine->sysmem,
                                        t8030_machine->clusters[i].base,
                                        &t8030_machine->clusters[i].mr);
        }
    }
}

static void t8030_cpu_setup(T8030MachineState *t8030_machine)
{
    unsigned int i;
    DTBNode *root;
    GList *iter;
    GList *next = NULL;

    t8030_cluster_setup(t8030_machine);

    root = dtb_get_node(t8030_machine->device_tree, "cpus");
    g_assert_nonnull(root);

    for (iter = root->children, i = 0; iter; iter = next, i++) {
        uint32_t cluster_id;
        DTBNode *node;

        next = iter->next;
        node = (DTBNode *)iter->data;
        if (i >= t8030_real_cpu_count(t8030_machine)) {
            dtb_remove_node(root, node);
            continue;
        }

        t8030_machine->cpus[i] = apple_a13_cpu_create(node, NULL, 0, 0, 0, 0);
        cluster_id = t8030_machine->cpus[i]->cluster_id;

        object_property_add_child(OBJECT(&t8030_machine->clusters[cluster_id]),
                                  DEVICE(t8030_machine->cpus[i])->id,
                                  OBJECT(t8030_machine->cpus[i]));
        qdev_realize(DEVICE(t8030_machine->cpus[i]), NULL, &error_fatal);
    }
    t8030_cluster_realize(t8030_machine);
}

static void t8030_create_aic(T8030MachineState *t8030_machine)
{
    unsigned int i;
    hwaddr *reg;
    DTBProp *prop;
    DTBNode *soc = dtb_get_node(t8030_machine->device_tree, "arm-io");
    DTBNode *child;
    DTBNode *timebase;

    g_assert_nonnull(soc);
    child = dtb_get_node(soc, "aic");
    g_assert_nonnull(child);
    timebase = dtb_get_node(soc, "aic-timebase");
    g_assert_nonnull(timebase);

    t8030_machine->aic =
        apple_aic_create(t8030_real_cpu_count(t8030_machine), child, timebase);
    object_property_add_child(OBJECT(t8030_machine), "aic",
                              OBJECT(t8030_machine->aic));
    g_assert_nonnull(t8030_machine->aic);
    sysbus_realize(t8030_machine->aic, &error_fatal);

    prop = dtb_find_prop(child, "reg");
    g_assert_nonnull(prop);

    reg = (hwaddr *)prop->data;

    for (i = 0; i < t8030_real_cpu_count(t8030_machine); i++) {
        memory_region_add_subregion_overlap(
            &t8030_machine->cpus[i]->memory,
            t8030_machine->soc_base_pa + reg[0],
            sysbus_mmio_get_region(t8030_machine->aic, i), 0);
        sysbus_connect_irq(
            t8030_machine->aic, i,
            qdev_get_gpio_in(DEVICE(t8030_machine->cpus[i]), ARM_CPU_IRQ));
    }
}

static void t8030_pmgr_setup(T8030MachineState *t8030_machine)
{
    uint64_t *reg;
    int i;
    char name[32];
    DTBProp *prop;
    DTBNode *child = dtb_get_node(t8030_machine->device_tree, "arm-io");

    g_assert_nonnull(child);
    child = dtb_get_node(child, "pmgr");
    g_assert_nonnull(child);

    prop = dtb_find_prop(child, "reg");
    g_assert_nonnull(prop);

    reg = (uint64_t *)prop->data;

    for (i = 0; i < prop->length / 8; i += 2) {
        MemoryRegion *mem = g_new(MemoryRegion, 1);
        if (i > 0) {
            snprintf(name, 32, "pmgr-unk-reg-%d", i);
            memory_region_init_io(mem, OBJECT(t8030_machine), &pmgr_unk_reg_ops,
                                  (void *)reg[i], name, reg[i + 1]);
        } else {
            memory_region_init_io(mem, OBJECT(t8030_machine), &pmgr_reg_ops,
                                  t8030_machine, "pmgr-reg", reg[i + 1]);
        }
        memory_region_add_subregion(t8030_machine->sysmem,
                                    reg[i] + reg[i + 1] <
                                            t8030_machine->soc_size ?
                                        t8030_machine->soc_base_pa + reg[i] :
                                        reg[i],
                                    mem);
    }

    {
        MemoryRegion *mem = g_new(MemoryRegion, 1);

        snprintf(name, 32, "pmp-reg");
        memory_region_init_io(mem, OBJECT(t8030_machine), &pmgr_unk_reg_ops,
                              (void *)0x3BC00000, name, 0x60000);
        memory_region_add_subregion(t8030_machine->sysmem,
                                    t8030_machine->soc_base_pa + 0x3BC00000,
                                    mem);
    }
    dtb_set_prop(child, "voltage-states5", sizeof(t8030_voltage_states5),
                 t8030_voltage_states5);
    dtb_set_prop(child, "voltage-states9-sram",
                 sizeof(t8030_voltage_states9_sram),
                 t8030_voltage_states9_sram);
    dtb_set_prop(child, "voltage-states0", sizeof(t8030_voltage_states0),
                 t8030_voltage_states0);
    dtb_set_prop(child, "voltage-states9", sizeof(t8030_voltage_states9),
                 t8030_voltage_states9);
    dtb_set_prop(child, "voltage-states2", sizeof(t8030_voltage_states2),
                 t8030_voltage_states2);
    dtb_set_prop(child, "voltage-states1-sram",
                 sizeof(t8030_voltage_states1_sram),
                 t8030_voltage_states1_sram);
    dtb_set_prop(child, "voltage-states10", sizeof(t8030_voltage_states10),
                 t8030_voltage_states10);
    dtb_set_prop(child, "voltage-states11", sizeof(t8030_voltage_states11),
                 t8030_voltage_states11);
    dtb_set_prop(child, "voltage-states8", sizeof(t8030_voltage_states8),
                 t8030_voltage_states8);
    dtb_set_prop(child, "voltage-states5-sram",
                 sizeof(t8030_voltage_states5_sram),
                 t8030_voltage_states5_sram);
    dtb_set_prop(child, "voltage-states1", sizeof(t8030_voltage_states1),
                 t8030_voltage_states1);
    dtb_set_prop(child, "bridge-settings-17", sizeof(t8030_bridge_settings17),
                 t8030_bridge_settings17);
    dtb_set_prop(child, "bridge-settings-15", sizeof(t8030_bridge_settings15),
                 t8030_bridge_settings15);
    dtb_set_prop(child, "bridge-settings-13", sizeof(t8030_bridge_settings13),
                 t8030_bridge_settings13);
    dtb_set_prop(child, "bridge-settings-1", sizeof(t8030_bridge_settings1),
                 t8030_bridge_settings1);
    dtb_set_prop(child, "bridge-settings-5", sizeof(t8030_bridge_settings5),
                 t8030_bridge_settings5);
    dtb_set_prop(child, "bridge-settings-6", sizeof(t8030_bridge_settings6),
                 t8030_bridge_settings6);
    dtb_set_prop(child, "bridge-settings-2", sizeof(t8030_bridge_settings2),
                 t8030_bridge_settings2);
    dtb_set_prop(child, "bridge-settings-16", sizeof(t8030_bridge_settings16),
                 t8030_bridge_settings16);
    dtb_set_prop(child, "bridge-settings-14", sizeof(t8030_bridge_settings14),
                 t8030_bridge_settings14);
    dtb_set_prop(child, "bridge-settings-7", sizeof(t8030_bridge_settings7),
                 t8030_bridge_settings7);
    dtb_set_prop(child, "bridge-settings-12", sizeof(t8030_bridge_settings12),
                 t8030_bridge_settings12);
    dtb_set_prop(child, "bridge-settings-3", sizeof(t8030_bridge_settings3),
                 t8030_bridge_settings3);
    dtb_set_prop(child, "bridge-settings-8", sizeof(t8030_bridge_settings8),
                 t8030_bridge_settings8);
    dtb_set_prop(child, "bridge-settings-4", sizeof(t8030_bridge_settings4),
                 t8030_bridge_settings4);
    dtb_set_prop(child, "bridge-settings-0", sizeof(t8030_bridge_settings0),
                 t8030_bridge_settings0);
}

static void t8030_amcc_setup(T8030MachineState *t8030_machine)
{
    DTBNode *child;

    child = dtb_get_node(t8030_machine->device_tree, "chosen/lock-regs/amcc");
    if (child == NULL) {
        fprintf(stderr, "%s: warning: amcc registers unavailable? iOS 13?\n",
                __func__);
        DTBNode *chosen = dtb_get_node(t8030_machine->device_tree, "chosen");
        DTBNode *lock_regs = dtb_create_node(chosen, "lock-regs");
        child = dtb_create_node(lock_regs, "amcc");
        dtb_create_node(child, "amcc-ctrr-a");
    }
    g_assert_nonnull(child);

    dtb_set_prop_u32(child, "aperture-count", 1);
    dtb_set_prop_u32(child, "aperture-size", 0x100000);
    dtb_set_prop_u32(child, "plane-count", AMCC_PLANE_COUNT);
    dtb_set_prop_u32(child, "plane-stride", AMCC_PLANE_STRIDE);
    dtb_set_prop_hwaddr(child, "aperture-phys-addr", T8030_AMCC_BASE);
    dtb_set_prop_u32(child, "cache-status-reg-offset", 0x1C00);
    dtb_set_prop_u32(child, "cache-status-reg-mask", 0x1F);
    dtb_set_prop_u32(child, "cache-status-reg-value", 0);

    child = dtb_get_node(child, "amcc-ctrr-a");
    g_assert_nonnull(child);

    dtb_set_prop_u32(child, "page-size-shift", 14);
    dtb_set_prop_u32(child, "lower-limit-reg-offset", AMCC_LOWER(0));
    dtb_set_prop_u32(child, "lower-limit-reg-mask", 0xFFFFFFFF);
    dtb_set_prop_u32(child, "upper-limit-reg-offset", AMCC_UPPER(0));
    dtb_set_prop_u32(child, "upper-limit-reg-mask", 0xFFFFFFFF);
    dtb_set_prop_u32(child, "lock-reg-offset", 0x68C);
    dtb_set_prop_u32(child, "lock-reg-mask", 1);
    dtb_set_prop_u32(child, "lock-reg-value", 1);

    memory_region_init_io(&t8030_machine->amcc, OBJECT(t8030_machine),
                          &amcc_reg_ops, t8030_machine, "amcc",
                          T8030_AMCC_SIZE);
    memory_region_add_subregion(t8030_machine->sysmem, T8030_AMCC_BASE,
                                &t8030_machine->amcc);
}

static void t8030_create_dart(T8030MachineState *t8030_machine,
                              const char *name)
{
    AppleDARTState *dart = NULL;
    DTBProp *prop;
    uint64_t *reg;
    uint32_t *ints;
    int i;
    DTBNode *child;

    child = dtb_get_node(t8030_machine->device_tree, "arm-io");
    g_assert_nonnull(child);

    child = dtb_get_node(child, name);
    if (!child)
        return;

    dart = apple_dart_create(child);
    g_assert_nonnull(dart);
    object_property_add_child(OBJECT(t8030_machine), name, OBJECT(dart));

    prop = dtb_find_prop(child, "reg");
    g_assert_nonnull(prop);

    reg = (uint64_t *)prop->data;

    for (i = 0; i < prop->length / 16; i++) {
        sysbus_mmio_map(SYS_BUS_DEVICE(dart), i,
                        t8030_machine->soc_base_pa + reg[i * 2]);
    }

    prop = dtb_find_prop(child, "interrupts");
    g_assert_nonnull(prop);
    ints = (uint32_t *)prop->data;

    for (i = 0; i < prop->length / sizeof(uint32_t); i++) {
        sysbus_connect_irq(
            SYS_BUS_DEVICE(dart), i,
            qdev_get_gpio_in(DEVICE(t8030_machine->aic), ints[i]));
    }

    sysbus_realize_and_unref(SYS_BUS_DEVICE(dart), &error_fatal);
}

static void t8030_create_sart(T8030MachineState *t8030_machine)
{
    uint64_t *reg;
    DTBNode *child;
    DTBProp *prop;
    SysBusDevice *sart;

    child = dtb_get_node(t8030_machine->device_tree, "arm-io/sart-ans");
    g_assert_nonnull(child);

    sart = apple_sart_create(child);
    g_assert_nonnull(sart);
    object_property_add_child(OBJECT(t8030_machine), "sart-ans", OBJECT(sart));

    prop = dtb_find_prop(child, "reg");
    g_assert_nonnull(prop);
    reg = (uint64_t *)prop->data;

    sysbus_mmio_map(sart, 0, t8030_machine->soc_base_pa + reg[0]);
    sysbus_realize_and_unref(sart, &error_fatal);
}

static void t8030_create_ans(T8030MachineState *t8030_machine)
{
    int i;
    uint32_t *ints;
    DTBProp *prop;
    uint64_t *reg;
    SysBusDevice *sart;
    SysBusDevice *ans;
    DTBNode *child = dtb_get_node(t8030_machine->device_tree, "arm-io");
    DTBNode *iop_nub;

    g_assert_nonnull(child);
    child = dtb_get_node(child, "ans");
    g_assert_nonnull(child);
    iop_nub = dtb_get_node(child, "iop-ans-nub");
    g_assert_nonnull(iop_nub);

    dtb_set_prop(iop_nub, "segment-names", 14, "__TEXT;__DATA");

    t8030_create_sart(t8030_machine);
    sart = SYS_BUS_DEVICE(object_property_get_link(OBJECT(t8030_machine),
                                                   "sart-ans", &error_fatal));

    ans = apple_ans_create(child, APPLE_A7IOP_V4,
                           t8030_machine->rtkit_protocol_ver);
    g_assert_nonnull(ans);
    g_assert_nonnull(object_property_add_const_link(
        OBJECT(ans), "dma-mr", OBJECT(sysbus_mmio_get_region(sart, 1))));

    object_property_add_child(OBJECT(t8030_machine), "ans", OBJECT(ans));
    prop = dtb_find_prop(child, "reg");
    g_assert_nonnull(prop);
    reg = (uint64_t *)prop->data;

    for (i = 0; i < 4; i++) {
        sysbus_mmio_map(ans, i, t8030_machine->soc_base_pa + reg[i << 1]);
    }

    prop = dtb_find_prop(child, "interrupts");
    g_assert_nonnull(prop);
    g_assert_cmpuint(prop->length, ==, 20);
    ints = (uint32_t *)prop->data;

    for (i = 0; i < prop->length / sizeof(uint32_t); i++) {
        sysbus_connect_irq(
            ans, i, qdev_get_gpio_in(DEVICE(t8030_machine->aic), ints[i]));
    }

    sysbus_realize_and_unref(ans, &error_fatal);
}

static void t8030_create_gpio(T8030MachineState *t8030_machine,
                              const char *name)
{
    DeviceState *gpio = NULL;
    DTBProp *prop;
    uint64_t *reg;
    uint32_t *ints;
    int i;
    DTBNode *child = dtb_get_node(t8030_machine->device_tree, "arm-io");

    child = dtb_get_node(child, name);
    g_assert_nonnull(child);
    gpio = apple_gpio_create(child);
    g_assert_nonnull(gpio);
    object_property_add_child(OBJECT(t8030_machine), name, OBJECT(gpio));

    prop = dtb_find_prop(child, "reg");
    g_assert_nonnull(prop);
    reg = (uint64_t *)prop->data;
    sysbus_mmio_map(SYS_BUS_DEVICE(gpio), 0,
                    t8030_machine->soc_base_pa + reg[0]);
    prop = dtb_find_prop(child, "interrupts");
    g_assert_nonnull(prop);

    ints = (uint32_t *)prop->data;

    for (i = 0; i < prop->length / sizeof(uint32_t); i++) {
        sysbus_connect_irq(
            SYS_BUS_DEVICE(gpio), i,
            qdev_get_gpio_in(DEVICE(t8030_machine->aic), ints[i]));
    }

    sysbus_realize_and_unref(SYS_BUS_DEVICE(gpio), &error_fatal);
}

static void t8030_create_i2c(T8030MachineState *t8030_machine, const char *name)
{
    SysBusDevice *i2c = NULL;
    DTBProp *prop;
    uint64_t *reg;
    DTBNode *child = dtb_get_node(t8030_machine->device_tree, "arm-io");

    child = dtb_get_node(child, name);
    if (!child)
        return;
    i2c = apple_i2c_create(name);
    g_assert_nonnull(i2c);
    object_property_add_child(OBJECT(t8030_machine), name, OBJECT(i2c));

    prop = dtb_find_prop(child, "reg");
    g_assert_nonnull(prop);
    reg = (uint64_t *)prop->data;
    sysbus_mmio_map(i2c, 0, t8030_machine->soc_base_pa + reg[0]);
    prop = dtb_find_prop(child, "interrupts");
    g_assert_nonnull(prop);

    sysbus_connect_irq(
        i2c, 0,
        qdev_get_gpio_in(DEVICE(t8030_machine->aic), *(uint32_t *)prop->data));

    sysbus_realize_and_unref(i2c, &error_fatal);
}

static void t8030_create_spi(T8030MachineState *t8030_machine, uint32_t port)
{
    SysBusDevice *spi = NULL;
    DeviceState *gpio = NULL;
    DTBProp *prop;
    uint64_t *reg;
    uint32_t *ints;
    DTBNode *child = dtb_get_node(t8030_machine->device_tree, "arm-io");
    Object *sio;
    char name[32] = { 0 };
    hwaddr base;
    uint32_t irq;
    uint32_t cs_pin;

    snprintf(name, sizeof(name), "spi%d", port);
    child = dtb_get_node(child, name);
    g_assert_nonnull(child);

    spi = apple_spi_create(child);
    g_assert_nonnull(spi);
    object_property_add_child(OBJECT(t8030_machine), name, OBJECT(spi));

    sio = object_property_get_link(OBJECT(t8030_machine), "sio", &error_fatal);
    g_assert_nonnull(object_property_add_const_link(OBJECT(spi), "sio", sio));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(spi), &error_fatal);

    prop = dtb_find_prop(child, "reg");
    g_assert_nonnull(prop);
    reg = (uint64_t *)prop->data;
    base = t8030_machine->soc_base_pa + reg[0];
    sysbus_mmio_map(spi, 0, base);

    prop = dtb_find_prop(child, "interrupts");
    g_assert_nonnull(prop);
    ints = (uint32_t *)prop->data;
    irq = ints[0];

    // The second sysbus IRQ is the cs line
    sysbus_connect_irq(SYS_BUS_DEVICE(spi), 0,
                       qdev_get_gpio_in(DEVICE(t8030_machine->aic), irq));

    prop = dtb_find_prop(child, "function-spi_cs0");
    g_assert_nonnull(prop);
    ints = (uint32_t *)prop->data;
    cs_pin = ints[2];
    gpio = DEVICE(
        object_property_get_link(OBJECT(t8030_machine), "gpio", &error_fatal));
    g_assert_nonnull(gpio);
    qdev_connect_gpio_out(gpio, cs_pin,
                          qdev_get_gpio_in_named(DEVICE(spi), SSI_GPIO_CS, 0));
}

static void t8030_create_usb(T8030MachineState *t8030_machine)
{
    DTBNode *child = dtb_get_node(t8030_machine->device_tree, "arm-io");
    DTBNode *drd = dtb_get_node(child, "usb-drd");
    DTBNode *dart_usb = dtb_get_node(child, "dart-usb");
    DTBNode *dart_mapper = dtb_get_node(dart_usb, "mapper-usb-drd");
    DTBNode *dart_dwc2_mapper = dtb_get_node(dart_usb, "mapper-usb-device");
    DTBNode *phy = dtb_get_node(child, "atc-phy");
    DTBProp *prop;
    DeviceState *atc;
    AppleDARTState *dart;
    IOMMUMemoryRegion *iommu = NULL;
    uint32_t *ints;

    dart = APPLE_DART(object_property_get_link(OBJECT(t8030_machine),
                                               "dart-usb", &error_fatal));

    atc = qdev_new(TYPE_APPLE_TYPEC);
    object_property_add_child(OBJECT(t8030_machine), "atc", OBJECT(atc));

    prop = dtb_find_prop(dart_mapper, "reg");
    g_assert_nonnull(prop);
    g_assert_cmpuint(prop->length, ==, 4);
    iommu = apple_dart_instance_iommu_mr(dart, 1, *(uint32_t *)prop->data);
    g_assert_nonnull(iommu);

    g_assert_nonnull(
        object_property_add_const_link(OBJECT(atc), "dma-xhci", OBJECT(iommu)));
    g_assert_nonnull(
        object_property_add_const_link(OBJECT(atc), "dma-drd", OBJECT(iommu)));

    prop = dtb_find_prop(dart_dwc2_mapper, "reg");
    g_assert_nonnull(prop);
    g_assert_cmpuint(prop->length, ==, 4);
    iommu = apple_dart_instance_iommu_mr(dart, 1, *(uint32_t *)prop->data);
    g_assert_nonnull(iommu);

    g_assert_nonnull(
        object_property_add_const_link(OBJECT(atc), "dma-otg", OBJECT(iommu)));

    prop = dtb_find_prop(phy, "reg");
    g_assert_nonnull(prop);
    sysbus_mmio_map(SYS_BUS_DEVICE(atc), 0,
                    t8030_machine->soc_base_pa + ((uint64_t *)prop->data)[0]);

    sysbus_realize_and_unref(SYS_BUS_DEVICE(atc), &error_fatal);

    prop = dtb_find_prop(drd, "interrupts");
    g_assert_nonnull(prop);
    ints = (uint32_t *)prop->data;
    for (int i = 0; i < 4; i++) {
        sysbus_connect_irq(
            SYS_BUS_DEVICE(atc), i,
            qdev_get_gpio_in(DEVICE(t8030_machine->aic), ints[i]));
    }
    sysbus_connect_irq(
        SYS_BUS_DEVICE(atc), 4,
        qdev_get_gpio_in(DEVICE(t8030_machine->aic), T8030_DWC2_IRQ));
}

static void t8030_create_wdt(T8030MachineState *t8030_machine)
{
    int i;
    uint32_t *ints;
    DTBProp *prop;
    uint64_t *reg;
    SysBusDevice *wdt;
    DTBNode *child = dtb_get_node(t8030_machine->device_tree, "arm-io");

    g_assert_nonnull(child);
    child = dtb_get_node(child, "wdt");
    g_assert_nonnull(child);

    wdt = apple_wdt_create(child);
    g_assert_nonnull(wdt);

    object_property_add_child(OBJECT(t8030_machine), "wdt", OBJECT(wdt));
    prop = dtb_find_prop(child, "reg");
    g_assert_nonnull(prop);
    reg = (uint64_t *)prop->data;

    sysbus_mmio_map(wdt, 0, t8030_machine->soc_base_pa + reg[0]);
    sysbus_mmio_map(wdt, 1, t8030_machine->soc_base_pa + reg[2]);

    prop = dtb_find_prop(child, "interrupts");
    g_assert_nonnull(prop);
    ints = (uint32_t *)prop->data;

    for (i = 0; i < prop->length / sizeof(uint32_t); i++) {
        sysbus_connect_irq(
            wdt, i, qdev_get_gpio_in(DEVICE(t8030_machine->aic), ints[i]));
    }

    // TODO: MCC
    dtb_remove_prop_named(child, "function-panic_flush_helper");
    dtb_remove_prop_named(child, "function-panic_halt_helper");

    dtb_set_prop_u32(child, "no-pmu", 1);

    sysbus_realize_and_unref(wdt, &error_fatal);
}

static void t8030_create_aes(T8030MachineState *t8030_machine)
{
    uint32_t *ints;
    DTBProp *prop;
    uint64_t *reg;
    SysBusDevice *aes;
    AppleDARTState *dart;
    DTBNode *child = dtb_get_node(t8030_machine->device_tree, "arm-io");
    IOMMUMemoryRegion *dma_mr = NULL;
    DTBNode *dart_sio = dtb_get_node(child, "dart-sio");
    DTBNode *dart_aes_mapper = dtb_get_node(dart_sio, "mapper-aes");

    g_assert_nonnull(child);
    child = dtb_get_node(child, "aes");
    g_assert_nonnull(child);
    g_assert_nonnull(dart_sio);
    g_assert_nonnull(dart_aes_mapper);

    aes = apple_aes_create(child, t8030_machine->board_id);
    g_assert_nonnull(aes);

    object_property_add_child(OBJECT(t8030_machine), "aes", OBJECT(aes));
    prop = dtb_find_prop(child, "reg");
    g_assert_nonnull(prop);
    reg = (uint64_t *)prop->data;

    sysbus_mmio_map(aes, 0, t8030_machine->soc_base_pa + reg[0]);
    sysbus_mmio_map(aes, 1, t8030_machine->soc_base_pa + reg[2]);

    prop = dtb_find_prop(child, "interrupts");
    g_assert_nonnull(prop);
    g_assert_cmpuint(prop->length, ==, 4);
    ints = (uint32_t *)prop->data;

    sysbus_connect_irq(aes, 0,
                       qdev_get_gpio_in(DEVICE(t8030_machine->aic), *ints));

    dart = APPLE_DART(object_property_get_link(OBJECT(t8030_machine),
                                               "dart-sio", &error_fatal));
    g_assert_nonnull(dart);

    prop = dtb_find_prop(dart_aes_mapper, "reg");

    dma_mr = apple_dart_iommu_mr(dart, *(uint32_t *)prop->data);
    g_assert_nonnull(dma_mr);
    g_assert_nonnull(
        object_property_add_const_link(OBJECT(aes), "dma-mr", OBJECT(dma_mr)));

    sysbus_realize_and_unref(aes, &error_fatal);
}

static void t8030_create_spmi(T8030MachineState *t8030_machine,
                              const char *name)
{
    SysBusDevice *spmi = NULL;
    DTBProp *prop;
    uint64_t *reg;
    uint32_t *ints;
    DTBNode *child = dtb_get_node(t8030_machine->device_tree, "arm-io");

    g_assert_nonnull(child);
    child = dtb_get_node(child, name);
    if (!child)
        return;

    spmi = apple_spmi_create(child);
    g_assert_nonnull(spmi);
    object_property_add_child(OBJECT(t8030_machine), name, OBJECT(spmi));

    prop = dtb_find_prop(child, "reg");
    g_assert_nonnull(prop);

    reg = (uint64_t *)prop->data;

    sysbus_mmio_map(SYS_BUS_DEVICE(spmi), 0,
                    (t8030_machine->soc_base_pa + reg[2]) &
                        ~(APPLE_SPMI_MMIO_SIZE - 1));

    prop = dtb_find_prop(child, "interrupts");
    g_assert_nonnull(prop);
    ints = (uint32_t *)prop->data;
    // XXX: Only the second interrupt's parent is AIC
    sysbus_connect_irq(SYS_BUS_DEVICE(spmi), 0,
                       qdev_get_gpio_in(DEVICE(t8030_machine->aic), ints[1]));

    sysbus_realize_and_unref(SYS_BUS_DEVICE(spmi), &error_fatal);
}

static void t8030_create_pmu(T8030MachineState *t8030_machine,
                             const char *parent, const char *name)
{
    DeviceState *pmu = NULL;
    AppleSPMIState *spmi = NULL;
    DTBProp *prop;
    DTBNode *child = dtb_get_node(t8030_machine->device_tree, "arm-io");
    uint32_t *ints;

    g_assert_nonnull(child);
    child = dtb_get_node(child, parent);
    if (!child)
        return;

    spmi = APPLE_SPMI(
        object_property_get_link(OBJECT(t8030_machine), parent, &error_fatal));
    g_assert_nonnull(spmi);

    child = dtb_get_node(child, name);
    if (!child)
        return;

    pmu = apple_spmi_pmu_create(child);
    g_assert_nonnull(pmu);
    object_property_add_child(OBJECT(t8030_machine), name, OBJECT(pmu));

    prop = dtb_find_prop(child, "interrupts");
    g_assert_nonnull(prop);
    ints = (uint32_t *)prop->data;

    qdev_connect_gpio_out(pmu, 0, qdev_get_gpio_in(DEVICE(spmi), ints[0]));
    spmi_slave_realize_and_unref(SPMI_SLAVE(pmu), spmi->bus, &error_fatal);

    qemu_register_wakeup_support();
}

static void t8030_create_smc(T8030MachineState *t8030_machine)
{
    int i;
    uint32_t *ints;
    DTBProp *prop;
    uint64_t *reg;
    SysBusDevice *smc;
    DTBNode *child = dtb_get_node(t8030_machine->device_tree, "arm-io");
    DTBNode *iop_nub;
    AppleIopSegmentRange segranges[2] = { 0 };
    hwaddr smc_region_base;
    uint64_t smc_region_size;

    g_assert_nonnull(child);
    child = dtb_get_node(child, "smc");
    g_assert_nonnull(child);
    iop_nub = dtb_get_node(child, "iop-smc-nub");
    g_assert_nonnull(iop_nub);

    dtb_set_prop(iop_nub, "segment-names", 14, "__TEXT;__DATA");

    prop = dtb_find_prop(iop_nub, "region-base");
    g_assert_nonnull(prop);
    smc_region_base = *(uint64_t *)prop->data;
    prop = dtb_find_prop(iop_nub, "region-size");
    g_assert_nonnull(prop);
    smc_region_size = *(uint64_t *)prop->data;

    allocate_ram(t8030_machine->sysmem, "SMC_REGION0", smc_region_base,
                 smc_region_size, 0);

    segranges[0].phys = smc_region_base + T8030_SMC_SRAM_SIZE;
    segranges[0].virt = 0x0;
    segranges[0].remap = smc_region_base;
    segranges[0].size = (smc_region_size - T8030_SMC_SRAM_SIZE) / 2;
    segranges[0].flag = 0x1;

    segranges[1].phys = segranges[0].phys + segranges[0].size;
    segranges[1].virt = segranges[0].size;
    segranges[1].remap = segranges[1].phys;
    segranges[1].size =
        smc_region_size - segranges[0].size - T8030_SMC_SRAM_SIZE;
    segranges[1].flag = 0x0;

    dtb_set_prop(iop_nub, "segment-ranges", sizeof(segranges), segranges);

    // used as a helper for apple_smc_create
    dtb_set_prop_hwaddr(iop_nub, "sram-addr", smc_region_base);

    smc = apple_smc_create(child, APPLE_A7IOP_V4,
                           t8030_machine->rtkit_protocol_ver);
    g_assert_nonnull(smc);

    object_property_add_child(OBJECT(t8030_machine), "smc", OBJECT(smc));
    prop = dtb_find_prop(child, "reg");
    g_assert_nonnull(prop);
    reg = (uint64_t *)prop->data;

    for (i = 0; i < prop->length / 16; i++) {
        sysbus_mmio_map(smc, i, t8030_machine->soc_base_pa + reg[i * 2]);
    }

    sysbus_mmio_map(smc, 2, smc_region_base);

    prop = dtb_find_prop(child, "interrupts");
    g_assert_nonnull(prop);
    ints = (uint32_t *)prop->data;

    for (i = 0; i < prop->length / sizeof(uint32_t); i++) {
        sysbus_connect_irq(
            smc, i, qdev_get_gpio_in(DEVICE(t8030_machine->aic), ints[i]));
    }

    sysbus_realize_and_unref(smc, &error_fatal);
}

static void t8030_create_sio(T8030MachineState *t8030_machine)
{
    int i;
    uint32_t *ints;
    DTBProp *prop;
    uint64_t *reg;
    SysBusDevice *sio;
    AppleDARTState *dart;
    DTBNode *child = dtb_get_node(t8030_machine->device_tree, "arm-io");
    DTBNode *iop_nub;
    IOMMUMemoryRegion *dma_mr = NULL;
    DTBNode *dart_sio = dtb_get_node(child, "dart-sio");
    DTBNode *dart_sio_mapper = dtb_get_node(dart_sio, "mapper-sio");

    g_assert_nonnull(child);
    child = dtb_get_node(child, "sio");
    g_assert_nonnull(child);
    iop_nub = dtb_get_node(child, "iop-sio-nub");
    g_assert_nonnull(iop_nub);

    dtb_set_prop(child, "segment-names", 14, "__TEXT;__DATA");
    dtb_set_prop(iop_nub, "segment-names", 14, "__TEXT;__DATA");

    sio = apple_sio_create(child, APPLE_A7IOP_V4,
                           t8030_machine->rtkit_protocol_ver);
    g_assert_nonnull(sio);

    object_property_add_child(OBJECT(t8030_machine), "sio", OBJECT(sio));
    prop = dtb_find_prop(child, "reg");
    g_assert_nonnull(prop);
    reg = (uint64_t *)prop->data;

    for (i = 0; i < 2; i++) {
        sysbus_mmio_map(sio, i, t8030_machine->soc_base_pa + reg[i * 2]);
    }

    prop = dtb_find_prop(child, "interrupts");
    g_assert_nonnull(prop);
    ints = (uint32_t *)prop->data;

    for (i = 0; i < prop->length / sizeof(uint32_t); i++) {
        sysbus_connect_irq(
            sio, i, qdev_get_gpio_in(DEVICE(t8030_machine->aic), ints[i]));
    }

    dart = APPLE_DART(object_property_get_link(OBJECT(t8030_machine),
                                               "dart-sio", &error_fatal));
    g_assert_nonnull(dart);

    prop = dtb_find_prop(dart_sio_mapper, "reg");

    dma_mr = apple_dart_iommu_mr(dart, *(uint32_t *)prop->data);
    g_assert_nonnull(dma_mr);
    g_assert_nonnull(
        object_property_add_const_link(OBJECT(sio), "dma-mr", OBJECT(dma_mr)));

    sysbus_realize_and_unref(sio, &error_fatal);
}

static void t8030_create_roswell(T8030MachineState *t8030_machine)
{
    DTBNode *child;
    DTBProp *prop;
    AppleI2CState *i2c;

    child = dtb_get_node(t8030_machine->device_tree, "arm-io/i2c3/roswell");
    g_assert_nonnull(child);

    prop = dtb_find_prop(child, "reg");
    g_assert_nonnull(prop);
    i2c = APPLE_I2C(
        object_property_get_link(OBJECT(t8030_machine), "i2c3", &error_fatal));
    i2c_slave_create_simple(i2c->bus, TYPE_APPLE_ROSWELL,
                            *(uint32_t *)prop->data);
}

static void t8030_create_backlight(T8030MachineState *t8030_machine)
{
    DTBNode *child;
    DTBProp *prop;
    AppleI2CState *i2c;

    child = dtb_get_node(t8030_machine->device_tree, "arm-io/i2c0/lm3539");
    g_assert_nonnull(child);

    prop = dtb_find_prop(child, "reg");
    g_assert_nonnull(prop);
    i2c = APPLE_I2C(
        object_property_get_link(OBJECT(t8030_machine), "i2c0", &error_fatal));
    i2c_slave_create_simple(i2c->bus, TYPE_APPLE_LM_BACKLIGHT,
                            *(uint32_t *)prop->data);

    child = dtb_get_node(t8030_machine->device_tree, "arm-io/i2c2/lm3539-1");
    g_assert_nonnull(child);

    prop = dtb_find_prop(child, "reg");
    g_assert_nonnull(prop);
    i2c = APPLE_I2C(
        object_property_get_link(OBJECT(t8030_machine), "i2c2", &error_fatal));
    i2c_slave_create_simple(i2c->bus, TYPE_APPLE_LM_BACKLIGHT,
                            *(uint32_t *)prop->data);
}

static void t8030_create_misc(T8030MachineState *t8030_machine)
{
    DTBNode *child;
    DTBNode *armio;

    armio = dtb_get_node(t8030_machine->device_tree, "arm-io");
    g_assert_nonnull(armio);

    child = dtb_get_node(armio, "bluetooth");
    g_assert_nonnull(child);

    // 0x0 = USB
    // 0x1 = HS
    // 0x2 = H4DS
    // 0x3 = H4BC (UART?)
    // 0x4 = H5
    // 0x5 = BCSP Transport not supported, fallback USB
    // 0x6 = APPLEBT
    // 0x7 = PCIE, the original value
    dtb_set_prop_u32(child, "transport-encoding", 0);
    // setting 0 results in defaulting to 2400000
    dtb_set_prop_u32(child, "transport-speed", 2400000);

    child = dtb_get_node(armio, "wlan");
    g_assert_nonnull(child);
}

static void t8030_create_display(T8030MachineState *t8030_machine)
{
    MachineState *machine;
    AppleDisplayPipeV4State *s;
    SysBusDevice *sbd;
    DTBNode *child;
    uint64_t *reg;
    DTBProp *prop;

    machine = MACHINE(t8030_machine);

    child = dtb_get_node(t8030_machine->device_tree, "arm-io/disp0");

    s = adp_v4_create(child);
    sbd = SYS_BUS_DEVICE(s);

    t8030_machine->video_args.row_bytes = s->width * 4;
    t8030_machine->video_args.width = s->width;
    t8030_machine->video_args.height = s->height;
    t8030_machine->video_args.depth = 32 | ((2 - 1) << 16);
    t8030_machine->video_args.display =
        !xnu_contains_boot_arg(machine->kernel_cmdline, "-s", false) &&
        !xnu_contains_boot_arg(machine->kernel_cmdline, "-v", false);

    prop = dtb_find_prop(child, "reg");
    g_assert_nonnull(prop);
    reg = (uint64_t *)prop->data;

    sysbus_mmio_map(sbd, 0, t8030_machine->soc_base_pa + reg[0]);

    prop = dtb_find_prop(child, "interrupts");
    g_assert_nonnull(prop);
    uint32_t *ints = (uint32_t *)prop->data;

    for (size_t i = 0; i < prop->length / sizeof(uint32_t); i++) {
        sysbus_connect_irq(
            sbd, i, qdev_get_gpio_in(DEVICE(t8030_machine->aic), ints[i]));
    }

    AppleDARTState *dart = APPLE_DART(object_property_get_link(
        OBJECT(t8030_machine), "dart-disp0", &error_fatal));
    g_assert_nonnull(dart);
    child = dtb_get_node(t8030_machine->device_tree,
                         "arm-io/dart-disp0/mapper-disp0");
    g_assert_nonnull(child);
    prop = dtb_find_prop(child, "reg");
    g_assert_nonnull(prop);
    s->dma_mr =
        MEMORY_REGION(apple_dart_iommu_mr(dart, *(uint32_t *)prop->data));
    g_assert_nonnull(s->dma_mr);
    g_assert_nonnull(object_property_add_const_link(OBJECT(sbd), "dma_mr",
                                                    OBJECT(s->dma_mr)));
    address_space_init(&s->dma_as, s->dma_mr, "disp0.dma");

    memory_region_init_ram(&s->vram, OBJECT(sbd), "vram", T8030_DISPLAY_SIZE,
                           &error_fatal);
    object_property_add_const_link(OBJECT(sbd), "vram", OBJECT(&s->vram));
    object_property_add_child(OBJECT(t8030_machine), "disp0", OBJECT(sbd));

    sysbus_realize_and_unref(sbd, &error_fatal);
}

static void t8030_create_sep(T8030MachineState *t8030_machine)
{
    DTBNode *armio;
    DTBNode *child;
    AppleSEPState *sep;
    DTBProp *prop;
    uint64_t *reg;
    uint32_t *ints;
    AppleDARTState *dart;

    prop = dtb_find_prop(dtb_get_node(t8030_machine->device_tree, "chosen"),
                         "chip-id");
    g_assert_nonnull(prop);
    ////uint32_t chip_id = *(uint32_t *)prop->value;
    uint32_t chip_id = 0x8030; // needed because of the AGX workaround

    armio = dtb_get_node(t8030_machine->device_tree, "arm-io");
    g_assert_nonnull(armio);

    dart = APPLE_DART(object_property_get_link(OBJECT(t8030_machine),
                                               "dart-sep", &error_fatal));

    child = dtb_get_node(armio, "dart-sep/mapper-sep");
    g_assert_nonnull(child);
    prop = dtb_find_prop(child, "reg");
    g_assert_nonnull(prop);

    child = dtb_get_node(armio, "sep");
    g_assert_nonnull(child);

    sep = apple_sep_create(
        child,
        MEMORY_REGION(apple_dart_iommu_mr(dart, *(uint32_t *)prop->data)),
        T8030_SEPROM_BASE, A13_MAX_CPU + 1, t8030_machine->build_version, true,
        chip_id);
    g_assert_nonnull(sep);

    object_property_add_child(OBJECT(t8030_machine), "sep", OBJECT(sep));

    prop = dtb_find_prop(child, "reg");
    g_assert_nonnull(prop);
    reg = (uint64_t *)prop->data;
    // AKF_MBOX reg is handled here, using the device tree.
    // XPRT_{PMSC,FUSE,MISC} regs are not handled in this function.
    sysbus_mmio_map(SYS_BUS_DEVICE(sep), 0,
                    t8030_machine->soc_base_pa + reg[0]);
    // PMGR_BASE T8020/T8030
    sysbus_mmio_map(SYS_BUS_DEVICE(sep), 1,
                    t8030_machine->soc_base_pa + 0x41000000);
    sysbus_mmio_map(SYS_BUS_DEVICE(sep), 2,
                    t8030_machine->soc_base_pa + 0x41180000); // TRNG_REGS T8030
    sysbus_mmio_map(SYS_BUS_DEVICE(sep), 3,
                    t8030_machine->soc_base_pa + 0x411c0000); // KEY_BASE T8030
    sysbus_mmio_map(SYS_BUS_DEVICE(sep), 4,
                    t8030_machine->soc_base_pa + 0x41440000); // KEY_FCFG T8030
    sysbus_mmio_map(SYS_BUS_DEVICE(sep), 5,
                    t8030_machine->soc_base_pa + 0x413c0000); // MONI_BASE T8030
    sysbus_mmio_map(SYS_BUS_DEVICE(sep), 6,
                    t8030_machine->soc_base_pa + 0x41400000); // MONI_THRM T8030
    // EISP_BASE T8020/T8030
    sysbus_mmio_map(SYS_BUS_DEVICE(sep), 7,
                    t8030_machine->soc_base_pa + 0x40800000);
    sysbus_mmio_map(SYS_BUS_DEVICE(sep), 8,
                    t8030_machine->soc_base_pa + 0x40aa0000); // EISP_HMAC T8030
    // AESS_BASE T8020/T8030
    sysbus_mmio_map(SYS_BUS_DEVICE(sep), 9,
                    t8030_machine->soc_base_pa + 0x41040000);
    sysbus_mmio_map(SYS_BUS_DEVICE(sep), 10,
                    t8030_machine->soc_base_pa + 0x41080000); // AESH_BASE T8030
    sysbus_mmio_map(SYS_BUS_DEVICE(sep), 11,
                    t8030_machine->soc_base_pa + 0x41100000); // PKA_BASE T8030
    sysbus_mmio_map(SYS_BUS_DEVICE(sep), 12,
                    t8030_machine->soc_base_pa + 0x41504000); // PKA_TMM T8030
    sysbus_mmio_map(SYS_BUS_DEVICE(sep), 13,
                    t8030_machine->soc_base_pa + 0x41080000); // MISC0 T80[23]0
    sysbus_mmio_map(SYS_BUS_DEVICE(sep), 14,
                    t8030_machine->soc_base_pa + 0x410C4000); // MISC2 T80[23]0
    sysbus_mmio_map(SYS_BUS_DEVICE(sep), 15,
                    t8030_machine->soc_base_pa +
                        0x41280000); // encrypted progress counter T8030
    sysbus_mmio_map(SYS_BUS_DEVICE(sep), 16,
                    t8030_machine->soc_base_pa +
                        0x41500000); // boot monitor T8030

    prop = dtb_find_prop(child, "interrupts");
    g_assert_nonnull(prop);
    ints = (uint32_t *)prop->data;

    for (int i = 0; i < prop->length / sizeof(uint32_t); i++) {
        sysbus_connect_irq(
            SYS_BUS_DEVICE(sep), i,
            qdev_get_gpio_in(DEVICE(t8030_machine->aic), ints[i]));
    }

    sysbus_realize_and_unref(SYS_BUS_DEVICE(sep), &error_fatal);
}

static void t8030_create_sep_sim(T8030MachineState *t8030_machine)
{
    DTBNode *armio;
    DTBNode *child;
    AppleSEPSimState *sep;
    DTBProp *prop;
    uint64_t *reg;
    uint32_t *ints;
    AppleDARTState *dart;

    armio = dtb_get_node(t8030_machine->device_tree, "arm-io");
    g_assert_nonnull(armio);
    child = dtb_get_node(armio, "sep");
    g_assert_nonnull(child);

    sep = apple_sep_sim_create(child, true);
    g_assert_nonnull(sep);

    object_property_add_child(OBJECT(t8030_machine), "sep", OBJECT(sep));

    prop = dtb_find_prop(child, "reg");
    g_assert_nonnull(prop);
    reg = (uint64_t *)prop->data;
    sysbus_mmio_map(SYS_BUS_DEVICE(sep), 0,
                    t8030_machine->soc_base_pa + reg[0]);

    prop = dtb_find_prop(child, "interrupts");
    g_assert_nonnull(prop);
    ints = (uint32_t *)prop->data;

    for (int i = 0; i < prop->length / sizeof(uint32_t); i++) {
        sysbus_connect_irq(
            SYS_BUS_DEVICE(sep), i,
            qdev_get_gpio_in(DEVICE(t8030_machine->aic), ints[i]));
    }

    dart = APPLE_DART(object_property_get_link(OBJECT(t8030_machine),
                                               "dart-sep", &error_fatal));
    g_assert_nonnull(dart);
    child = dtb_get_node(armio, "dart-sep");
    g_assert_nonnull(child);
    child = dtb_get_node(child, "mapper-sep");
    g_assert_nonnull(child);
    prop = dtb_find_prop(child, "reg");
    g_assert_nonnull(prop);
    sep->dma_mr =
        MEMORY_REGION(apple_dart_iommu_mr(dart, *(uint32_t *)prop->data));
    g_assert_nonnull(sep->dma_mr);
    g_assert_nonnull(object_property_add_const_link(OBJECT(sep), "dma-mr",
                                                    OBJECT(sep->dma_mr)));
    sep->dma_as = g_new0(AddressSpace, 1);
    address_space_init(sep->dma_as, sep->dma_mr, "sep.dma");

    sysbus_realize_and_unref(SYS_BUS_DEVICE(sep), &error_fatal);
}

static void t8030_create_mt_spi(T8030MachineState *t8030_machine)
{
    AppleSPIState *spi;
    DeviceState *device;
    DeviceState *aop_gpio;
    DTBNode *child;
    DTBProp *prop;
    uint32_t *ints;

    spi = APPLE_SPI(
        object_property_get_link(OBJECT(t8030_machine), "spi1", &error_fatal));
    device = ssi_create_peripheral(apple_spi_get_bus(spi), TYPE_APPLE_MT_SPI);

    aop_gpio = DEVICE(object_property_get_link(OBJECT(t8030_machine),
                                               "aop-gpio", &error_fatal));

    child = dtb_get_node(t8030_machine->device_tree, "arm-io/spi1/multi-touch");
    g_assert_nonnull(child);

    dtb_set_prop(child, "multi-touch-calibration", sizeof(t8030_mt_cal_data),
                 t8030_mt_cal_data);

    dtb_set_prop_null(child, "function-power_ana");

    prop = dtb_find_prop(child, "interrupts");
    g_assert_nonnull(prop);
    ints = (uint32_t *)prop->data;

    qdev_connect_gpio_out_named(device, APPLE_MT_SPI_IRQ, 0,
                                qdev_get_gpio_in(aop_gpio, ints[0]));
}

static void t8030_cpu_reset_work(CPUState *cpu, run_on_cpu_data data)
{
    T8030MachineState *t8030_machine;

    t8030_machine = data.host_ptr;
    cpu_reset(cpu);
    ARM_CPU(cpu)->env.xregs[0] = t8030_machine->bootinfo.kern_boot_args_addr;
    cpu_set_pc(cpu, t8030_machine->bootinfo.kern_entry);
}

static void t8030_cpu_reset(T8030MachineState *t8030_machine)
{
    CPUState *cpu;
    AppleA13State *tcpu;
    uint64_t m_lo;
    uint64_t m_hi;

    qemu_guest_getrandom(&m_lo, sizeof(m_lo), NULL);
    qemu_guest_getrandom(&m_hi, sizeof(m_hi), NULL);

    CPU_FOREACH (cpu) {
        tcpu = APPLE_A13(cpu);
        if (tcpu->cpu_id == A13_MAX_CPU + 1) {
            continue;
        }
        object_property_set_uint(OBJECT(cpu), "rvbar",
                                 t8030_machine->bootinfo.kern_entry & ~0xFFF,
                                 &error_abort);
        object_property_set_uint(OBJECT(cpu), "pauth-mlo", m_lo, &error_abort);
        object_property_set_uint(OBJECT(cpu), "pauth-mhi", m_hi, &error_abort);
        if (tcpu->cpu_id == 0) {
            async_run_on_cpu(cpu, t8030_cpu_reset_work,
                             RUN_ON_CPU_HOST_PTR(t8030_machine));
        } else {
            apple_a13_cpu_reset(tcpu);
        }
    }
}

static void t8030_machine_reset(MachineState *machine, ResetType type)
{
    T8030MachineState *t8030_machine = T8030_MACHINE(machine);
    DeviceState *gpio;

    qemu_devices_reset(type);

    memset(&t8030_machine->pmgr_reg, 0, sizeof(t8030_machine->pmgr_reg));
    if (!runstate_check(RUN_STATE_RESTORE_VM) &&
        !runstate_check(RUN_STATE_PRELAUNCH)) {
        t8030_memory_setup(t8030_machine);

        pmgr_unk_e4800 = 0;
        // maybe also reset pmgr_unk_e4000 array
        // Ah, what the heck. Let's do it.
        memset(pmgr_unk_e4000, 0, sizeof(pmgr_unk_e4000));
    }
    t8030_cpu_reset(t8030_machine);
    gpio =
        DEVICE(object_property_get_link(OBJECT(machine), "gpio", &error_fatal));

    qemu_set_irq(qdev_get_gpio_in(gpio, T8030_GPIO_FORCE_DFU),
                 t8030_machine->force_dfu);
}

static void t8030_machine_init_done(Notifier *notifier, void *data)
{
    T8030MachineState *t8030_machine =
        container_of(notifier, T8030MachineState, init_done_notifier);
    t8030_memory_setup(t8030_machine);
    t8030_cpu_reset(t8030_machine);
}

static void t8030_machine_init(MachineState *machine)
{
    T8030MachineState *t8030_machine;
    MachoHeader64 *hdr;
    uint64_t kernel_low, kernel_high;
    uint32_t build_version;
    uint8_t buffer[0x40];
    DTBNode *child;
    DTBProp *prop;
    hwaddr *ranges;

    memset(buffer, 0, sizeof(buffer));

    t8030_machine = T8030_MACHINE(machine);

    if (!t8030_machine->sep_fw_filename != !t8030_machine->seprom_filename) {
        error_setg(&error_abort,
                   "You need to specify both the SEPROM and the decrypted "
                   "SEPFW in order to use SEP emulation!");
        return;
    }

    t8030_machine->sysmem = get_system_memory();
    allocate_ram(t8030_machine->sysmem, "SROM", T8030_SROM_BASE,
                 T8030_SROM_SIZE, 0);
    allocate_ram(t8030_machine->sysmem, "SRAM", T8030_SRAM_BASE,
                 T8030_SRAM_SIZE, 0);
    allocate_ram(t8030_machine->sysmem, "DRAM", T8030_DRAM_BASE,
                 machine->maxram_size, 0);
    if (t8030_machine->seprom_filename) {
        allocate_ram(t8030_machine->sysmem, "SEPROM", T8030_SEPROM_BASE,
                     T8030_SEPROM_SIZE, 0);
        allocate_ram(t8030_machine->sysmem, "DRAM_30", 0x300000000ULL,
                     0x8000000ULL, 0); // 0x4000000 is too low
        allocate_ram(t8030_machine->sysmem, "DRAM_34", 0x340000000ULL,
                     0x2000000ULL, 0); // 0x1000000 is too low
    }
    if (t8030_machine->sep_fw_filename) {
        allocate_ram(t8030_machine->sysmem, "SEPFW_", 0x0, SEP_DMA_MAPPING_SIZE,
                     0);
    }

    hdr = macho_load_file(machine->kernel_filename, NULL);
    g_assert_nonnull(hdr);
    t8030_machine->kernel = hdr;
    build_version = macho_build_version(hdr);
    info_report("Loading %s %u.%u...", macho_platform_string(hdr),
                BUILD_VERSION_MAJOR(build_version),
                BUILD_VERSION_MINOR(build_version));
    t8030_machine->build_version = build_version;

    switch (BUILD_VERSION_MAJOR(build_version)) {
    case 13:
        t8030_machine->rtkit_protocol_ver = 10;
        break;
    case 14:
        t8030_machine->rtkit_protocol_ver = 11;
        break;
    case 15:
    case 16:
    case 17:
    case 18:
        t8030_machine->rtkit_protocol_ver = 12;
        break;
    default:
        break;
    }

    macho_highest_lowest(hdr, &kernel_low, &kernel_high);
    info_report("Kernel virtual low: 0x" TARGET_FMT_lx, kernel_low);
    info_report("Kernel virtual high: 0x" TARGET_FMT_lx, kernel_high);

    g_virt_base = kernel_low;
    g_phys_base = (hwaddr)macho_get_buffer(hdr);

    t8030_patch_kernel(hdr);

    t8030_machine->device_tree = load_dtb_from_file(machine->dtb);
    t8030_machine->trustcache =
        load_trustcache_from_file(t8030_machine->trustcache_filename,
                                  &t8030_machine->bootinfo.trustcache_size);
    dtb_set_prop_u32(t8030_machine->device_tree, "clock-frequency", 24000000);
    child = dtb_get_node(t8030_machine->device_tree, "arm-io");
    g_assert_nonnull(child);

    dtb_set_prop_u32(child, "chip-revision", 0x20);

    dtb_set_prop(child, "clock-frequencies", sizeof(t8030_clock_freq),
                 t8030_clock_freq);

    prop = dtb_find_prop(child, "ranges");
    g_assert_nonnull(prop);

    ranges = (hwaddr *)prop->data;
    t8030_machine->soc_base_pa = ranges[1];
    t8030_machine->soc_size = ranges[2];

    memset(buffer, 0, sizeof(buffer));
    memcpy(buffer, "t8030", 5);
    dtb_set_prop(t8030_machine->device_tree, "platform-name", 32, buffer);
    memset(buffer, 0, sizeof(buffer));
    memcpy(buffer, "MWL72", 5);
    dtb_set_prop(t8030_machine->device_tree, "model-number", 32, buffer);
    memset(buffer, 0, sizeof(buffer));
    memcpy(buffer, "LL/A", 4);
    dtb_set_prop(t8030_machine->device_tree, "region-info", 32, buffer);
    memset(buffer, 0, sizeof(buffer));
    dtb_set_prop(t8030_machine->device_tree, "config-number", 0x40, buffer);
    memset(buffer, 0, sizeof(buffer));
    memcpy(buffer, "C39ZRMDEN72J", 12);
    dtb_set_prop(t8030_machine->device_tree, "serial-number", 32, buffer);
    memset(buffer, 0, sizeof(buffer));
    memcpy(buffer, "C39948108J9N72J1F", 17);
    dtb_set_prop(t8030_machine->device_tree, "mlb-serial-number", 32, buffer);
    memset(buffer, 0, sizeof(buffer));
    memcpy(buffer, "A2111", 5);
    dtb_set_prop(t8030_machine->device_tree, "regulatory-model-number", 32,
                 buffer);

    child = dtb_get_node(t8030_machine->device_tree, "chosen");
    // TODO: Basic AGX emulation, as QuartzCore & co expect graphics
    // acceleration on T8030. It also gives us AGX-compressed data in the
    // Display Pipes, which we don't know how to decompress yet.
    dtb_set_prop_u32(child, "chip-id", 0x8015);
    t8030_machine->board_id = 0x4;
    dtb_set_prop_u32(child, "board-id", t8030_machine->board_id);
    dtb_set_prop_u32(child, "certificate-production-status", 1);
    dtb_set_prop_u32(child, "certificate-security-mode", 1);

    if (t8030_machine->ecid == 0) {
        t8030_machine->ecid = 0x1122334455667788;
    }
    dtb_set_prop_u64(child, "unique-chip-id", t8030_machine->ecid);

    // Update the display parameters
    dtb_set_prop_u32(child, "display-rotation", 0);
    dtb_set_prop_u32(child, "display-scale", 2);

    child = dtb_get_node(t8030_machine->device_tree, "product");
    dtb_set_prop_u64(child, "display-corner-radius", 0x100000027);
    dtb_set_prop_u32(child, "oled-display", 1);
    dtb_set_prop(child, "graphics-featureset-class", 1, "");
    dtb_set_prop(child, "graphics-featureset-fallbacks", 1, "");
    // TODO: PMP
    dtb_set_prop(t8030_machine->device_tree, "target-type", 4, "sim");
    dtb_set_prop_u32(child, "device-color-policy", 0);

    t8030_cpu_setup(t8030_machine);

    t8030_create_aic(t8030_machine);

    for (int i = 0; i < T8030_NUM_UARTS; i++) {
        t8030_create_s3c_uart(t8030_machine, i, serial_hd(i));
    }

    t8030_pmgr_setup(t8030_machine);
    t8030_amcc_setup(t8030_machine);

    t8030_create_ans(t8030_machine);

    t8030_create_gpio(t8030_machine, "gpio");
    t8030_create_gpio(t8030_machine, "smc-gpio");
    t8030_create_gpio(t8030_machine, "nub-gpio");
    t8030_create_gpio(t8030_machine, "aop-gpio");

    t8030_create_i2c(t8030_machine, "i2c0");
    t8030_create_i2c(t8030_machine, "i2c1");
    t8030_create_i2c(t8030_machine, "i2c2");
    t8030_create_i2c(t8030_machine, "i2c3");
    t8030_create_i2c(t8030_machine, "smc-i2c0");
    t8030_create_i2c(t8030_machine, "smc-i2c1");

    t8030_create_dart(t8030_machine, "dart-usb");
    t8030_create_dart(t8030_machine, "dart-sio");
    t8030_create_dart(t8030_machine, "dart-disp0");
    t8030_create_dart(t8030_machine, "dart-sep");
    t8030_create_usb(t8030_machine);

    t8030_create_wdt(t8030_machine);

    t8030_create_aes(t8030_machine);

    t8030_create_spmi(t8030_machine, "spmi0");
    t8030_create_spmi(t8030_machine, "spmi1");
    t8030_create_spmi(t8030_machine, "spmi2");

    t8030_create_pmu(t8030_machine, "spmi0", "spmi-pmu");

    t8030_create_smc(t8030_machine);
    t8030_create_sio(t8030_machine);

    t8030_create_spi(t8030_machine, 1);
    t8030_create_spi(t8030_machine, 3);

    if (t8030_machine->seprom_filename && t8030_machine->sep_fw_filename) {
        t8030_create_sep(t8030_machine);
    } else {
        t8030_create_sep_sim(t8030_machine);
    }

    t8030_create_roswell(t8030_machine);

    t8030_create_backlight(t8030_machine);

    t8030_create_misc(t8030_machine);

    t8030_create_display(t8030_machine);

    t8030_create_mt_spi(t8030_machine);

    t8030_machine->init_done_notifier.notify = t8030_machine_init_done;
    qemu_add_machine_init_done_notifier(&t8030_machine->init_done_notifier);
}

static void t8030_set_trustcache_filename(Object *obj, const char *value,
                                          Error **errp)
{
    T8030MachineState *t8030_machine;

    t8030_machine = T8030_MACHINE(obj);
    g_free(t8030_machine->trustcache_filename);
    t8030_machine->trustcache_filename = g_strdup(value);
}

static char *t8030_get_trustcache_filename(Object *obj, Error **errp)
{
    T8030MachineState *t8030_machine;

    t8030_machine = T8030_MACHINE(obj);
    return g_strdup(t8030_machine->trustcache_filename);
}

static void t8030_set_ticket_filename(Object *obj, const char *value,
                                      Error **errp)
{
    T8030MachineState *t8030_machine;

    t8030_machine = T8030_MACHINE(obj);
    g_free(t8030_machine->ticket_filename);
    t8030_machine->ticket_filename = g_strdup(value);
}

static char *t8030_get_ticket_filename(Object *obj, Error **errp)
{
    T8030MachineState *t8030_machine;

    t8030_machine = T8030_MACHINE(obj);
    return g_strdup(t8030_machine->ticket_filename);
}

static void t8030_set_seprom_filename(Object *obj, const char *value,
                                      Error **errp)
{
    T8030MachineState *t8030_machine;

    t8030_machine = T8030_MACHINE(obj);
    g_free(t8030_machine->seprom_filename);
    t8030_machine->seprom_filename = g_strdup(value);
}

static char *t8030_get_seprom_filename(Object *obj, Error **errp)
{
    T8030MachineState *t8030_machine;

    t8030_machine = T8030_MACHINE(obj);
    return g_strdup(t8030_machine->seprom_filename);
}

static void t8030_set_sepfw_filename(Object *obj, const char *value,
                                     Error **errp)
{
    T8030MachineState *t8030_machine;

    t8030_machine = T8030_MACHINE(obj);
    g_free(t8030_machine->sep_fw_filename);
    t8030_machine->sep_fw_filename = g_strdup(value);
}

static char *t8030_get_sepfw_filename(Object *obj, Error **errp)
{
    T8030MachineState *t8030_machine;

    t8030_machine = T8030_MACHINE(obj);
    return g_strdup(t8030_machine->sep_fw_filename);
}

static void t8030_set_boot_mode(Object *obj, const char *value, Error **errp)
{
    T8030MachineState *t8030_machine;

    t8030_machine = T8030_MACHINE(obj);
    if (g_str_equal(value, "auto")) {
        t8030_machine->boot_mode = kBootModeAuto;
    } else if (g_str_equal(value, "manual")) {
        t8030_machine->boot_mode = kBootModeManual;
    } else if (g_str_equal(value, "enter_recovery")) {
        t8030_machine->boot_mode = kBootModeEnterRecovery;
    } else if (g_str_equal(value, "exit_recovery")) {
        t8030_machine->boot_mode = kBootModeExitRecovery;
    } else {
        t8030_machine->boot_mode = kBootModeAuto;
        error_setg(errp, "Invalid boot mode: %s", value);
    }
}

static char *t8030_get_boot_mode(Object *obj, Error **errp)
{
    T8030MachineState *t8030_machine;

    t8030_machine = T8030_MACHINE(obj);
    switch (t8030_machine->boot_mode) {
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

static void t8030_get_ecid(Object *obj, Visitor *v, const char *name,
                           void *opaque, Error **errp)
{
    T8030MachineState *t8030_machine;
    int64_t value;

    t8030_machine = T8030_MACHINE(obj);
    value = t8030_machine->ecid;
    visit_type_int(v, name, &value, errp);
}

static void t8030_set_ecid(Object *obj, Visitor *v, const char *name,
                           void *opaque, Error **errp)
{
    T8030MachineState *t8030_machine;
    int64_t value;

    t8030_machine = T8030_MACHINE(obj);

    if (!visit_type_int(v, name, &value, errp)) {
        return;
    }

    t8030_machine->ecid = value;
}

static void t8030_set_kaslr_off(Object *obj, bool value, Error **errp)
{
    T8030MachineState *t8030_machine;

    t8030_machine = T8030_MACHINE(obj);
    t8030_machine->kaslr_off = value;
}

static bool t8030_get_kaslr_off(Object *obj, Error **errp)
{
    T8030MachineState *t8030_machine;

    t8030_machine = T8030_MACHINE(obj);
    return t8030_machine->kaslr_off;
}

static ram_addr_t t8030_machine_fixup_ram_size(ram_addr_t size)
{
    return ROUND_UP_16K(size);
}

static void t8030_set_force_dfu(Object *obj, bool value, Error **errp)
{
    T8030MachineState *t8030_machine;

    t8030_machine = T8030_MACHINE(obj);
    t8030_machine->force_dfu = value;
}

static bool t8030_get_force_dfu(Object *obj, Error **errp)
{
    T8030MachineState *t8030_machine;

    t8030_machine = T8030_MACHINE(obj);
    return t8030_machine->force_dfu;
}

static void t8030_machine_class_init(ObjectClass *klass, void *data)
{
    MachineClass *mc = MACHINE_CLASS(klass);

    mc->desc = "T8030";
    mc->init = t8030_machine_init;
    mc->reset = t8030_machine_reset;
    mc->max_cpus = A13_MAX_CPU + 1;
    mc->no_sdcard = 1;
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
    mc->no_parallel = 1;
    mc->default_cpu_type = TYPE_APPLE_A13;
    mc->minimum_page_bits = 14;
    mc->default_ram_size = 4 * GiB;
    mc->fixup_ram_size = t8030_machine_fixup_ram_size;

    object_class_property_add_str(klass, "trustcache",
                                  t8030_get_trustcache_filename,
                                  t8030_set_trustcache_filename);
    object_class_property_set_description(klass, "trustcache",
                                          "Trustcache to be loaded");
    object_class_property_add_str(klass, "ticket", t8030_get_ticket_filename,
                                  t8030_set_ticket_filename);
    object_class_property_set_description(klass, "ticket",
                                          "APTicket to be loaded");
    object_class_property_add_str(klass, "seprom", t8030_get_seprom_filename,
                                  t8030_set_seprom_filename);
    object_class_property_set_description(klass, "seprom",
                                          "SEPROM to be loaded");
    object_class_property_add_str(klass, "sepfw", t8030_get_sepfw_filename,
                                  t8030_set_sepfw_filename);
    object_class_property_set_description(klass, "sepfw", "SEPFW to be loaded");
    object_class_property_add_str(klass, "boot-mode", t8030_get_boot_mode,
                                  t8030_set_boot_mode);
    object_class_property_set_description(klass, "boot-mode",
                                          "Boot mode of the machine");
    object_class_property_add(klass, "ecid", "uint64", t8030_get_ecid,
                              t8030_set_ecid, NULL, NULL);
    object_class_property_set_description(klass, "ecid", "Device ECID");
    object_class_property_add_bool(klass, "kaslr-off", t8030_get_kaslr_off,
                                   t8030_set_kaslr_off);
    object_class_property_set_description(klass, "kaslr-off", "Disable KASLR");
    object_class_property_add_bool(klass, "force-dfu", t8030_get_force_dfu,
                                   t8030_set_force_dfu);
    object_class_property_set_description(klass, "force-dfu", "Force DFU");
}

static const TypeInfo t8030_machine_info = {
    .name = TYPE_T8030_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(T8030MachineState),
    .class_size = sizeof(T8030MachineClass),
    .class_init = t8030_machine_class_init,
};

static void t8030_machine_types(void)
{
    type_register_static(&t8030_machine_info);
}

type_init(t8030_machine_types)
