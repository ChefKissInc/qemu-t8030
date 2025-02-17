#ifndef HW_MISC_APPLE_SILICON_AES_REG_H
#define HW_MISC_APPLE_SILICON_AES_REG_H

#include "qemu/osdep.h"

#pragma pack(push, 1)

#define REG_AES_VERSION (0x0)
#define REG_AES_CONFIG (0x4)
#define REG_AES_CONTROL (0x8)
#define AES_BLK_CONTROL_START (1 << 0)
#define AES_BLK_CONTROL_STOP (1 << 1)
#define AES_BLK_CONTROL_RESET (1 << 2)
#define AES_BLK_CONTROL_RESET_AES (1 << 3)

#define REG_AES_STATUS (0xc)
#define AES_BLK_STATUS_DMA_READ_ACTIVE (1 << 0)
#define AES_BLK_STATUS_DMA_READ_INCOMPLETE (1 << 1)
#define AES_BLK_STATUS_DMA_WRITE_ACTIVE (1 << 2)
#define AES_BLK_STATUS_DMA_WRITE_INCOMPLETE (1 << 3)
#define AES_BLK_STATUS_ACTIVE (1 << 4)
#define AES_BLK_STATUS_COMMAND_FIFO_ACTIVE (1 << 5)
#define AES_BLK_STATUS_COMMAND_FIFO_ENABLED (1 << 6)
#define AES_BLK_STATUS_TEXT_DPA_RANDOM_SEEDED (1 << 7)
#define AES_BLK_STATUS_KEY_UNWRAP_DPA_RANDOM_SEEDED (1 << 8)

#define REG_AES_KEY_ID (0x10)
#define AES_BLK_KEY_ID_CONTEXT_0_SHIFT (0)
#define AES_BLK_KEY_ID_CONTEXT_0_WIDTH (8)
#define AES_BLK_KEY_ID_CONTEXT_0_UMASK (0xff)
#define AES_BLK_KEY_ID_CONTEXT_0_SMASK (0xff)
#define AES_BLK_KEY_ID_CONTEXT_0_XTRCT(r) ((((uint32_t)r) >> 0) & 0xff)
#define AES_BLK_KEY_ID_CONTEXT_0_INSRT(f) ((((uint32_t)f) & 0xff) << 0)
#define AES_BLK_KEY_ID_CONTEXT_1_SHIFT (8)
#define AES_BLK_KEY_ID_CONTEXT_1_WIDTH (8)
#define AES_BLK_KEY_ID_CONTEXT_1_UMASK (0xff00)
#define AES_BLK_KEY_ID_CONTEXT_1_SMASK (0xff)
#define AES_BLK_KEY_ID_CONTEXT_1_XTRCT(r) ((((uint32_t)r) >> 8) & 0xff)
#define AES_BLK_KEY_ID_CONTEXT_1_INSRT(f) ((((uint32_t)f) & 0xff) << 8)

#define REG_AES_AXI_STATUS (0x14)

#define REG_AES_INT_STATUS (0x18)
#define REG_AES_INT_ENABLE (0x1c)
#define AES_BLK_INT_COMMAND_FIFO_LOW (1 << 0)
#define AES_BLK_INT_COMMAND_FIFO_OVERFLOW (1 << 1)
#define AES_BLK_INT_INVALID_COMMAND (1 << 2)
#define AES_BLK_INT_AXI_READ_RESPONSE_NOT_OKAY (1 << 3)
#define AES_BLK_INT_AXI_WRITE_RESPONSE_NOT_OKAY (1 << 4)
#define AES_BLK_INT_FLAG_COMMAND (1 << 5)
#define AES_BLK_INT_INVALID_DATA_LENGTH (1 << 6)
#define AES_BLK_INT_KEY_0_DISABLED (1 << 7)
#define AES_BLK_INT_KEY_0_CMAC_TAG_MISMATCH (1 << 8)
#define AES_BLK_INT_KEY_0_POLICY_MISMATCH (1 << 9)
#define AES_BLK_INT_KEY_0_INVALID_AT_USE_TIME (1 << 10)
#define AES_BLK_INT_KEY_0_HDCP_ERR (1 << 11)
#define AES_BLK_INT_KEY_0_DPA_RANDOM_UNSEEDED (1 << 12)
#define AES_BLK_INT_KEY_1_DISABLED (1 << 13)
#define AES_BLK_INT_KEY_1_CMAC_TAG_MISMATCH (1 << 14)
#define AES_BLK_INT_KEY_1_POLICY_MISMATCH (1 << 15)
#define AES_BLK_INT_KEY_1_INVALID_AT_USE_TIME (1 << 16)
#define AES_BLK_INT_KEY_1_HDCP_ERR (1 << 17)
#define AES_BLK_INT_KEY_1_DPA_RANDOM_UNSEEDED (1 << 18)

