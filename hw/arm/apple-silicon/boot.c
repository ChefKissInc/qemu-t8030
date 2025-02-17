/*
 * General Apple XNU utilities.
 *
 * Copyright (c) 2019 Jonathan Afek <jonyafek@me.com>
 * Copyright (c) 2021 Nguyen Hoang Trung (TrungNguyen1909)
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
#include "crypto/hash.h"
#include "crypto/random.h"
#include "exec/memory.h"
#include "hw/arm/apple-silicon/boot.h"
#include "hw/arm/apple-silicon/dtb.h"
#include "hw/arm/apple-silicon/mem.h"
#include "qapi/error.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/guest-random.h"
#include "img4.h"
#include "lzfse.h"
#include "lzss.h"

static const char *KEEP_COMP[] = {
    "adbe0,s8000\0$",
    "aes,s8000\0$",
    "aic,1\0$",
    "apple,lightning\0ARM,v8\0$",
    "apple,thunder\0ARM,v8\0$",
    "apple,twister\0ARM,v8\0$",
    "arm-io,s8000\0$",
    "arm-io,t8030\0$",
    "atc-phy,t8030\0atc-phy,t8027\0$",
    "backlight\0$",
    "backlight,lm3539\0$",
#ifdef ENABLE_BASEBAND
    "baseband,i19\0$",
#endif
#ifdef ENABLE_SEP_SECURITY
    "biosensor,pearl\0$",
#endif
    "buttons\0$",
    // "dart,s8000\0dart,s5l8960x\0$",
    "dart,t8020\0$",
    "disp0,t8030\0$",
    "gen-mt-decider\0$",
    "gpio,s8000\0gpio,s5l8960x\0$",
    "gpio,t8015\0gpio,s5l8960x\0$",
    "gpio,t8030\0gpio,s5l8960x\0$",
    "i2c,s8000\0i2c,s5l8940x\0iic,soft\0$",
    "i2c,t8030\0i2c,s5l8940x\0iic,soft\0$",
    "iic,soft\0$",
    "iommu-mapper\0$",
    "iop,ascwrap-v2\0$",
    "iop,t8030\0iop,t8015\0$",
    "iop-nub,rtbuddy-v2\0$",
    "iop-nub,sep\0$",
    "N104AP\0iPhone12,1\0AppleARM\0$",
    "N104DEV\0iPhone12,1\0AppleARM\0$",
    "N66AP\0iPhone8,2\0AppleARM\0$",
    "N66DEV\0iPhone8,2\0AppleARM\0$",
    "nvme-mmu,s8000\0$",
    "otgphyctrl,s8000\0otgphyctrl,s5l8960x\0$",
    "pmgr1,s8000\0$",
    "pmgr1,t8030\0$",
    "pmu,d2255\0$", // problematic on S8000 (if AIC configed for T8030?)
    "pmu,spmi\0pmu,avus\0$",
    "roswell\0$",
    "sart,coastguard\0$",
    "sart,t8030\0$",
    "sacm,1\0$",
    "sio-dma-controller\0$",
    "smc-pmu\0$",
    "smc-tempsensor\0$",
    "soc-tuner,s8000\0$",
    "soc-tuner,t8030\0$",
    "spi-1,samsung\0$",
    "spmi,gen0\0$",
    "spmi,t8015\0$",
    "uart-1,samsung\0$",
    "usb-complex,s8000\0usb-complex,s5l8960x\0$",
    "usb-device,s5l8900x\0$",
    "usb-device,s8000\0usb-device,t7000\0usb-device,s5l8900x\0$",
    "usb-device,t7000\0usb-device,s5l8900x\0$",
    "usb-drd,t8030\0usb-drd,t8027\0$",
    "wdt,s8000\0wdt,s5l8960x\0$",
    "wdt,t8030\0wdt,s5l8960x\0$",
};

static const char *REM_NAMES[] = {
    "gfx-asc\0$",
    "smc-aop\0$",
    "amfm\0$",
    "dart-ane\0$",
    "dart-aop\0$",
    "dart-apcie2\0$",
    "dart-apcie3\0$",
    "dart-avd\0$",
    "dart-ave\0$",
    "dart-isp\0$",
    "dart-jpeg0\0$",
    "dart-jpeg1\0$",
    "dart-pmp\0$",
    "dart-rsm\0$",
    "dart-scaler\0$",
    "dockchannel-uart\0$",
    "dotara\0$",
    "pmp\0$",
    "stockholm-spmi\0$",
#ifndef ENABLE_BASEBAND
    "baseband\0$",
    "baseband-spmi\0$",
    "baseband-vol\0$",
    "pci-bridge3\0$",
    "dart-apcie3\0$",
#endif
#ifndef ENABLE_SEP
    "sep\0$",
    "dart-sep\0$",
    "xart-vol\0$",
    "pearl-sep\0$",
    "isp\0$",
    "xART\0$",
#endif
#ifndef ENABLE_SEP_SECURITY // necessary?
    "pearl-sep\0$",
    "isp\0$",
    "Lynx\0$",
#endif
};

static const char *REM_DEV_TYPES[] = {
    "bluetooth\0$",     "wlan\0$",          "aop\0$", "pmp\0$",
#ifndef ENABLE_BASEBAND
    "baseband\0$",      "baseband-spmi\0$",
#endif
#ifndef ENABLE_SEP_SECURITY // necessary?
    "spherecontrol\0$",
#endif
};

static const char *REM_PROPS[] = {
#ifndef ENABLE_SEP_SECURITY
    "content-protect",
    "encryptable",
#endif
    "function-brick_id_voltage",
    "function-dock_parent",
    "function-error_handler",
    "function-ldcm_bypass_en",
    "function-mcc_ctrl",
    "function-pmp_control",
    "function-spi0_mosi_config",
    "function-spi0_sclk_config",
    "function-usb_500_100",
    "function-vbus_voltage",
    "mcc-power-gating",
    "nand-debug",
    "nvme-coastguard",
    "pmp",
    "soc-tuning",
#ifndef ENABLE_BASEBAND
    "baseband-chipset",
    "has-baseband",
    "event_name-gpio9",
    "gps-capable",
    "location-reminders",
    "personal-hotspot",
    "bitrate-3g",
    "bitrate-lte",
    "bitrate-2g",
    "navigation",
#endif
#ifndef ENABLE_SEP_SECURITY // necessary?
    "pearl-camera",
    "face-detection-support",
#endif
};

static void allocate_and_copy(MemoryRegion *mem, AddressSpace *as,
                              const char *name, hwaddr pa, hwaddr size,
                              void *buf)
{
    address_space_rw(as, pa, MEMTXATTRS_UNSPECIFIED, buf, size, 1);
}

static void *srawmemchr(void *str, int chr)
{
    uint8_t *ptr = (uint8_t *)str;

    while (*ptr != chr) {
        ptr++;
    }

    return ptr;
}

static uint64_t sstrlen(const char *str)
{
    const int chr = *(uint8_t *)"$";
    char *end = srawmemchr((void *)str, chr);

    return end - str;
}

static void macho_dtb_node_process(DTBNode *node, DTBNode *parent)
{
    GList *iter = NULL;
    DTBNode *child = NULL;
    DTBProp *prop = NULL;
    uint64_t count;
    uint64_t i;
    bool found;

    prop = dtb_find_prop(node, "compatible");
    if (prop != NULL) {
        g_assert_nonnull(prop->data);
        found = false;
        for (count = sizeof(KEEP_COMP) / sizeof(KEEP_COMP[0]), i = 0; i < count;
             i++) {
            if (memcmp(prop->data, KEEP_COMP[i],
                       MIN(prop->length, sstrlen(KEEP_COMP[i]))) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            g_assert_nonnull(parent);
            info_report("Removing node %s because its compatible property %s "
                        "is not in the whitelist",
                        dtb_find_prop(node, "name")->data, prop->data);
            dtb_remove_node(parent, node);
            return;
        }
    }

    prop = dtb_find_prop(node, "name");
    if (prop != NULL) {
        g_assert_nonnull(prop->data);
        for (count = sizeof(REM_NAMES) / sizeof(REM_NAMES[0]), i = 0; i < count;
             i++) {
            uint64_t size = MIN(prop->length, sstrlen(REM_NAMES[i]));
            if (memcmp(prop->data, REM_NAMES[i], size) == 0) {
                g_assert_nonnull(parent);
                dtb_remove_node(parent, node);
                return;
            }
        }
    }

    prop = dtb_find_prop(node, "device_type");
    if (prop != NULL) {
        g_assert_nonnull(prop->data);
        for (count = sizeof(REM_DEV_TYPES) / sizeof(REM_DEV_TYPES[0]), i = 0;
             i < count; i++) {
            uint64_t size = MIN(prop->length, sstrlen(REM_DEV_TYPES[i]));
            if (memcmp(prop->data, REM_DEV_TYPES[i], size) == 0) {
                g_assert_nonnull(parent);
                dtb_remove_node(parent, node);
                return;
            }
        }
    }

    for (count = sizeof(REM_PROPS) / sizeof(REM_PROPS[0]), i = 0; i < count;
         i++) {
        dtb_remove_prop_named(node, REM_PROPS[i]);
    }

    for (iter = node->children; iter != NULL;) {
        child = (DTBNode *)iter->data;

        // iter might be invalidated by macho_dtb_node_process
        iter = iter->next;
        macho_dtb_node_process(child, node);
    }
}

/*
 * \param payload_type must be at least 4 bytes long
 */
