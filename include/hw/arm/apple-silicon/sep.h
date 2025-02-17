/*
 * Apple SEP.
 *
 * Copyright (c) 2023-2025 Visual Ehrmanntraut.
 * Copyright (c) 2023-2025 Christian Inci.
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

#ifndef HW_ARM_APPLE_SILICON_SEP_H
#define HW_ARM_APPLE_SILICON_SEP_H

#include "qemu/osdep.h"
#include "hw/arm/apple-silicon/dtb.h"
#include "hw/i2c/i2c.h"
#include "hw/misc/apple-silicon/a7iop/core.h"
#include "hw/nvram/eeprom_at24c.h"
#include "hw/sysbus.h"
#include "qemu/typedefs.h"
#include "qom/object.h"
#include "cpu-qom.h"
#include "nettle/drbg-ctr.h"
#include "nettle/ecc.h"
#include "nettle/sha2.h"
#include "stdbool.h"
#include "stdint.h"

#define TYPE_APPLE_SEP "apple-sep"
OBJECT_DECLARE_TYPE(AppleSEPState, AppleSEPClass, APPLE_SEP)

#define TYPE_APPLE_SSC "apple-ssc"
typedef struct AppleSSCState AppleSSCState;
DECLARE_INSTANCE_CHECKER(AppleSSCState, APPLE_SSC, TYPE_APPLE_SSC)

#define DEBUG_TRACE_SIZE (0x10000)

// Some SEPFW versions can be a tad larger than 8 MiB
#define SEPFW_MAPPING_SIZE (16 * MiB)
#define SEP_DMA_MAPPING_SIZE (SEPFW_MAPPING_SIZE * 2)
#define SEP_SHMBUF_BASE (SEPFW_MAPPING_SIZE + 0xC000)

typedef struct {
    uint8_t key[32];
    uint8_t fifo[16];
    uint32_t offset_0x70;
    uint64_t ecid;
    uint64_t counter;
    uint32_t config;
    bool ctr_drbg_init;
    struct drbg_ctr_aes256_ctx ctr_drbg_rng;
} AppleTRNGState;

typedef struct {
    uint32_t chip_id;
    uint32_t status; // 0x4
    uint32_t command; // 0x8
    uint32_t interrupt_status; // 0xc
    uint32_t interrupt_enabled; // 0x10
    uint32_t reg_0x14_keywrap_iterations_counter; // 0x14
    uint32_t reg_0x18_keydisable; // 0x18
    uint32_t seed_bits; // 0x1c
    uint32_t seed_bits_lock; // 0x20
    union {
        struct {
            uint8_t iv[16]; // 0x40 // IV for enc, IN for dec?
            uint8_t in[16]; // 0x50 // IN for enc, IV for dec?
        };
        struct {
            uint8_t in_dec[16]; // 0x40 // IV for enc, IN for dec?
            uint8_t iv_dec[16]; // 0x50 // IN for enc, IV for dec?
        };
        uint8_t in_full[32]; // 0x40
    };
    // uint8_t in_t8015[16];      // 0x100
    // uint8_t iv_t8015[16];      // 0x110
    union {
        struct {
            uint8_t tag_out[16]; // 0x60
            uint8_t out[16]; // 0x70
        };
        uint8_t out_full[32]; // 0x60
    };
    uint8_t key_256_in[32]; // 0x40 ; for custom key
    uint8_t key_t8015_in[16]; // 0x100 ; for custom key
    uint8_t key_256_out[32]; // 0x60 ; for custom key
    uint8_t key_128_out[16]; // 0x60 ; for custom key
    //
    uint8_t keywrap_key_uid0[32];
    uint8_t keywrap_key_uid1[32];
    uint8_t custom_key_index[4][32];
    bool custom_key_index_enabled[4];
    // put keywrap_uid[01]_enabled here, or else ASAN will complain about
    // misalignment.
    bool keywrap_uid0_enabled;
    bool keywrap_uid1_enabled;
} AppleAESSState;

typedef struct {
    uint32_t command; // 0x0
    uint32_t status0; // 0x4
    uint32_t status_in0; // 0x8
    uint32_t img4out_dgst_clock; // 0x40
    uint8_t output0[32]; // 0x60 ; read_cmd_0x2
    uint8_t input0[0x80]; // 0x80 ; write_cmd_0x0 ; SMRK_pub ; 1024 bits ; measurement==0x34_bytes
    uint8_t public_key[32]; // 0x100 // for AESS ; read_cmd_0x0 ; read public_key ; status_in0 needs to be 0x1
    uint8_t attest_hash[32]; // 0x180 ; read_cmd_0x3 ; read attest_hash ; status_in0 needs to be 0x1
    uint8_t input1[0x20a]; // 0x200 .. 0x40a (not inclusive) ; write_cmd_0x1 ; 4176 bits, maybe rsa input?
    uint32_t chip_revision_clock; // 0x800
    uint32_t cmd_0xd_answer; // 0x820 ; cmd_0xd
    uint32_t chipid_ecid_misc_clock; // 0x840
} ApplePKAState;

#define KBKDF_CMAC_OUTPUT_LEN 0x48
#define AES_CCM_NONCE_LENGTH 12
#define AES_CCM_AUTH_LENGTH 8
#define AES_CCM_TAG_LENGTH 0x10
#define AES_CCM_COUNTER_LENGTH 4
#define AES_CCM_MAX_DATA_LENGTH 0x54
#define MSG_PREFIX_LENGTH 4

#define KBKDF_KEY_SEED_OFFSET 0x00
#define KBKDF_KEY_REQUEST_KEY_OFFSET 0x08
#define KBKDF_KEY_RESPONSE_KEY_OFFSET 0x28
#define KBKDF_KEY_SEED_LENGTH 8
#define KBKDF_KEY_KEY_LENGTH 0x20
#define KBKDF_KEY_MAX_SLOTS 0x49
#define KBKDF_KEY_KEY_FILE_OFFSET 0x100 // 0x100*4*0x40 // store mac_keys after that

#define KBKDF_CMAC_LENGTH_SIZE 2
#define KBKDF_CMAC_LABEL_SIZE 0x10
#define KBKDF_CMAC_CONTEXT_SIZE MSG_PREFIX_LENGTH

#define CMD_METADATA_READ_REQUEST_ENCRYPTED_LENGTH 0x10
#define CMD_METADATA_PAYLOAD_LENGTH 0x20
#define CMD_METADATA_DATA_PAYLOAD_LENGTH 0x40

#define SSC_MAX_REQUEST_SIZE 0x84
#define SSC_MAX_RESPONSE_SIZE 0xc4

#define BYTELEN_384 0x30

#define SECP384_PUBLIC_SIZE 0x30
#define SECP384_PUBLIC_XY_SIZE (SECP384_PUBLIC_SIZE * 2)

#define SSC_REQUEST_MAX_COPIES 4 // 0 .. 3

#define SSC_RESPONSE_FLAG_COMMAND_SIZE_MISMATCH 0x02
#define SSC_RESPONSE_FLAG_COMMAND_OR_FIELD_INVALID 0x04
#define SSC_RESPONSE_FLAG_KEYSLOT_INVALID 0x08
#define SSC_RESPONSE_FLAG_CMAC_INVALID 0x10
#define SSC_RESPONSE_FLAG_CURVE_INVALID 0x20
#define SSC_RESPONSE_FLAG_OK 0x80

struct AppleSSCState {
    /*< private >*/
    I2CSlave i2c;
    BlockBackend *blk;

    /*< public >*/
    uint32_t req_cur;
    uint32_t resp_cur;
    uint8_t req_cmd[1024];
    uint8_t resp_cmd[1024];

    AppleAESSState *aess_state;
    struct ecc_scalar ecc_key_main, ecc_keys[KBKDF_KEY_MAX_SLOTS];
    // struct ecc_point  ecc_pub0, ecc_pub1, cmd0_ecpub;
    uint8_t random_hmac_key[SHA256_DIGEST_SIZE];
    uint8_t slot_hmac_key[KBKDF_KEY_MAX_SLOTS][SHA256_DIGEST_SIZE];
    uint8_t kbkdf_keys[KBKDF_KEY_MAX_SLOTS][KBKDF_CMAC_OUTPUT_LEN];
    uint32_t kbkdf_counter[KBKDF_KEY_MAX_SLOTS];
    uint8_t cpsn[0x07];
    // bool cmd_0x7_called;
};

