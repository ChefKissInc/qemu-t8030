/*
 * Copyright (c) 2023-2025 Visual Ehrmanntraut.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 or later, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef HW_ARM_APPLE_SILICON_BOOT_H
#define HW_ARM_APPLE_SILICON_BOOT_H

// #define ENABLE_BASEBAND
#define ENABLE_SEP
#define ENABLE_SEP_SECURITY

#include "qemu/osdep.h"
#include "exec/hwaddr.h"
#include "hw/arm/apple-silicon/dtb.h"

#define BOOT_ARGS_REVISION_2 (2)
#define BOOT_ARGS_VERSION_2 (2)
#define BOOT_ARGS_VERSION_4 (4)
#define BOOT_LINE_LENGTH (608)

#define LC_SYMTAB (0x2)
#define LC_UNIXTHREAD (0x5)
#define LC_DYSYMTAB (0xB)
#define LC_SEGMENT_64 (0x19)
#define LC_SOURCE_VERSION (0x2A)
#define LC_BUILD_VERSION (0x32)
#define LC_REQ_DYLD (0x80000000)
#define LC_DYLD_CHAINED_FIXUPS (0x34 | LC_REQ_DYLD)
#define LC_FILESET_ENTRY (0x35 | LC_REQ_DYLD)

typedef struct {
    uint32_t cmd;
    uint32_t cmd_size;
    uint32_t sym_off;
    uint32_t nsyms;
    uint32_t str_off;
    uint32_t str_size;
} MachoSymtabCommand;

typedef struct {
    uint32_t cmd;
    uint32_t cmd_size;
    uint32_t local_sym_i;
    uint32_t local_sym_n;
    uint32_t ext_def_sym_i;
    uint32_t ext_def_sym_n;
    uint32_t undef_sym_i;
    uint32_t undef_sym_n;
    uint32_t toc_off;
    uint32_t toc_n;
    uint32_t mod_tab_off;
    uint32_t mod_tab_n;
    uint32_t ext_ref_sym_off;
    uint32_t ext_ref_syms_n;
    uint32_t indirect_sym_off;
    uint32_t indirect_syms_n;
    uint32_t ext_rel_off;
    uint32_t ext_rel_n;
    uint32_t loc_rel_off;
    uint32_t loc_rel_n;
} MachoDysymtabCommand;

typedef struct {
    uint32_t cmd;
    uint32_t cmd_size;
    char segname[16];
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
    uint32_t maxprot;
    uint32_t initprot;
    uint32_t nsects;
    uint32_t flags;
} MachoSegmentCommand64;

typedef struct {
    char sect_name[16];
    char seg_name[16];
    uint64_t addr;
    uint64_t size;
    uint32_t offset;
    uint32_t align;
    uint32_t rel_off;
    uint32_t n_reloc;
    uint32_t flags;
    uint32_t reserved1;
    uint32_t reserved2;
    uint32_t reserved3;
} MachoSection64;

#define SECTION_TYPE (0x000000ff)
#define S_NON_LAZY_SYMBOL_POINTERS (0x6)

typedef struct {
    uint32_t cmd;
    uint32_t cmd_size;
    uint64_t vm_addr;
    uint64_t file_off;
    uint32_t entry_id;
    uint32_t reserved;
} MachoFilesetEntryCommand;

typedef struct {
    uint32_t cmd;
    uint32_t cmd_size;
    uint64_t version;
} MachoSourceVersionCommand;

#define PLATFORM_MACOS (1)
#define PLATFORM_IOS (2)
#define PLATFORM_TVOS (3)
#define PLATFORM_WATCHOS (4)
#define PLATFORM_BRIDGEOS (5)

#define BUILD_VERSION_MAJOR(_v) (((_v) & 0xFFFF0000) >> 16)
#define BUILD_VERSION_MINOR(_v) (((_v) & 0x0000FF00) >> 8)

typedef struct {
    uint32_t cmd;
    uint32_t cmd_size;
    uint32_t platform;
    uint32_t min_os;
    uint32_t sdk;
    uint32_t n_tools;
} MachoBuildVersionCommand;

#define MACH_MAGIC_64 (0xFEEDFACFu)
#define MH_EXECUTE (0x2)
#define MH_FILESET (0xC)

typedef struct {
    uint32_t magic;
    uint32_t /*cpu_type_t*/ cpu_type;
    uint32_t /*cpu_subtype_t*/ cpu_subtype;
    uint32_t file_type;
    uint32_t n_cmds;
    uint32_t size_of_cmds;
    uint32_t flags;
    uint32_t reserved;
} MachoHeader64;