static void extract_im4p_payload(const char *filename, char *payload_type,
                                 uint8_t **data, uint32_t *length,
                                 uint8_t **secure_monitor)
{
    uint8_t *file_data;
    gsize fsize;
    char errorDescription[ASN1_MAX_ERROR_DESCRIPTION_SIZE];
    asn1_node img4_definitions = NULL;
    asn1_node img4;
    int ret;
    char magic[4];
    char description[128];
    int len;
    uint8_t *payload_data;

    if (!g_file_get_contents(filename, (gchar **)&file_data, &fsize, NULL)) {
        error_setg(&error_fatal, "file read for `%s` failed", filename);
    }

    if (asn1_array2tree(img4_definitions_array, &img4_definitions,
                        errorDescription) != ASN1_SUCCESS) {
        error_setg(&error_fatal, "ASN.1 parser initialisation failed: `%s`.",
                   errorDescription);
    }

    ret = asn1_create_element(img4_definitions, "Img4.Img4Payload", &img4);
    if (ret != ASN1_SUCCESS) {
        error_setg(&error_fatal, "Img4Payload element creation failed: %d",
                   ret);
    }

    ret =
        asn1_der_decoding(&img4, file_data, (uint32_t)fsize, errorDescription);

    if (ret != ASN1_SUCCESS) {
        *data = file_data;
        *length = (uint32_t)fsize;
        strncpy(payload_type, "raw", 4);
        asn1_delete_structure(&img4);
        asn1_delete_structure(&img4_definitions);
        return;
    }

    len = 4;
    ret = asn1_read_value(img4, "magic", magic, &len);
    if (ret != ASN1_SUCCESS) {
        error_setg(&error_fatal, "im4p magic read for `%s` failed: %d.",
                   filename, ret);
    }

    if (strncmp(magic, "IM4P", 4) != 0) {
        error_setg(&error_fatal, "`%s` is not an img4 payload.", filename);
    }

    len = 4;
    ret = asn1_read_value(img4, "type", payload_type, &len);
    if (ret != ASN1_SUCCESS) {
        error_setg(&error_fatal, "img4 payload type read for `%s` failed: %d.",
                   filename, ret);
    }

    len = 128;
    ret = asn1_read_value(img4, "description", description, &len);
    if (ret != ASN1_SUCCESS) {
        error_setg(&error_fatal,
                   "img4 payload description read for `%s` failed: %d.",
                   filename, ret);
    }

    payload_data = NULL;
    len = 0;
    ret = asn1_read_value(img4, "data", payload_data, &len);
    if (ret != ASN1_MEM_ERROR) {
        error_setg(&error_fatal, "img4 payload size read for `%s` failed: %d.",
                   filename, ret);
    }

    payload_data = g_malloc0(len);
    ret = asn1_read_value(img4, "data", payload_data, &len);
    g_free(file_data);

    if (ret != ASN1_SUCCESS) {
        error_setg(&error_fatal, "img4 payload read for `%s` failed: %d.",
                   filename, ret);
    }

    asn1_delete_structure(&img4);
    asn1_delete_structure(&img4_definitions);

    if (memcmp(payload_data, "bvx", 3) == 0) {
        size_t decode_buffer_size = len * 8;
        uint8_t *decode_buffer = g_malloc0(decode_buffer_size);
        int decoded_length =
            lzfse_decode_buffer(decode_buffer, decode_buffer_size, payload_data,
                                len, NULL /* scratch_buffer */);
        g_free(payload_data);

        if (decoded_length == 0 || decoded_length == decode_buffer_size) {
            error_setg(
                &error_fatal,
                "LZFSE decompression for `%s` failed; insufficient buffer size",
                filename);
        }

        *data = decode_buffer;
        *length = decoded_length;
        return;
    }

    if (memcmp(payload_data, "complzss", 8) == 0) {
        LzssCompHeader *comp_hdr = (LzssCompHeader *)payload_data;
        size_t uncompressed_size = be32_to_cpu(comp_hdr->uncompressed_size);
        size_t compressed_size = be32_to_cpu(comp_hdr->compressed_size);
        uint8_t *decode_buffer = g_malloc0(uncompressed_size);
        int decoded_length =
            decompress_lzss(decode_buffer, comp_hdr->data, compressed_size);
        if (decoded_length == 0 || decoded_length != uncompressed_size) {
            error_setg(&error_fatal, "LZSS decompression for `%s` failed.",
                       filename);
        }

        size_t monitor_off = compressed_size + sizeof(LzssCompHeader);
        if (secure_monitor && monitor_off < len) {
            size_t monitor_size = len - monitor_off;
            info_report("Found AP Secure Monitor in payload with size 0x%zX!",
                        monitor_size);
            *secure_monitor = g_malloc0(monitor_size);
            memcpy(*secure_monitor, payload_data + monitor_off, monitor_size);
        }

        g_free(payload_data);

        *data = decode_buffer;
        *length = decoded_length;
        return;
    }

    *data = payload_data;
    *length = len;
}