#define REG_AES_WATERMARKS (0x20)
#define AES_BLK_WATERMARKS_COMMAND_FIFO_LOW_SHIFT (0)
#define AES_BLK_WATERMARKS_COMMAND_FIFO_LOW_WIDTH (7)
#define AES_BLK_WATERMARKS_COMMAND_FIFO_LOW_UMASK (0x7f)
#define AES_BLK_WATERMARKS_COMMAND_FIFO_LOW_SMASK (0x7f)
#define AES_BLK_WATERMARKS_COMMAND_FIFO_LOW_XTRCT(r) \
    ((((uint32_t)r) >> 0) & 0x7f)
#define AES_BLK_WATERMARKS_COMMAND_FIFO_LOW_INSRT(f) \
    ((((uint32_t)f) & 0x7f) << 0)

#define REG_AES_COMMAND_FIFO_STATUS (0x24)
#define AES_BLK_COMMAND_FIFO_STATUS_LOW (1 << 0)
#define AES_BLK_COMMAND_FIFO_STATUS_EMPTY (1 << 1)
#define AES_BLK_COMMAND_FIFO_STATUS_FULL (1 << 2)
#define AES_BLK_COMMAND_FIFO_STATUS_OVERFLOW (1 << 3)
#define AES_BLK_COMMAND_FIFO_STATUS_LEVEL_SHIFT (8)
#define AES_BLK_COMMAND_FIFO_STATUS_LEVEL_WIDTH (8)
#define AES_BLK_COMMAND_FIFO_STATUS_LEVEL_UMASK (0xff00)
#define AES_BLK_COMMAND_FIFO_STATUS_LEVEL_SMASK (0xff)
#define AES_BLK_COMMAND_FIFO_STATUS_LEVEL_XTRCT(r) ((((uint32_t)r) >> 8) & 0xff)
#define AES_BLK_COMMAND_FIFO_STATUS_LEVEL_INSRT(f) ((((uint32_t)f) & 0xff) << 8)
#define AES_BLK_COMMAND_FIFO_STATUS_READ_POINTER_SHIFT (16)
#define AES_BLK_COMMAND_FIFO_STATUS_READ_POINTER_WIDTH (7)
#define AES_BLK_COMMAND_FIFO_STATUS_READ_POINTER_UMASK (0x7f0000)
#define AES_BLK_COMMAND_FIFO_STATUS_READ_POINTER_SMASK (0x7f)
#define AES_BLK_COMMAND_FIFO_STATUS_READ_POINTER_XTRCT(r) \
    ((((uint32_t)r) >> 16) & 0x7f)
#define AES_BLK_COMMAND_FIFO_STATUS_READ_POINTER_INSRT(f) \
    ((((uint32_t)f) & 0x7f) << 16)
#define AES_BLK_COMMAND_FIFO_STATUS_WRITE_POINTER_SHIFT (24)
#define AES_BLK_COMMAND_FIFO_STATUS_WRITE_POINTER_WIDTH (7)
#define AES_BLK_COMMAND_FIFO_STATUS_WRITE_POINTER_UMASK (0x7f000000)
#define AES_BLK_COMMAND_FIFO_STATUS_WRITE_POINTER_SMASK (0x7f)
#define AES_BLK_COMMAND_FIFO_STATUS_WRITE_POINTER_XTRCT(r) \
    ((((uint32_t)r) >> 24) & 0x7f)
#define AES_BLK_COMMAND_FIFO_STATUS_WRITE_POINTER_INSRT(f) \
    ((((uint32_t)f) & 0x7f) << 24)