typedef struct {
    uint32_t cmd;
    uint32_t cmd_size;
} MachoLoadCommand;

typedef struct {
    union {
        uint32_t n_strx;
    } n_un;
    uint8_t n_type;
    uint8_t n_sect;
    uint16_t n_desc;
    uint64_t n_value;
} MachoNList64;

#define N_STAB (0xE0)
#define N_PEXT (0x10)
#define N_TYPE (0x0E)
#define N_EXT (0x01)

typedef struct {
    uint64_t base_addr;
    uint64_t display;
    uint64_t row_bytes;
    uint64_t width;
    uint64_t height;
    uint64_t depth;
} AppleVideoArgs;

typedef struct {
    uint64_t version;
    uint64_t virt_base;
    uint64_t phys_base;
    uint64_t mem_size;
    uint64_t kern_args;
    uint64_t kern_entry;
    uint64_t kern_phys_base;
    uint64_t kern_phys_slide;
    uint64_t kern_virt_slide;
    uint64_t kern_text_section_off;
    uint8_t random_bytes[0x10];
} AppleMonitorBootArgs;

typedef struct {
    uint16_t revision;
    uint16_t version;
    uint64_t virt_base;
    uint64_t phys_base;
    uint64_t mem_size;
    uint64_t kernel_top;
    AppleVideoArgs video_args;
    uint32_t machine_type;
    uint64_t device_tree_ptr;
    uint32_t device_tree_length;
    char cmdline[BOOT_LINE_LENGTH];
    uint64_t boot_flags;
    uint64_t mem_size_actual;
} AppleKernelBootArgs;

#define EMBEDDED_PANIC_HEADER_FLAG_COREDUMP_COMPLETE (0x01)
#define EMBEDDED_PANIC_HEADER_FLAG_STACKSHOT_SUCCEEDED (0x02)
#define EMBEDDED_PANIC_HEADER_FLAG_STACKSHOT_FAILED_DEBUGGERSYNC (0x04)
#define EMBEDDED_PANIC_HEADER_FLAG_STACKSHOT_FAILED_ERROR (0x08)
#define EMBEDDED_PANIC_HEADER_FLAG_STACKSHOT_FAILED_INCOMPLETE (0x10)
#define EMBEDDED_PANIC_HEADER_FLAG_STACKSHOT_FAILED_NESTED (0x20)
#define EMBEDDED_PANIC_HEADER_FLAG_NESTED_PANIC (0x40)
#define EMBEDDED_PANIC_HEADER_FLAG_BUTTON_RESET_PANIC (0x80)
#define EMBEDDED_PANIC_HEADER_FLAG_COPROC_INITIATED_PANIC (0x100)
#define EMBEDDED_PANIC_HEADER_FLAG_COREDUMP_FAILED (0x200)
#define EMBEDDED_PANIC_HEADER_FLAG_COMPRESS_FAILED (0x400)
#define EMBEDDED_PANIC_HEADER_FLAG_STACKSHOT_DATA_COMPRESSED (0x800)

#define EMBEDDED_PANIC_HEADER_CURRENT_VERSION (2)
#define EMBEDDED_PANIC_MAGIC (0x46554E4B)
#define EMBEDDED_PANIC_HEADER_OSVERSION_LEN (32)

typedef struct {
    uint32_t magic;
    uint32_t crc;
    uint32_t version;
    uint64_t panic_flags;
    uint32_t panic_log_offset;
    uint32_t panic_log_len;
    uint32_t stackshot_offset;
    uint32_t stackshot_len;
    uint32_t other_log_offset;
    uint32_t other_log_len;
    union {
        struct {
            uint64_t x86_power_state : 8;
            uint64_t x86_efi_boot_state : 8;
            uint64_t x86_system_state : 8;
            uint64_t x86_unused_bits : 40;
        };
        uint64_t x86_do_not_use;
    };
    char os_version[EMBEDDED_PANIC_HEADER_OSVERSION_LEN];
    char macos_version[EMBEDDED_PANIC_HEADER_OSVERSION_LEN];
} QEMU_PACKED AppleEmbeddedPanicHeader;

typedef struct {
    uint64_t phys;
    uint64_t virt;
    uint64_t remap;
    uint32_t size;
    uint32_t flag;
} QEMU_PACKED AppleIopSegmentRange;