DTBNode *load_dtb_from_file(char *filename)
{
    DTBNode *root = NULL;
    uint8_t *file_data = NULL;
    uint32_t fsize;
    char payload_type[4];

    extract_im4p_payload(filename, payload_type, &file_data, &fsize, NULL);

    if (strncmp(payload_type, "dtre", 4) != 0 &&
        strncmp(payload_type, "raw", 4) != 0) {
        error_setg(&error_fatal, "`%s` is a `%.4s` object (expected `dtre`)",
                   filename, payload_type);
    }

    root = dtb_deserialise(file_data);
    g_free(file_data);
    return root;
}

void macho_populate_dtb(DTBNode *root, AppleBootInfo *info)
{
    DTBNode *child;
    DTBProp *prop;
    uint64_t memmap[2] = { 0 };

    child = dtb_get_node(root, "chosen");
    g_assert_nonnull(child);

#ifndef ENABLE_SEP_SECURITY
    dtb_set_prop_u32(child, "protected-data-access", 0);
#endif

    prop = dtb_find_prop(child, "random-seed");
    g_assert_nonnull(prop);
    qemu_guest_getrandom_nofail(prop->data, prop->length);

    dtb_set_prop_hwaddr(child, "dram-base", info->dram_base);
    dtb_set_prop_hwaddr(child, "dram-size", info->dram_size);
    dtb_set_prop(child, "firmware-version", 28, "ChefKiss QEMU Apple Silicon");

    if (info->nvram_size > XNU_MAX_NVRAM_SIZE) {
        warn_report("NVRAM size is larger than expected. (0x%" PRIx32 " vs %X)",
                    info->nvram_size, XNU_MAX_NVRAM_SIZE);
        info->nvram_size = XNU_MAX_NVRAM_SIZE;
    }
    dtb_set_prop_u32(child, "nvram-total-size", info->nvram_size);
    dtb_set_prop_u32(child, "nvram-bank-size", info->nvram_size);
    dtb_set_prop(child, "nvram-proxy-data", info->nvram_size, info->nvram_data);

    // dtb_set_prop_u32(child, "research-enabled", 1);
    dtb_set_prop_u32(child, "effective-production-status-ap", 1);
    dtb_set_prop_u32(child, "effective-security-mode-ap", 1);
    dtb_set_prop_u32(child, "security-domain", 1);
    dtb_set_prop_u32(child, "chip-epoch", 1);
    // dtb_set_prop_u32(child, "debug-enabled", 1);

    child = dtb_get_node(root, "defaults");
    g_assert_nonnull(child);
#ifndef ENABLE_SEP_SECURITY
    dtb_set_prop_null(child, "no-effaceable-storage");
#endif

    child = dtb_get_node(root, "chosen/manifest-properties");
    dtb_set_prop(child, "BNCH", sizeof(info->boot_nonce_hash),
                 info->boot_nonce_hash);

    child = dtb_get_node(root, "product");
    g_assert_nonnull(child);
    dtb_set_prop_u32(child, "allow-hactivation", 1);

    macho_dtb_node_process(root, NULL);

    child = dtb_get_node(root, "chosen/memory-map");
    g_assert_nonnull(child);

    dtb_set_prop(child, "RAMDisk", sizeof(memmap), memmap);
    dtb_set_prop(child, "TrustCache", sizeof(memmap), memmap);
    dtb_set_prop(child, "SEPFW", sizeof(memmap), memmap);
    dtb_set_prop(child, "BootArgs", sizeof(memmap), &memmap);
    dtb_set_prop(child, "DeviceTree", sizeof(memmap), &memmap);

    info->device_tree_size = ROUND_UP_16K(dtb_get_serialised_node_size(root));
}