#define REG_AES_COMMAND_FIFO_COUNT (0x2c)
#define AES_BLK_COMMAND_FIFO_COUNT_TOTAL_SHIFT (0)
#define AES_BLK_COMMAND_FIFO_COUNT_TOTAL_WIDTH (32)
#define AES_BLK_COMMAND_FIFO_COUNT_TOTAL_UMASK (0xffffffff)
#define AES_BLK_COMMAND_FIFO_COUNT_TOTAL_SMASK (0xffffffff)
#define AES_BLK_COMMAND_FIFO_COUNT_TOTAL_XTRCT(r) \
    ((((uint32_t)r) >> 0) & 0xffffffff)
#define AES_BLK_COMMAND_FIFO_COUNT_TOTAL_INSRT(f) \
    ((((uint32_t)f) & 0xffffffff) << 0)

#define REG_AES_FLAG_COMMAND (0x30)
#define AES_BLK_FLAG_COMMAND_CODE_SHIFT (0)
#define AES_BLK_FLAG_COMMAND_CODE_WIDTH (16)
#define AES_BLK_FLAG_COMMAND_CODE_UMASK (0xffff)
#define AES_BLK_FLAG_COMMAND_CODE_SMASK (0xffff)
#define AES_BLK_FLAG_COMMAND_CODE_XTRCT(r) ((((uint32_t)r) >> 0) & 0xffff)
#define AES_BLK_FLAG_COMMAND_CODE_INSRT(f) ((((uint32_t)f) & 0xffff) << 0)

#define REG_AES_SKG_KEY (0x34)

#define REG_AES_CLEAR_FIFO (0x38)
#define REG_AES_CLEAR_FIFO_RESET (1 << 0)

#define REG_AES_COMMAND_FIFO_S8000 (0x100)
#define REG_AES_COMMAND_FIFO (0x200)

#define REG_AES_HISTORY_FIFO (0x400)

#define COMMAND_FIFO_SIZE 128

typedef enum {
    BLOCK_MODE_ECB = 0,
    BLOCK_MODE_CBC = 1,
    BLOCK_MODE_CTR = 2,
} block_mode_t;

typedef enum {
    KEY_LEN_128 = 0,
    KEY_LEN_192 = 1,
    KEY_LEN_256 = 2,
} key_len_t;

typedef enum {
    KEY_SELECT_SOFTWARE = 0,
    KEY_SELECT_UID1 = 1,
    KEY_SELECT_GID_AP_1 = 2,
    KEY_SELECT_GID_AP_2 = 3,
    KEY_SELECT_HDCP_0 = 4,
    KEY_SELECT_HDCP_1 = 5,
    KEY_SELECT_HDCP_2 = 6,
    KEY_SELECT_HDCP_3 = 7,
} key_select_t;

typedef enum {
    KEY_FUNC_NONE = 0,
    KEY_FUNC_LEGACY = 1,
    KEY_FUNC_FAIRPLAY_LEGACY = 2,
    KEY_FUNC_FAIRPLAY_H8F = 3,
} key_func_t;

typedef enum {
    OPCODE_KEY = 0x01,
    OPCODE_IV = 0x02,
    OPCODE_DSB = 0x03,
    OPCODE_SKG = 0x04,
    OPCODE_DATA = 0x05,
    OPCODE_STORE_IV = 0x06,
    OPCODE_WRITE_REG = 0x07,
    OPCODE_FLAG = 0x08,
} command_opcodes_t;

#define COMMAND_OPCODE_SHIFT (28)
#define COMMAND_OPCODE_MASK (0xF)
#define COMMAND_OPCODE(_x) \
    (((_x) >> COMMAND_OPCODE_SHIFT) & COMMAND_OPCODE_MASK)

#define COMMAND_KEY_COMMAND_KEY_CONTEXT(_x) ((_x >> 27) & 0x1)

#define COMMAND_KEY_COMMAND_KEY_SELECT_SHIFT (24)
#define COMMAND_KEY_COMMAND_KEY_SELECT_MASK (0x7)
#define COMMAND_KEY_COMMAND_KEY_SELECT(_x)            \
    (((_x) >> COMMAND_KEY_COMMAND_KEY_SELECT_SHIFT) & \
     COMMAND_KEY_COMMAND_KEY_SELECT_MASK)