#define PMGR_BASE_REG_SIZE (0x10000)
#define TRNG_REGS_REG_SIZE (0x10000)
#define KEY_BASE_REG_SIZE (0x10000)
// #define KEY_FKEY_REG_SIZE (0x4000) // T8015
#define KEY_FCFG_REG_SIZE (0x14000) // T8030 ; T8015 size is 0x10000
#define MONI_BASE_REG_SIZE (0x40000)
#define MONI_THRM_REG_SIZE (0x10000)
#define EISP_BASE_REG_SIZE (0x240000)
#define EISP_HMAC_REG_SIZE (0x4000)
#define AESS_BASE_REG_SIZE (0x10000)
#define AESH_BASE_REG_SIZE (0x10000)
#define PKA_BASE_REG_SIZE (0x10000)
#define PKA_TMM_REG_SIZE (0x4000)
#define MISC0_REG_SIZE (0x4000) // ?
#define MISC2_REG_SIZE (0x4000) // ?
#define PROGRESS_REG_SIZE (0x4000) // ?
#define BOOT_MONITOR_REG_SIZE (0x4000) // ?

struct AppleSEPClass {
    /*< private >*/
    SysBusDeviceClass base_class;

    /*< public >*/
    DeviceRealize parent_realize;
    ResettablePhases parent_phases;
};