static void set_memory_range(DTBNode *root, const char *name, uint64_t addr,
                             uint64_t size)
{
    DTBNode *child;
    DTBProp *prop;

    g_assert_cmphex(addr, !=, 0);
    g_assert_cmpuint(size, !=, 0);

    child = dtb_get_node(root, "chosen/memory-map");
    g_assert_nonnull(child);

    prop = dtb_find_prop(child, name);
    g_assert_nonnull(prop);

    ((uint64_t *)prop->data)[0] = addr;
    ((uint64_t *)prop->data)[1] = size;
}

static void remove_memory_range(DTBNode *root, const char *name)
{
    DTBNode *child;

    child = dtb_get_node(root, "chosen/memory-map");
    g_assert_nonnull(child);

    dtb_remove_prop_named(child, "DeviceTree");
}

static void set_or_remove_memory_range(DTBNode *root, const char *name,
                                       uint64_t addr, uint64_t size)
{
    if (addr) {
        set_memory_range(root, name, addr, size);
    } else {
        remove_memory_range(root, name);
    }
}

void macho_load_dtb(DTBNode *root, AddressSpace *as, MemoryRegion *mem,
                    const char *name, AppleBootInfo *info)
{
    g_autofree uint8_t *buf = NULL;

    set_memory_range(root, "DeviceTree", info->device_tree_addr,
                     info->device_tree_size);
    set_or_remove_memory_range(root, "RAMDisk", info->ramdisk_addr,
                               info->ramdisk_size);
    set_or_remove_memory_range(root, "TrustCache", info->trustcache_addr,
                               info->trustcache_size);
    set_or_remove_memory_range(root, "SEPFW", info->sep_fw_addr,
                               info->sep_fw_size);
    set_memory_range(root, "BootArgs", info->kern_boot_args_addr,
                     info->kern_boot_args_size);

    if (info->ticket_data && info->ticket_length) {
        QCryptoHashAlgo alg = QCRYPTO_HASH_ALGO_SHA1;
        g_autofree uint8_t *hash = NULL;
        size_t hash_len = 0;
        DTBNode *child = dtb_get_node(root, "chosen");
        DTBProp *prop = NULL;
        g_autofree Error *err = NULL;
        prop = dtb_find_prop(child, "crypto-hash-method");

        if (prop != NULL) {
            if (strcmp((char *)prop->data, "sha2-384") == 0) {
                alg = QCRYPTO_HASH_ALGO_SHA384;
            }
        }

        prop = dtb_find_prop(child, "boot-manifest-hash");
        g_assert_nonnull(prop);

        if (qcrypto_hash_bytes(alg, info->ticket_data, info->ticket_length,
                               &hash, &hash_len, &err) >= 0) {
            g_assert_cmpuint(hash_len, ==, prop->length);
            memcpy(prop->data, hash, hash_len);
        } else {
            error_report_err(err);
        }
    }

    g_assert_cmpuint(info->device_tree_size, >=,
                     dtb_get_serialised_node_size(root));
    buf = g_malloc0(info->device_tree_size);
    dtb_serialise(buf, root);
    allocate_and_copy(mem, as, name, info->device_tree_addr,
                      info->device_tree_size, buf);
}

uint8_t *load_trustcache_from_file(const char *filename, uint64_t *size)
{
    uint32_t *trustcache_data = NULL;
    uint64_t trustcache_size = 0;
    g_autofree uint8_t *file_data = NULL;
    unsigned long file_size = 0;
    uint32_t length = 0;
    char payload_type[4];
    uint32_t trustcache_version, trustcache_entry_count, expected_file_size;
    uint32_t trustcache_entry_size = 0;

    extract_im4p_payload(filename, payload_type, &file_data, &length, NULL);

    if (strncmp(payload_type, "trst", 4) != 0 &&
        strncmp(payload_type, "rtsc", 4) != 0 &&
        strncmp(payload_type, "raw", 4) != 0) {
        error_setg(&error_fatal,
                   "`%s` is a `%.4s` object (expected `trst`/`rtsc`).",
                   filename, payload_type);
    }

    file_size = (unsigned long)length;

    trustcache_size = ROUND_UP_16K(file_size + 8);
    trustcache_data = (uint32_t *)g_malloc(trustcache_size);
    trustcache_data[0] = 1; // #trustcaches
    trustcache_data[1] = 8; // offset
    memcpy(&trustcache_data[2], file_data, file_size);

    // Validate the trustcache v1 header. The layout is:
    // uint32_t version
    // uuid (16 bytes)
    // uint32_t entry_count
    //
    // The cache is then followed by entry_count entries, each of which
    // contains a 20 byte hash and 2 additional bytes (hence is 22 bytes long)
    // for v1 and contains a 20 byte hash and 4 additional bytes (hence is 24
    // bytes long) for v2
    trustcache_version = trustcache_data[2];
    trustcache_entry_count = trustcache_data[7];

    switch (trustcache_version) {
    case 1:
        trustcache_entry_size = 22;
        break;
    case 2:
        trustcache_entry_size = 24;
        break;
    default:
        error_setg(
            &error_fatal,
            "invalid trustcache header in `%s` (expected v1 or v2, got %d)",
            filename, trustcache_version);
        break;
    }

    // 24 is header size
    expected_file_size = 24 + trustcache_entry_count * trustcache_entry_size;

    g_assert_cmpuint(file_size, ==, expected_file_size);

    *size = trustcache_size;
    return (uint8_t *)trustcache_data;
}

void macho_load_trustcache(void *trustcache, uint64_t size, AddressSpace *as,
                           MemoryRegion *mem, hwaddr pa)
{
    allocate_and_copy(mem, as, "TrustCache", pa, size, trustcache);
}