#define COMMAND_KEY_COMMAND_KEY_LENGTH_SHIFT (22)
#define COMMAND_KEY_COMMAND_KEY_LENGTH_MASK (0x3)
#define COMMAND_KEY_COMMAND_KEY_LENGTH(_x)            \
    (((_x) >> COMMAND_KEY_COMMAND_KEY_LENGTH_SHIFT) & \
     COMMAND_KEY_COMMAND_KEY_LENGTH_MASK)

#define COMMAND_KEY_COMMAND_WRAPPED (1 << 21)

#define COMMAND_KEY_COMMAND_ENCRYPT (1 << 20)

#define COMMAND_KEY_COMMAND_KEY_FUNC_SHIFT (18)
#define COMMAND_KEY_COMMAND_KEY_FUNC_MASK (0x3)
#define COMMAND_KEY_COMMAND_KEY_FUNC(_x) (((_x) >> 18) & 0x3)

#define COMMAND_KEY_COMMAND_BLOCK_MODE_SHIFT (16)
#define COMMAND_KEY_COMMAND_BLOCK_MODE_MASK (0x3)
#define COMMAND_KEY_COMMAND_BLOCK_MODE(_x) (((_x) >> 16) & 0x3)

#define COMMAND_KEY_COMMAND_COMMAND_ID_SHIFT (0)
#define COMMAND_KEY_COMMAND_COMMAND_ID_MASK (0xFF)
#define COMMAND_KEY_COMMAND_COMMAND_ID(_x) (((_x) >> 0) & 0xFF)

typedef struct command_key {
    uint32_t command;
    uint32_t key[8];
} command_key_t;

#define COMMAND_IV_COMMAND_IV_CONTEXT_SHIFT (26)
#define COMMAND_IV_COMMAND_IV_CONTEXT_MASK (0x3)
#define COMMAND_IV_COMMAND_IV_CONTEXT(_x) (((_x) >> 26) & 0x3)

#define COMMAND_IV_COMMAND_HDCP_KEY_SHIFT (25)
#define COMMAND_IV_COMMAND_HDCP_KEY_MASK (0x1)

#define COMMAND_IV_COMMAND_IV_IN_HDCP_SHIFT (23)
#define COMMAND_IV_COMMAND_IV_IN_HDCP_MASK (0x3)

typedef struct aes_command_iv {
    uint32_t command;
    uint32_t iv[4];
} command_iv_t;

typedef struct aes_command_dsb {
    uint32_t command;
    uint32_t r1_in[4];
    uint32_t r2_in[4];
} command_dsb_t;

#define COMMAND_DATA_COMMAND_KEY_CONTEXT_SHIFT (27)
#define COMMAND_DATA_COMMAND_KEY_CONTEXT_MASK (0x1)
#define COMMAND_DATA_COMMAND_KEY_CONTEXT(_x) (((_x) >> 27) & 0x1)

#define COMMAND_DATA_COMMAND_IV_CONTEXT_SHIFT (25)
#define COMMAND_DATA_COMMAND_IV_CONTEXT_MASK (0x3)
#define COMMAND_DATA_COMMAND_IV_CONTEXT(_x) (((_x) >> 25) & 0x3)

#define COMMAND_DATA_COMMAND_LENGTH_SHIFT (0)
#define COMMAND_DATA_COMMAND_LENGTH_MASK (0xFFFFFF)
#define COMMAND_DATA_COMMAND_LENGTH(_x) (((_x) >> 0) & 0xFFFFFF)
#define COMMAND_DATA_UPPER_ADDR_SOURCE_SHIFT (16)
#define COMMAND_DATA_UPPER_ADDR_SOURCE_MASK (0xFF)
#define COMMAND_DATA_UPPER_ADDR_SOURCE(_x) (((_x) >> 16) & 0xFF)

#define COMMAND_DATA_UPPER_ADDR_DEST_SHIFT (0)
#define COMMAND_DATA_UPPER_ADDR_DEST_MASK (0xFF)
#define COMMAND_DATA_UPPER_ADDR_DEST(_x) (((_x) >> 0) & 0xFF)