struct AppleSEPState {
    /*< private >*/
    AppleA7IOP parent_obj;

    /*< public >*/
    vaddr base;
    ARMCPU *cpu;
    bool modern;
    MemoryRegion *ool_mr;
    AddressSpace *ool_as;
    MemoryRegion pmgr_base_mr;
    MemoryRegion trng_regs_mr;
    MemoryRegion key_base_mr;
    MemoryRegion key_fcfg_mr;
    MemoryRegion moni_base_mr;
    MemoryRegion moni_thrm_mr;
    MemoryRegion eisp_base_mr;
    MemoryRegion eisp_hmac_mr;
    MemoryRegion aess_base_mr;
    MemoryRegion aesh_base_mr;
    MemoryRegion pka_base_mr;
    MemoryRegion pka_tmm_mr;
    MemoryRegion misc0_mr;
    MemoryRegion misc2_mr;
    MemoryRegion progress_mr;
    MemoryRegion boot_monitor_mr;
    MemoryRegion debug_trace_mr;
    uint8_t pmgr_base_regs[PMGR_BASE_REG_SIZE];
    uint8_t key_base_regs[KEY_BASE_REG_SIZE];
    uint8_t key_fcfg_regs[KEY_FCFG_REG_SIZE];
    uint8_t moni_base_regs[MONI_BASE_REG_SIZE];
    uint8_t moni_thrm_regs[MONI_THRM_REG_SIZE];
    uint8_t eisp_base_regs[EISP_BASE_REG_SIZE];
    uint8_t eisp_hmac_regs[EISP_HMAC_REG_SIZE];
    uint8_t aess_base_regs[AESS_BASE_REG_SIZE];
    uint8_t aesh_base_regs[AESH_BASE_REG_SIZE];
    uint8_t pka_base_regs[PKA_BASE_REG_SIZE];
    uint8_t pka_tmm_regs[PKA_TMM_REG_SIZE];
    uint8_t misc0_regs[MISC0_REG_SIZE];
    uint8_t misc2_regs[MISC2_REG_SIZE];
    uint8_t progress_regs[PROGRESS_REG_SIZE];
    uint8_t boot_monitor_regs[PROGRESS_REG_SIZE];
    uint8_t debug_trace_regs[DEBUG_TRACE_SIZE]; // 0x10000
    QEMUTimer *timer;
    AppleTRNGState trng_state;
    AppleAESSState aess_state;
    ApplePKAState pka_state;
    DeviceState *fiq_or;
    DeviceState *irq_or;
    EEPROMState *eeprom0;
    AppleSSCState *ssc_state;
    hwaddr sep_fw_addr;
    uint64_t sep_fw_size;
    uint32_t chip_id;
    hwaddr shmbuf_base;
    hwaddr trace_buffer_base_offset;
    hwaddr debug_trace_size;
    gchar *sepfw_data;
    bool pmgr_fuse_changer_bit0_was_set;
    bool pmgr_fuse_changer_bit1_was_set;
    uint8_t key_fcfg_offset_0x14_index;
    uint16_t key_fcfg_offset_0x14_values[5];
};

AppleSEPState *apple_sep_create(DTBNode *node, MemoryRegion *ool_mr, vaddr base,
                                uint32_t cpu_id, uint32_t build_version,
                                bool modern, uint32_t chip_id);

AppleSSCState *apple_ssc_create(MachineState *machine, uint8_t addr);

void enable_trace_buffer(AppleSEPState *s);

#endif /* HW_ARM_APPLE_SILICON_SEP_H */