void macho_load_ramdisk(const char *filename, AddressSpace *as,
                        MemoryRegion *mem, hwaddr pa, uint64_t *size)
{
    uint8_t *file_data = NULL;
    unsigned long file_size = 0;
    uint32_t length = 0;
    char payload_type[4];

    extract_im4p_payload(filename, payload_type, &file_data, &length, NULL);
    if (strncmp(payload_type, "rdsk", 4) != 0 &&
        strncmp(payload_type, "raw", 4) != 0) {
        error_setg(&error_fatal, "`%s` is a `%.4s` object (expected `rdsk`)",
                   filename, payload_type);
    }

    file_size = length;
    file_data = g_realloc(file_data, file_size);

    allocate_and_copy(mem, as, "RamDisk", pa, file_size, file_data);
    *size = file_size;
    g_free(file_data);
}

void macho_load_raw_file(const char *filename, AddressSpace *as,
                         MemoryRegion *mem, const char *name, hwaddr file_pa,
                         uint64_t *size)
{
    uint8_t *file_data = NULL;
    gsize sizef;

    if (g_file_get_contents(filename, (char **)&file_data, &sizef, NULL)) {
        *size = sizef;
        allocate_and_copy(mem, as, name, file_pa, *size, file_data);
        g_free(file_data);
    } else {
        error_setg(&error_fatal, "file read for `%s` failed", filename);
    }
}

bool xnu_contains_boot_arg(const char *bootArgs, const char *arg,
                           bool prefixmatch)
{
    g_autofree char *args = g_strdup(bootArgs);
    char *pos = args;
    char *token;
    size_t arglen = strlen(arg);

    if (args == NULL) {
        return false;
    }

    while ((token = qemu_strsep(&pos, " ")) != NULL) {
        if (prefixmatch && strncmp(token, arg, arglen) == 0) {
            return true;
        } else if (strcmp(token, arg) == 0) {
            return true;
        }
    }

    return false;
}

void apple_monitor_setup_boot_args(
    const char *name, AddressSpace *as, MemoryRegion *mem, hwaddr addr,
    hwaddr virt_base, hwaddr phys_base, hwaddr mem_size, hwaddr kern_args,
    hwaddr kern_entry, hwaddr kern_phys_base, hwaddr kern_phys_slide,
    hwaddr kern_virt_slide, hwaddr kern_text_section_off)
{
    AppleMonitorBootArgs boot_args;

    memset(&boot_args, 0, sizeof(boot_args));
    boot_args.version = BOOT_ARGS_VERSION_4;
    boot_args.virt_base = virt_base;
    boot_args.phys_base = phys_base;
    boot_args.mem_size = mem_size;
    boot_args.kern_args = kern_args;
    boot_args.kern_entry = kern_entry;
    boot_args.kern_phys_base = kern_phys_base;
    boot_args.kern_phys_slide = kern_phys_slide;
    boot_args.kern_virt_slide = kern_virt_slide;
    boot_args.kern_text_section_off = kern_text_section_off;
    qcrypto_random_bytes(&boot_args.random_bytes, 0x10, NULL);

    allocate_and_copy(mem, as, name, addr, sizeof(boot_args), &boot_args);
}

void macho_setup_bootargs(const char *name, AddressSpace *as, MemoryRegion *mem,
                          hwaddr addr, hwaddr virt_base, hwaddr phys_base,
                          hwaddr mem_size, hwaddr kernel_top, hwaddr dtb_va,
                          hwaddr dtb_size, AppleVideoArgs video_args,
                          const char *cmdline)
{
    AppleKernelBootArgs boot_args;

    memset(&boot_args, 0, sizeof(boot_args));
    boot_args.revision = BOOT_ARGS_VERSION_2;
    boot_args.version = BOOT_ARGS_REVISION_2;
    boot_args.virt_base = virt_base;
    boot_args.phys_base = phys_base;
    boot_args.mem_size = mem_size;
    memcpy(&boot_args.video_args, &video_args, sizeof(boot_args.video_args));
    boot_args.kernel_top = kernel_top;
    boot_args.device_tree_ptr = dtb_va;
    boot_args.device_tree_length = dtb_size;
    boot_args.boot_flags = 1; // is dark boot

    if (cmdline) {
        g_strlcpy(boot_args.cmdline, cmdline, sizeof(boot_args.cmdline));
    }

    allocate_and_copy(mem, as, name, addr, sizeof(boot_args), &boot_args);
}

void macho_highest_lowest(MachoHeader64 *mh, uint64_t *lowaddr,
                          uint64_t *highaddr)
{
    MachoLoadCommand *cmd =
        (MachoLoadCommand *)((uint8_t *)mh + sizeof(MachoHeader64));
    // iterate all the segments once to find highest and lowest addresses
    uint64_t low_addr_temp = ~0, high_addr_temp = 0;
    unsigned int index;

    for (index = 0; index < mh->n_cmds; index++) {
        switch (cmd->cmd) {
        case LC_SEGMENT_64: {
            MachoSegmentCommand64 *segCmd = (MachoSegmentCommand64 *)cmd;
            if (!strncmp(segCmd->segname, "__PAGEZERO", 11)) {
                continue;
            }

            if (segCmd->vmaddr < low_addr_temp) {
                low_addr_temp = segCmd->vmaddr;
            }
            if (segCmd->vmaddr + segCmd->vmsize > high_addr_temp) {
                high_addr_temp = segCmd->vmaddr + segCmd->vmsize;
            }
            break;
        }

        default:
            break;
        }
        cmd = (MachoLoadCommand *)((char *)cmd + cmd->cmd_size);
    }
    if (lowaddr) {
        *lowaddr = low_addr_temp & -0x2000000ull;
    }
    if (highaddr) {
        *highaddr = high_addr_temp;
    }
}

void macho_text_base(MachoHeader64 *mh, uint64_t *base)
{
    MachoLoadCommand *cmd = (MachoLoadCommand *)(mh + 1);
    unsigned int index;
    *base = 0;
    for (index = 0; index < mh->n_cmds; index++) {
        switch (cmd->cmd) {
        case LC_SEGMENT_64: {
            MachoSegmentCommand64 *segCmd = (MachoSegmentCommand64 *)cmd;

            if (segCmd->vmaddr && segCmd->fileoff == 0) {
                *base = segCmd->vmaddr;
                return;
            }
            break;
        }
        default:
            break;
        }
        cmd = (MachoLoadCommand *)((char *)cmd + cmd->cmd_size);
    }
}