#define XNU_MAX_NVRAM_SIZE (0xFFFF * 0x10)
#define XNU_BNCH_SIZE (32)

typedef struct {
    hwaddr kern_entry;
    hwaddr kern_text_off;
    hwaddr tz1_entry;
    hwaddr device_tree_addr;
    uint64_t device_tree_size;
    hwaddr ramdisk_addr;
    uint64_t ramdisk_size;
    hwaddr trustcache_addr;
    uint64_t trustcache_size;
    hwaddr sep_fw_addr;
    uint64_t sep_fw_size;
    hwaddr kern_boot_args_addr;
    uint64_t kern_boot_args_size;
    hwaddr top_of_kernel_data_pa;
    hwaddr tz1_boot_args_pa;
    hwaddr dram_base;
    uint64_t dram_size;
    uint8_t nvram_data[XNU_MAX_NVRAM_SIZE];
    uint32_t nvram_size;
    char *ticket_data;
    uint64_t ticket_length;
    uint8_t boot_nonce_hash[XNU_BNCH_SIZE];
} AppleBootInfo;

MachoHeader64 *macho_load_file(const char *filename,
                               MachoHeader64 **secure_monitor);

MachoHeader64 *macho_parse(uint8_t *data, uint32_t len);

uint8_t *macho_get_buffer(MachoHeader64 *hdr);

void macho_free(MachoHeader64 *hdr);

uint32_t macho_build_version(MachoHeader64 *mh);

uint32_t macho_platform(MachoHeader64 *mh);

const char *macho_platform_string(MachoHeader64 *mh);

void macho_highest_lowest(MachoHeader64 *mh, uint64_t *lowaddr,
                          uint64_t *highaddr);

void macho_text_base(MachoHeader64 *mh, uint64_t *text_base);

MachoFilesetEntryCommand *macho_get_fileset(MachoHeader64 *header,
                                            const char *entry);

MachoHeader64 *macho_get_fileset_header(MachoHeader64 *header,
                                        const char *entry);

MachoSegmentCommand64 *macho_get_segment(MachoHeader64 *header,
                                         const char *segname);

MachoSection64 *macho_get_section(MachoSegmentCommand64 *seg, const char *name);

uint64_t xnu_slide_hdr_va(MachoHeader64 *header, uint64_t hdr_va);

void *xnu_va_to_ptr(uint64_t va);

bool xnu_contains_boot_arg(const char *bootArgs, const char *arg,
                           bool prefixmatch);

void apple_monitor_setup_boot_args(
    const char *name, AddressSpace *as, MemoryRegion *mem, hwaddr bootargs_addr,
    hwaddr virt_base, hwaddr phys_base, hwaddr mem_size, hwaddr kern_args,
    hwaddr kern_entry, hwaddr kern_phys_base, hwaddr kern_phys_slide,
    hwaddr kern_virt_slide, hwaddr kern_text_section_off);
void macho_setup_bootargs(const char *name, AddressSpace *as, MemoryRegion *mem,
                          hwaddr bootargs_pa, hwaddr virt_base,
                          hwaddr phys_base, hwaddr mem_size,
                          hwaddr top_of_kernel_data_pa, hwaddr dtb_va,
                          hwaddr dtb_size, AppleVideoArgs v_bootargs,
                          const char *cmdline);

void macho_allocate_segment_records(DTBNode *memory_map, MachoHeader64 *mh);

hwaddr arm_load_macho(MachoHeader64 *mh, AddressSpace *as, MemoryRegion *mem,
                      DTBNode *memory_map, hwaddr phys_base, hwaddr virt_slide);

void macho_load_raw_file(const char *filename, AddressSpace *as,
                         MemoryRegion *mem, const char *name, hwaddr file_pa,
                         uint64_t *size);

DTBNode *load_dtb_from_file(char *filename);

void macho_populate_dtb(DTBNode *root, AppleBootInfo *info);

void macho_load_dtb(DTBNode *root, AddressSpace *as, MemoryRegion *mem,
                    const char *name, AppleBootInfo *info);

uint8_t *load_trustcache_from_file(const char *filename, uint64_t *size);
void macho_load_trustcache(void *trustcache, uint64_t size, AddressSpace *as,
                           MemoryRegion *mem, hwaddr pa);

void macho_load_ramdisk(const char *filename, AddressSpace *as,
                        MemoryRegion *mem, hwaddr pa, uint64_t *size);

#endif /* HW_ARM_APPLE_SILICON_BOOT_H */