typedef struct aes_command_data {
    uint32_t command;
    uint32_t upper_addr;
    uint32_t source_addr;
    uint32_t dest_addr;
} command_data_t;

#define COMMAND_STORE_IV_COMMAND_CONTEXT_SHIFT (26)
#define COMMAND_STORE_IV_COMMAND_CONTEXT_MASK (0x3)
#define COMMAND_STORE_IV_COMMAND_CONTEXT(_x) (((_x) >> 26) & 0x3)

#define COMMAND_STORE_IV_COMMAND_UPPER_ADDR_DEST_SHIFT (0)
#define COMMAND_STORE_IV_COMMAND_UPPER_ADDR_DEST_MASK (0xFF)
#define COMMAND_STORE_IV_COMMAND_UPPER_ADDR_DEST(_x) (((_x) >> 0) & 0xFF)

typedef struct aes_command_store_iv {
    uint32_t command;
    uint32_t dest_addr;
} command_store_iv_t;

#define COMMAND_FLAG_ID_CODE_SHIFT (0)
#define COMMAND_FLAG_ID_CODE_MASK (0xFF)
#define COMMAND_FLAG_ID_CODE(_x) \
    (((_x) >> COMMAND_FLAG_ID_CODE_SHIFT) & COMMAND_FLAG_ID_CODE_MASK)
#define COMMAND_FLAG_STOP_COMMANDS (1 << 26)
#define COMMAND_FLAG_SEND_INTERRUPT (1 << 27)

typedef union {
    uint32_t raw;
    struct {
        uint32_t minor_release : 8;
        uint32_t major_release : 8;
        uint32_t fld : 8;
        uint32_t rsvd0 : 8;
    };
} aes_blk_version_t;

typedef union {
    uint32_t raw;
    struct {
        uint32_t axi_aruser : 4;
        uint32_t axi_arcache : 4;
        uint32_t axi_awuser : 4;
        uint32_t axi_awcache : 4;
        uint32_t burst_size : 2;
        uint32_t rsvd0 : 14;
    };
} aes_blk_config_t;

typedef union {
    uint32_t raw;
    struct {
        uint32_t start : 1;
        uint32_t stop : 1;
        uint32_t reset : 1;
        uint32_t reset_aes : 1;
        uint32_t reset_read_channel : 1;
        uint32_t reset_write_channel : 1;
        uint32_t rsvd0 : 26;
    };
} aes_blk_control_t;

typedef union {
    uint32_t raw;
    struct {
        uint32_t dma_read_active : 1;
        uint32_t dma_read_incomplete : 1;
        uint32_t dma_write_active : 1;
        uint32_t dma_write_incomplete : 1;
        uint32_t active : 1;
        uint32_t command_fifo_active : 1;
        uint32_t command_fifo_enabled : 1;
        uint32_t text_dpa_random_seeded : 1;
        uint32_t key_unwrap_dpa_random_seeded : 1;
        uint32_t dpa_seeded_unk0 : 1;
        uint32_t dpa_seeded_unk1 : 1;
        uint32_t dpa_seeded_unk2 : 1;
        uint32_t dpa_seeded_unk3 : 1;
        uint32_t dpa_seeded_unk4 : 1;
        uint32_t dpa_seeded_unk5 : 1;
        uint32_t rsvd0 : 17;
    };
} aes_blk_status_t;

typedef union {
    uint32_t raw;
    struct {
        uint32_t context_0 : 8;
        uint32_t context_1 : 8;
        uint32_t rsvd0 : 16;
    };
} aes_blk_key_id_t;

typedef union {
    uint32_t raw;
    struct {
        uint32_t read_response : 2;
        uint32_t write_response : 2;
        uint32_t rsvd0 : 28;
    };
} aes_blk_axi_status_t;