MachoHeader64 *macho_load_file(const char *filename,
                               MachoHeader64 **secure_monitor)
{
    uint32_t len;
    uint8_t *data = NULL;
    char payload_type[4];
    MachoHeader64 *mh = NULL;

    extract_im4p_payload(filename, payload_type, &data, &len,
                         (uint8_t **)secure_monitor);

    if (strncmp(payload_type, "krnl", 4) != 0 &&
        strncmp(payload_type, "raw", 3) != 0) {
        error_setg(&error_fatal, "`%s` is a `%.4s` object (expected `krnl`)",
                   filename, payload_type);
    }

    mh = macho_parse(data, len);
    g_free(data);
    return mh;
}

MachoHeader64 *macho_parse(uint8_t *data, uint32_t len)
{
    uint8_t *phys_base = NULL;
    MachoHeader64 *mh;
    MachoLoadCommand *cmd;
    uint64_t lowaddr = 0, highaddr = 0;
    uint64_t virt_base = 0;
    uint64_t text_base = 0;
    int index;

    mh = (MachoHeader64 *)data;
    g_assert_cmphex(mh->magic, ==, MACH_MAGIC_64);

    macho_highest_lowest(mh, &lowaddr, &highaddr);
    g_assert_cmphex(lowaddr, <, highaddr);

    phys_base = g_malloc0(highaddr - lowaddr);
    virt_base = lowaddr;
    cmd = (MachoLoadCommand *)(data + sizeof(MachoHeader64));

    for (index = 0; index < mh->n_cmds; index++) {
        switch (cmd->cmd) {
        case LC_SEGMENT_64: {
            MachoSegmentCommand64 *segCmd = (MachoSegmentCommand64 *)cmd;
            if (!strncmp(segCmd->segname, "__PAGEZERO", 11)) {
                continue;
            }
            if (segCmd->vmsize == 0) {
                break;
            }
            g_assert_cmphex(segCmd->fileoff, <, len);
            if (segCmd->vmaddr && segCmd->fileoff == 0 && !text_base) {
                text_base = segCmd->vmaddr;
            }
            memcpy(phys_base + segCmd->vmaddr - virt_base,
                   data + segCmd->fileoff, segCmd->filesize);
            break;
        }
        default:
            break;
        }

        cmd = (MachoLoadCommand *)((char *)cmd + cmd->cmd_size);
    }

    return (MachoHeader64 *)(phys_base + text_base - virt_base);
}

uint32_t macho_build_version(MachoHeader64 *mh)
{
    MachoLoadCommand *cmd;
    int index;

    if (mh->file_type == MH_FILESET) {
        mh = macho_get_fileset_header(mh, "com.apple.kernel");
    }
    cmd = (MachoLoadCommand *)((char *)mh + sizeof(MachoHeader64));

    for (index = 0; index < mh->n_cmds; index++) {
        switch (cmd->cmd) {
        case LC_BUILD_VERSION: {
            MachoBuildVersionCommand *buildVerCmd =
                (MachoBuildVersionCommand *)cmd;
            return buildVerCmd->sdk;
            break;
        }

        default:
            break;
        }

        cmd = (MachoLoadCommand *)((char *)cmd + cmd->cmd_size);
    }
    return 0;
}

uint32_t macho_platform(MachoHeader64 *mh)
{
    MachoLoadCommand *cmd;
    int index;

    if (mh->file_type == MH_FILESET) {
        mh = macho_get_fileset_header(mh, "com.apple.kernel");
    }
    cmd = (MachoLoadCommand *)((char *)mh + sizeof(MachoHeader64));

    for (index = 0; index < mh->n_cmds; index++) {
        switch (cmd->cmd) {
        case LC_BUILD_VERSION: {
            MachoBuildVersionCommand *buildVerCmd =
                (MachoBuildVersionCommand *)cmd;
            return buildVerCmd->platform;
        }

        default:
            break;
        }

        cmd = (MachoLoadCommand *)((char *)cmd + cmd->cmd_size);
    }
    return 0;
}

const char *macho_platform_string(MachoHeader64 *mh)
{
    uint32_t platform = macho_platform(mh);
    switch (platform) {
    case PLATFORM_MACOS:
        return "macOS";
    case PLATFORM_IOS:
        return "iOS";
    case PLATFORM_TVOS:
        return "tvOS";
    case PLATFORM_WATCHOS:
        return "watchOS";
    case PLATFORM_BRIDGEOS:
        return "bridgeOS";
    default:
        return "Unknown";
    }
}

static MachoSegmentCommand64 *macho_get_firstseg(MachoHeader64 *header)
{
    MachoSegmentCommand64 *sgp;
    uint32_t i;

    sgp = (MachoSegmentCommand64 *)(header + 1);

    for (i = 0; i < header->n_cmds; i++) {
        if (sgp->cmd == LC_SEGMENT_64) {
            return sgp;
        }

        sgp = (MachoSegmentCommand64 *)((char *)sgp + sgp->cmd_size);
    }

    // not found
    return NULL;
}

static MachoSegmentCommand64 *macho_get_nextseg(MachoHeader64 *header,
                                                MachoSegmentCommand64 *seg)
{
    MachoSegmentCommand64 *sgp;
    uint32_t i;
    bool found = false;

    sgp = (MachoSegmentCommand64 *)((char *)header + sizeof(MachoHeader64));

    for (i = 0; i < header->n_cmds; i++) {
        if (found && sgp->cmd == LC_SEGMENT_64) {
            return sgp;
        }
        if (seg == sgp) {
            found = true;
        }

        sgp = (MachoSegmentCommand64 *)((char *)sgp + sgp->cmd_size);
    }

    // not found
    return NULL;
}

static MachoSection64 *firstsect(MachoSegmentCommand64 *seg)
{
    return (MachoSection64 *)(seg + 1);
}

static MachoSection64 *nextsect(MachoSection64 *sp)
{
    return sp + 1;
}

static MachoSection64 *endsect(MachoSegmentCommand64 *seg)
{
    MachoSection64 *sp;

    sp = (MachoSection64 *)(seg + 1);
    return &sp[seg->nsects];
}

static void macho_process_symbols(MachoHeader64 *mh, uint64_t slide)
{
    MachoLoadCommand *cmd;
    uint8_t *data;
    uint64_t kernel_low, kernel_high;
    uint32_t index;
    void *base;
    MachoSegmentCommand64 *linkedit_seg;
    MachoNList64 *sym;
    uint32_t off;
    hwaddr text_base;

    if (!slide) {
        return;
    }

    macho_highest_lowest(mh, &kernel_low, &kernel_high);

    data = macho_get_buffer(mh);
    linkedit_seg = macho_get_segment(mh, "__LINKEDIT");

    cmd = (MachoLoadCommand *)(mh + 1);
    for (index = 0; index < mh->n_cmds; index++) {
        switch (cmd->cmd) {
        case LC_SYMTAB: {
            MachoSymtabCommand *symtab = (MachoSymtabCommand *)cmd;
            if (linkedit_seg == NULL) {
                error_report("Did not find __LINKEDIT segment");
                return;
            }
            base = data + (linkedit_seg->vmaddr - kernel_low);
            off = linkedit_seg->fileoff;
            sym = (MachoNList64 *)(base + (symtab->sym_off - off));
            for (int i = 0; i < symtab->nsyms; i++) {
                if (sym[i].n_type & N_STAB) {
                    continue;
                }
                sym[i].n_value += slide;
            }
            break;
        }
        case LC_DYSYMTAB: {
            MachoDysymtabCommand *dysymtab = (MachoDysymtabCommand *)cmd;
            if (!dysymtab->loc_rel_n) {
                break;
            }

            if (linkedit_seg == NULL) {
                error_report("Did not find __LINKEDIT segment");
                return;
            }

            base = data + (linkedit_seg->vmaddr - kernel_low);
            off = linkedit_seg->fileoff;
            macho_text_base(mh, &text_base);
            for (size_t i = 0; i < dysymtab->loc_rel_n; i++) {
                int32_t r_address =
                    *(int32_t *)(base + (dysymtab->loc_rel_off - off) + i * 8);
                *(uint64_t *)(data + ((text_base - kernel_low) + r_address)) +=
                    slide;
            }
            break;
        }
        default:
            break;
        }
        cmd = (MachoLoadCommand *)((char *)cmd + cmd->cmd_size);
    }
}

void macho_allocate_segment_records(DTBNode *memory_map, MachoHeader64 *mh)
{
    unsigned int index;
    MachoLoadCommand *cmd;

    cmd = (MachoLoadCommand *)((char *)mh + sizeof(MachoHeader64));
    for (index = 0; index < mh->n_cmds; index++) {
        switch (cmd->cmd) {
        case LC_SEGMENT_64: {
            MachoSegmentCommand64 *segCmd = (MachoSegmentCommand64 *)cmd;
            char region_name[32] = { 0 };

            snprintf(region_name, sizeof(region_name), "Kernel-%s",
                     segCmd->segname);
            struct MemoryMapFileInfo {
                uint64_t paddr;
                uint64_t length;
            } file_info = { 0 };
            dtb_set_prop(memory_map, region_name, sizeof(file_info),
                         &file_info);
            break;
        }
        default:
            break;
        }

        cmd = (MachoLoadCommand *)((char *)cmd + cmd->cmd_size);
    }
}