typedef union {
    uint32_t raw;
    struct {
        uint32_t command_fifo_low : 1;
        uint32_t command_fifo_overflow : 1;
        uint32_t invalid_command : 1;
        uint32_t axi_read_response_not_okay : 1;
        uint32_t axi_write_response_not_okay : 1;
        uint32_t flag_command : 1;
        uint32_t invalid_data_length : 1;
        uint32_t key_0_disabled : 1;
        uint32_t key_0_cmac_tag_mismatch : 1;
        uint32_t key_0_policy_mismatch : 1;
        uint32_t key_0_invalid_at_use_time : 1;
        uint32_t key_0_hdcp_err : 1;
        uint32_t key_0_dpa_random_unseeded : 1;
        uint32_t key_1_disabled : 1;
        uint32_t key_1_cmac_tag_mismatch : 1;
        uint32_t key_1_policy_mismatch : 1;
        uint32_t key_1_invalid_at_use_time : 1;
        uint32_t key_1_hdcp_err : 1;
        uint32_t key_1_dpa_random_unseeded : 1;
        uint32_t rsvd0 : 13;
    };
} aes_blk_int_status_t;

typedef union {
    uint32_t raw;
    struct {
        uint32_t command_fifo_low : 1;
        uint32_t command_fifo_overflow : 1;
        uint32_t invalid_command : 1;
        uint32_t axi_read_response_not_okay : 1;
        uint32_t axi_write_response_not_okay : 1;
        uint32_t flag_command : 1;
        uint32_t invalid_data_length : 1;
        uint32_t key_0_disabled : 1;
        uint32_t key_0_cmac_tag_mismatch : 1;
        uint32_t key_0_policy_mismatch : 1;
        uint32_t key_0_invalid_at_use_time : 1;
        uint32_t key_0_hdcp_err : 1;
        uint32_t key_0_dpa_random_unseeded : 1;
        uint32_t key_1_disabled : 1;
        uint32_t key_1_cmac_tag_mismatch : 1;
        uint32_t key_1_policy_mismatch : 1;
        uint32_t key_1_invalid_at_use_time : 1;
        uint32_t key_1_hdcp_err : 1;
        uint32_t key_1_dpa_random_unseeded : 1;
        uint32_t rsvd0 : 13;
    };
} aes_blk_int_enable_t;

typedef union {
    uint32_t raw;
    struct {
        uint32_t command_fifo_low : 7;
        uint32_t rsvd0 : 25;
    };
} aes_blk_watermarks_t;

typedef union {
    uint32_t raw;
    struct {
        uint32_t low : 1;
        uint32_t empty : 1;
        uint32_t full : 1;
        uint32_t overflow : 1;
        uint32_t rsvd0 : 4;
        uint32_t level : 8;
        uint32_t read_pointer : 7;
        uint32_t rsvd1 : 1;
        uint32_t write_pointer : 7;
        uint32_t rsvd2 : 1;
    };
} aes_blk_command_fifo_status_t;

typedef union {
    uint32_t raw;
    struct {
        uint32_t total : 32;
    };
} aes_blk_command_fifo_count_t;

typedef union {
    uint32_t raw;
    struct {
        uint16_t code;
        uint16_t rsvd0;
    };
} aes_blk_flag_command_t;

typedef union {
    uint32_t raw;
    struct {
        uint32_t count : 32;
    };
} aes_blk_skg_key_t;

#define AES_BLK_REG_SIZE (0x204)

typedef union {
    uint32_t raw[AES_BLK_REG_SIZE / sizeof(uint32_t)];
    struct {
        aes_blk_version_t version;
        aes_blk_config_t config;
        aes_blk_control_t control;
        aes_blk_status_t status;
        aes_blk_key_id_t key_id;
        aes_blk_axi_status_t axi_status;
        aes_blk_int_status_t int_status;
        aes_blk_int_enable_t int_enable;
        aes_blk_watermarks_t watermarks;
        aes_blk_command_fifo_status_t command_fifo_status;
        uint32_t history_fifo_status;
        aes_blk_command_fifo_count_t command_fifo_count;
        aes_blk_flag_command_t flag_command;
        aes_blk_skg_key_t skg_key;
        uint32_t clear_fifos;
        uint8_t rsvd0[452];
        uint32_t command_fifo;
    };
} aes_reg_t;

#pragma pack(pop)

#endif /* HW_MISC_APPLE_SILICON_AES_REG_H */