hwaddr arm_load_macho(MachoHeader64 *mh, AddressSpace *as, MemoryRegion *mem,
                      DTBNode *memory_map, hwaddr phys_base,
                      uint64_t virt_slide)
{
    uint8_t *data = NULL;
    unsigned int index;
    MachoLoadCommand *cmd;
    hwaddr pc = 0;
    data = macho_get_buffer(mh);
    uint64_t virt_low, virt_high;
    macho_highest_lowest(mh, &virt_low, &virt_high);
    bool is_fileset = mh->file_type == MH_FILESET;
    MachoHeader64 *mh2 = NULL;
    void *load_from2 = NULL;

    cmd = (MachoLoadCommand *)(mh + 1);
    if (!is_fileset) {
        macho_process_symbols(mh, virt_slide);
    }
    for (index = 0; index < mh->n_cmds; index++) {
        switch (cmd->cmd) {
        case LC_SEGMENT_64: {
            MachoSegmentCommand64 *segCmd = (MachoSegmentCommand64 *)cmd;
            if (!strncmp(segCmd->segname, "__PAGEZERO", 11)) {
                continue;
            }
            char region_name[64] = { 0 };
            void *load_from = (void *)(data + segCmd->vmaddr - virt_low);
            hwaddr load_to = (phys_base + segCmd->vmaddr - virt_low);
            if (memory_map) {
                snprintf(region_name, sizeof(region_name), "Kernel-%s",
                         segCmd->segname);
                struct MemoryMapFileInfo {
                    uint64_t paddr;
                    uint64_t length;
                } file_info = { load_to, segCmd->vmsize };
                dtb_set_prop(memory_map, region_name, sizeof(file_info),
                             &file_info);
            } else {
                snprintf(region_name, sizeof(region_name), "TZ1-%s",
                         segCmd->segname);
            }

            if (segCmd->vmsize == 0) {
                break;
            }

            if (!is_fileset) {
                MachoSection64 *sp;
                for (sp = firstsect(segCmd); sp != endsect(segCmd);
                     sp = nextsect(sp)) {
                    if ((sp->flags & SECTION_TYPE) ==
                        S_NON_LAZY_SYMBOL_POINTERS) {
                        load_from2 = (void *)(data + sp->addr - virt_low);
                        void **nl_symbol_ptr;
                        for (nl_symbol_ptr = load_from2;
                             nl_symbol_ptr < (void **)(load_from2 + sp->size);
                             nl_symbol_ptr++) {
                            *nl_symbol_ptr += virt_slide;
                        }
                    }
                }
            }

            if (!is_fileset) {
                if (strcmp(segCmd->segname, "__TEXT") == 0) {
                    mh2 = load_from;
                    MachoSegmentCommand64 *seg;
                    g_assert_cmphex(mh2->magic, ==, MACH_MAGIC_64);
                    for (seg = macho_get_firstseg(mh2); seg != NULL;
                         seg = macho_get_nextseg(mh2, seg)) {
                        MachoSection64 *sp;
                        seg->vmaddr += virt_slide;
                        for (sp = firstsect(seg); sp != endsect(seg);
                             sp = nextsect(sp)) {
                            sp->addr += virt_slide;
                        }
                    }
                }
            }


#if 0
            error_report(
                "Loading %s to 0x%" PRIx64 " (filesize: 0x%" PRIx64 " vmsize: 0x%" PRIx64 ")",
                region_name, load_to, segCmd->filesize, segCmd->vmsize);
#endif
            uint8_t *buf = g_malloc0(segCmd->vmsize);
            memcpy(buf, load_from, segCmd->filesize);
            allocate_and_copy(mem, as, region_name, load_to, segCmd->vmsize,
                              buf);
            g_free(buf);

            if (!is_fileset) {
                if (strcmp(segCmd->segname, "__TEXT") == 0) {
                    mh2 = load_from;
                    MachoSegmentCommand64 *seg;
                    g_assert_cmphex(mh2->magic, ==, MACH_MAGIC_64);
                    for (seg = macho_get_firstseg(mh2); seg != NULL;
                         seg = macho_get_nextseg(mh2, seg)) {
                        MachoSection64 *sp;
                        seg->vmaddr -= virt_slide;
                        for (sp = firstsect(seg); sp != endsect(seg);
                             sp = nextsect(sp)) {
                            sp->addr -= virt_slide;
                        }
                    }
                }
            }

            if (!is_fileset) {
                MachoSection64 *sp;
                for (sp = firstsect(segCmd); sp != endsect(segCmd);
                     sp = nextsect(sp)) {
                    if ((sp->flags & SECTION_TYPE) ==
                        S_NON_LAZY_SYMBOL_POINTERS) {
                        load_from2 = (void *)(data + sp->addr - virt_low);
                        void **nl_symbol_ptr;
                        for (nl_symbol_ptr = load_from2;
                             nl_symbol_ptr < (void **)(load_from2 + sp->size);
                             nl_symbol_ptr++) {
                            *nl_symbol_ptr -= virt_slide;
                        }
                    }
                }
            }
            break;
        }

        case LC_UNIXTHREAD: {
            // grab just the entry point PC
            uint64_t *ptrPc = (uint64_t *)((char *)cmd + 0x110);

            // 0x110 for arm64 only.
            pc = vtop_bases(*ptrPc, phys_base, virt_low);

            break;
        }

        default:
            break;
        }

        cmd = (MachoLoadCommand *)((char *)cmd + cmd->cmd_size);
    }

    if (!is_fileset) {
        macho_process_symbols(mh, -virt_slide);
    }

    return pc;
}

uint8_t *macho_get_buffer(MachoHeader64 *hdr)
{
    uint64_t lowaddr = 0, highaddr = 0, text_base = 0;

    macho_highest_lowest(hdr, &lowaddr, &highaddr);
    macho_text_base(hdr, &text_base);

    return (uint8_t *)((uint8_t *)hdr - text_base + lowaddr);
}

void macho_free(MachoHeader64 *hdr)
{
    g_free(macho_get_buffer(hdr));
}

MachoFilesetEntryCommand *macho_get_fileset(MachoHeader64 *header,
                                            const char *entry)
{
    if (header->file_type != MH_FILESET) {
        return NULL;
    }
    MachoFilesetEntryCommand *fileset;
    fileset =
        (MachoFilesetEntryCommand *)((char *)header + sizeof(MachoHeader64));

    for (uint32_t i = 0; i < header->n_cmds; i++) {
        if (fileset->cmd == LC_FILESET_ENTRY) {
            const char *entry_id = (char *)fileset + fileset->entry_id;
            if (!strcmp(entry_id, entry)) {
                return fileset;
            }
        }

        fileset =
            (MachoFilesetEntryCommand *)((char *)fileset + fileset->cmd_size);
    }
    return NULL;
}

MachoHeader64 *macho_get_fileset_header(MachoHeader64 *header,
                                        const char *entry)
{
    MachoFilesetEntryCommand *fileset = macho_get_fileset(header, entry);
    MachoHeader64 *sub_header;
    if (fileset == NULL) {
        return NULL;
    }
    sub_header = (MachoHeader64 *)((char *)header + fileset->file_off);
    return sub_header;
}

MachoSegmentCommand64 *macho_get_segment(MachoHeader64 *header,
                                         const char *segname)
{
    uint32_t i;

    if (header->file_type == MH_FILESET) {
        return macho_get_segment(
            macho_get_fileset_header(header, "com.apple.kernel"), segname);
    } else {
        MachoSegmentCommand64 *sgp;


        for (sgp = (MachoSegmentCommand64 *)(header + 1), i = 0;
             i < header->n_cmds; i++,
            sgp = (MachoSegmentCommand64 *)((char *)sgp + sgp->cmd_size)) {
            if (sgp->cmd == LC_SEGMENT_64) {
                if (strncmp(sgp->segname, segname, sizeof(sgp->segname)) == 0)
                    return sgp;
            }
        }
    }

    return NULL;
}

MachoSection64 *macho_get_section(MachoSegmentCommand64 *seg,
                                  const char *sect_name)
{
    MachoSection64 *sp;
    uint32_t i;

    for (sp = (MachoSection64 *)(seg + 1), i = 0; i < seg->nsects; i++, sp++) {
        if (!strncmp(sp->sect_name, sect_name, sizeof(sp->sect_name))) {
            return sp;
        }
    }

    return NULL;
}

uint64_t xnu_slide_hdr_va(MachoHeader64 *header, uint64_t hdr_va)
{
    return hdr_va + g_virt_slide;
}

void *xnu_va_to_ptr(uint64_t va)
{
    return (void *)(va - g_virt_base + g_phys_base);
}
