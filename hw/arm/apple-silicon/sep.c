/*
 * Apple SEP.
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
#include "crypto/cipher.h"
#include "crypto/hmac.h"
#include "crypto/random.h"
#include "exec/address-spaces.h"
#include "hw/arm/apple-silicon/a13.h"
#include "hw/arm/apple-silicon/a9.h"
#include "hw/arm/apple-silicon/mem.h"
#include "hw/arm/apple-silicon/sep.h"
#include "hw/boards.h"
#include "hw/core/cpu.h"
#include "hw/gpio/apple_gpio.h"
#include "hw/i2c/apple_i2c.h"
#include "hw/irq.h"
#include "hw/misc/apple-silicon/a7iop/core.h"
#include "hw/misc/apple-silicon/a7iop/private.h"
#include "hw/or-irq.h"
#include "hw/qdev-properties-system.h"
#include "hw/qdev-properties.h"
#include "hw/resettable.h"
#include "qapi/error.h"
#include "qemu/crc-ccitt.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qom/object.h"
#include "sysemu/block-backend-global-state.h"
#include "sysemu/block-backend-io.h"
#include "target/arm/cpregs.h"
#include "nettle/ccm.h"
#include "nettle/cmac.h"
#include "nettle/ecc-curve.h"
#include "nettle/ecc.h"
#include "nettle/ecdsa.h"
#include "nettle/hkdf.h"
#include "nettle/hmac.h"
#include "nettle/knuth-lfib.h"
#include <nettle/macros.h>
#include <nettle/memxor.h>
#include "trace.h"

// #define SEP_DEBUG

#ifdef SEP_DEBUG
#define HEXDUMP(a, b, c) qemu_hexdump(stderr, a, b, c)
#define DBGLOG(v, ...) fprintf(stderr, v, ##__VA_ARGS__)
#else
#define HEXDUMP(a, b, c) \
    do {                 \
    } while (0)
#define DBGLOG(v, ...) \
    do {               \
    } while (0)
#endif

// #define ENABLE_CPU_DUMP_STATE

#define SEP_ENABLE_HARDCODED_FIRMWARE
#define SEP_ENABLE_DEBUG_TRACE_MAPPING
#define SEP_ENABLE_TRACE_BUFFER
// can cause conflicts with kernel and userspace, not anymore?
#define SEP_ENABLE_OVERWRITE_SHMBUF_OBJECTS
#define SEP_DISABLE_ASLR

#define SEP_USE_IOS14_OVERRIDE

#define SEP_AESS_CMD_FLAG_KEYSIZE_AES128 0x0
#define SEP_AESS_CMD_FLAG_KEYSIZE_AES192 0x100
#define SEP_AESS_CMD_FLAG_KEYSIZE_AES256 0x200

#define SEP_AESS_CMD_FLAG_KEYSELECT_GID0_T8010 0x00 // ???
#define SEP_AESS_CMD_FLAG_KEYSELECT_GID1_T8010 0x10 // ???
#define SEP_AESS_CMD_FLAG_KEYSELECT_CUSTOM_T8010 0x20 // ???
#define SEP_AESS_CMD_FLAG_UNKNOWN0_T8010 0x00 // ???

#define SEP_AESS_CMD_FLAG_KEYSELECT_GID0_T8020 0x00 // also for T8015
#define SEP_AESS_CMD_FLAG_KEYSELECT_GID1_T8020 0x40 // also for T8015
// Also for T8015, this (custom) takes precedence over the other
// keyselect flags
#define SEP_AESS_CMD_FLAG_KEYSELECT_CUSTOM_T8020 0x80
#define SEP_AESS_CMD_FLAG_UNKNOWN0_T8020 0x10
#define SEP_AESS_CMD_FLAG_UNKNOWN1_T8020 0x20

#define SEP_AESS_CMD_FLAG_UNKNOWN0 SEP_AESS_CMD_FLAG_UNKNOWN0_T8020
#define SEP_AESS_CMD_FLAG_UNKNOWN1 SEP_AESS_CMD_FLAG_UNKNOWN1_T8020

#define SEP_AESS_CMD_FLAG_KEYSELECT_GID0 SEP_AESS_CMD_FLAG_KEYSELECT_GID0_T8020
#define SEP_AESS_CMD_FLAG_KEYSELECT_GID1 SEP_AESS_CMD_FLAG_KEYSELECT_GID1_T8020
#define SEP_AESS_CMD_FLAG_KEYSELECT_CUSTOM \
    SEP_AESS_CMD_FLAG_KEYSELECT_CUSTOM_T8020


#define SEP_AESS_CMD_WITHOUT_KEYSIZE(cmd)                                    \
    (cmd &                                                                   \
     ~(SEP_AESS_CMD_FLAG_KEYSIZE_AES256 | SEP_AESS_CMD_FLAG_KEYSIZE_AES192 | \
       SEP_AESS_CMD_FLAG_KEYSIZE_AES128))
#define SEP_AESS_CMD_WITHOUT_FLAGS(cmd)                                      \
    (SEP_AESS_CMD_WITHOUT_KEYSIZE(cmd) &                                     \
     ~(SEP_AESS_CMD_FLAG_KEYSELECT_GID0 | SEP_AESS_CMD_FLAG_KEYSELECT_GID1 | \
       SEP_AESS_CMD_FLAG_KEYSELECT_CUSTOM))
// #define SEP_AESS_CMD_WITHOUT_FLAGS(cmd) (cmd &
// ~(SEP_AESS_CMD_FLAG_KEYSIZE_AES256 | SEP_AESS_CMD_FLAG_KEYSIZE_AES192 |
// SEP_AESS_CMD_FLAG_UNKNOWN0))
////#define SEP_AESS_CMD_WITHOUT_FLAGS(cmd) (cmd &
///~(SEP_AESS_CMD_FLAG_KEYSIZE_AES256 | SEP_AESS_CMD_FLAG_KEYSIZE_AES192 |
/// SEP_AESS_CMD_FLAG_UNKNOWN0 | SEP_AESS_CMD_FLAG_UNKNOWN1))

#define SEP_AESS_CMD_FLAG_KEYSELECT_GID1_CUSTOM(cmd) \
    (cmd &                                           \
     (SEP_AESS_CMD_FLAG_KEYSELECT_GID1 | SEP_AESS_CMD_FLAG_KEYSELECT_CUSTOM))

#define SEP_AESS_COMMAND_SYNC_SEEDBITS 0x0 // sync with register_seed_bits?
#define SEP_AESS_COMMAND_ENCRYPT_CBC_ONLY_NONCUSTOM_FORCE_CUSTOM_AES256 \
    0x6 // forces and overwrites flags, aes256 && custom. do nothing if the
        // custom flag was set.
#define SEP_AESS_COMMAND_ENCRYPT_CBC_FORCE_CUSTOM_AES256 \
    0x8 // forces and overwrites flags, aes256 && custom.
#define SEP_AESS_COMMAND_ENCRYPT_CBC 0x9
#define SEP_AESS_COMMAND_DECRYPT_CBC 0xa
#define SEP_AESS_COMMAND_0xb 0xb // custom aes key?


#define SEP_AESS_REGISTER_STATUS 0x4
#define SEP_AESS_REGISTER_COMMAND 0x8
#define SEP_AESS_REGISTER_INTERRUPT_STATUS 0xc
#define SEP_AESS_REGISTER_INTERRUPT_ENABLED 0x10
#define SEP_AESS_REGISTER_0x14_KEYWRAP_ITERATIONS_COUNTER 0x14
#define SEP_AESS_REGISTER_0x18_KEYDISABLE 0x18
#define SEP_AESS_REGISTER_SEED_BITS 0x1c
#define SEP_AESS_REGISTER_SEED_BITS_LOCK 0x20
#define SEP_AESS_REGISTER_IV 0x40
#define SEP_AESS_REGISTER_IN 0x50
#define SEP_AESS_REGISTER_TAG_OUT 0x60
#define SEP_AESS_REGISTER_OUT 0x70

#define SEP_AESS_REGISTER_STATUS_RUN_COMMAND 0x1
#define SEP_AESS_REGISTER_INTERRUPT_STATUS_UNRECOVERABLE_ERROR_INTERRUPT 0x2

#define SEP_AESS_SEED_BITS_BIT0 (1 << 0)
#define SEP_AESS_SEED_BITS_BIT27 (1 << 27) // cmds 0x50 and 0x90
#define SEP_AESS_SEED_BITS_BIT28 (1 << 28) // invalid EKEY?
#define SEP_AESS_SEED_BITS_SEP_DSEC_DEMOTED (1 << 29) // DSEC tag present, demote SEP
#define SEP_AESS_SEED_BITS_AP_PROD_DEMOTED (1 << 30) // ap is prod-fused and demoted
#define SEP_AESS_SEED_BITS_IMG4_VERIFIED (1 << 31) // img4 verified?


// static uint32_t AESS_UID[0x20 / 4] = {0xdeadbeef, 0x13371337, 0xa55a5aa5,
// 0xcafecafe, 0xc4f3c4f3, 0xd34db33f, 0x73317331, 0x5aa5a55a};
static uint32_t AESS_UID0[0x20 / 4] = { 0xdeadbeef, 0x13370000, 0xa55a0000,
                                        0xcafecafe, 0xc4f3c4f3, 0xd34db33f,
                                        0xff317331, 0xffa50000 };
static uint32_t AESS_UID1[0x20 / 4] = { 0xdeadbeef, 0x13371111, 0xa55a1111,
                                        0xcafecafe, 0xc4f3c4f3, 0xd34db33f,
                                        0xff317331, 0xffa50000 };
static uint32_t AESS_GID0[0x20 / 4] = { 0xdeadbe00, 0x13371337, 0xa55a5aa5,
                                        0xcafeca00, 0xc4f3c400, 0xd34db33f,
                                        0x73317331, 0x5aa5a500 };
static uint32_t AESS_GID1[0x20 / 4] = { 0xdeadbe11, 0x13371337, 0xa55a5aa5,
                                        0xcafeca11, 0xc4f3c411, 0xd34db33f,
                                        0x73317331, 0x5aa5a511 };
static uint32_t AESS_KEY_FOR_DISABLED_KEY[0x20 / 4] = {
    0xf00ff00f, 0xf00ff00f, 0xf00ff00f, 0xcafeca33,
    0xc4f3c488, 0xd34db33f, 0xf00ff00f, 0xf00ff00f
};
static uint32_t AESS_UID_SEED_NOT_ENABLED[0x20 / 4] = {
    0x0ff00ff0, 0x0ff00ff0, 0x0ff00ff0, 0xcafeca44,
    0xc4f3c499, 0xd34db33f, 0x0ff00ff0, 0x0ff00ff0
};
static uint32_t AESS_UID_SEED_INVALID[0x20 / 4] = { 0x1ff11ff1, 0x1ff11ff1,
                                                    0x1ff11ff1, 0xcafeca55,
                                                    0xc4f3c4aa, 0xd34db33f,
                                                    0x1ff11ff1, 0x1ff11ff1 };

#define SEP_PKA_STATUS_INTERRUPT_0xA 0x1
#define SEP_PKA_STATUS_INTERRUPT_0xB 0x2
#define SEP_PKA_STATUS_INTERRUPT_0xC 0x4


static inline void block16_set(union nettle_block16 *r,
                               const union nettle_block16 *x)
{
    r->u64[0] = x->u64[0];
    r->u64[1] = x->u64[1];
}
static void drbg_ctr_aes256_output(const struct aes256_ctx *key,
                                   union nettle_block16 *V, size_t n,
                                   uint8_t *dst)
{
    for (; n >= AES_BLOCK_SIZE; n -= AES_BLOCK_SIZE, dst += AES_BLOCK_SIZE) {
        INCREMENT(AES_BLOCK_SIZE, V->b);
        aes256_encrypt(key, AES_BLOCK_SIZE, dst, V->b);
    }
    if (n > 0) {
        union nettle_block16 block;

        INCREMENT(AES_BLOCK_SIZE, V->b);
        aes256_encrypt(key, AES_BLOCK_SIZE, block.b, V->b);
        memcpy(dst, block.b, n);
    }
}
static void drbg_ctr_aes256_update(struct aes256_ctx *key,
                                   union nettle_block16 *V,
                                   const uint8_t *provided_data)
{
    union nettle_block16 tmp[3];
    drbg_ctr_aes256_output(key, V, DRBG_CTR_AES256_SEED_SIZE, tmp[0].b);

    if (provided_data)
        memxor(tmp[0].b, provided_data, DRBG_CTR_AES256_SEED_SIZE);

    aes256_set_encrypt_key(key, tmp[0].b);
    block16_set(V, &tmp[2]);
}

static const char *
sepos_return_module_thread_string_t8015(uint64_t module_thread_id)
{
    // base == sepdump02_SEPOS?
    // T8015 thread name/info base 0xffffffe00001a988

    switch (module_thread_id) {
    case 0x0:
        return "SEPOS"; // SEPOS/BOOT, actually BOOT
    case 0x10000:
        return "SEPD";
    case 0x10001:
        return "intr";
    case 0x10002:
        return "XPRT";
    case 0x10003:
        return "PMGR";
    case 0x10004:
        return "AKF";
    case 0x10005:
        return "EP0D";
    case 0x10006:
        return "TRNG";
    case 0x10007:
        return "KEY";
    case 0x10008:
        return "shnd";
    case 0x10009:
        return "ep0";
    case 0x20000:
        return "DAES";
    case 0x20001:
        return "AESS";
    case 0x20002:
        return "AEST";
    case 0x20003:
        return "PKA";
    case 0x30000:
        return "dxio";
    case 0x30001:
        return "GPIO";
    case 0x30002:
        return "I2C";
    case 0x40000:
        return "enti";
    case 0x50000:
        return "sskg";
    case 0x50001:
        return "skgs";
    case 0x50002:
        return "crow";
    case 0x50003:
        return "cro2";
    case 0x60000:
        return "sars";
    case 0x70000:
        return "ARTM";
    case 0x80000:
        return "xART";
    case 0x90000:
        return "scrd";
    case 0xa0000:
        return "pass";
    case 0xb0000:
        return "sks"; // 13
    case 0xb0001:
        return "sksa";
    case 0xc0000:
        return "sbio"; // 14
    case 0xc0001:
        return "SBIO_THREAD"; // thread name missing from array
    case 0xd0000:
        return "sse"; // 15
    default:
        return "Unknown";
    }
}

static const char *
sepos_return_module_thread_string_t8020(uint64_t module_thread_id)
{
    // base == sepdump02_SEPOS?
    // T8020 thread name/info base 0xffffffe00001b1c8

    switch (module_thread_id) {
    case 0x0:
        return "SEPOS";
    case 0x10000:
        return "SEPD";
    case 0x10001:
        return "intr";
    case 0x10002:
        return "XPRT";
    case 0x10003:
        return "PMGR";
    case 0x10004:
        return "AKF";
    case 0x10005:
        return "EP0D";
    case 0x10006:
        return "TRNG";
    case 0x10007:
        return "KEY";
    case 0x10008:
        return "MONI";
    case 0x10009:
        return "EISP";
    case 0x1000a:
        return "shnd";
    case 0x1000b:
        return "ep0";
    case 0x20000:
        return "DAES";
    case 0x20001:
        return "AESS";
    case 0x20002:
        return "AEST";
    case 0x20003:
        return "PKA";
    case 0x30000:
        return "dxio";
    case 0x30001:
        return "GPIO";
    case 0x30002:
        return "I2C";
    case 0x40000:
        return "enti";
    case 0x50000:
        return "sskg";
    case 0x50001:
        return "skgs";
    case 0x50002:
        return "crow";
    case 0x50003:
        return "cro2";
    case 0x60000:
        return "sars";
    case 0x70000:
        return "ARTM";
    case 0x80000:
        return "xART";
    case 0x90000:
        return "eiAp";
    case 0x90001:
        return "EISP";
    case 0x90002:
        return "HWRS";
    case 0x90003:
        return "FDCN";
    case 0x90004:
        return "FIPP";
    case 0x90005:
        return "FPCE";
    case 0x90006:
        return "FPPD";
    case 0x90007:
        return "FDMA";
    case 0x90008:
        return "SHAV";
    case 0x90009:
        return "PROX";
    case 0xa0000:
        return "scrd";
    case 0xb0000:
        return "pass"; // 13
    case 0xc0000:
        return "sks"; // 14
    case 0xc0001:
        return "sksa"; // 14
    case 0xd0000:
        return "sprl";
    case 0xe0000:
        return "sse"; // 16
    case 0xf0000:
        return "hilo";
    default:
        return "Unknown";
    }
}

static const char *sepos_return_module_thread_string(uint32_t chip_id,
                                                     uint64_t module_thread_id)
{
    if (chip_id == 0x8015) {
        return sepos_return_module_thread_string_t8015(module_thread_id);
    } else {
        return sepos_return_module_thread_string_t8020(module_thread_id);
    }
}

static void debug_trace_reg_write(void *opaque, hwaddr addr, uint64_t data,
                                  unsigned size)
{
    AppleSEPState *s = APPLE_SEP(opaque);
    uint32_t offset = 0;
    if (size == 1) {
        // iOS 15 SEPFW workaround against a brief logspam
        return;
    }

#ifdef ENABLE_CPU_DUMP_STATE
    // cpu_dump_state(CPU(s->cpu), stderr, CPU_DUMP_CODE);
#endif

    if (s->shmbuf_base == 0) {
        qemu_log_mask(
            LOG_UNIMP,
            "DEBUG_TRACE: SHMBUF_BASE==NULL: Unknown write at 0x" HWADDR_FMT_plx
            " of value 0x%" PRIx64 " size=%u\n",
            addr, data, size);
        return;
    }

    offset = ((uint32_t *)s->debug_trace_regs)[0x4 / 4];
    offset -= 1;
    offset <<= 6;

    memcpy(&s->debug_trace_regs[addr], &data, size);

    uint32_t addr_mod = addr % 0x40;
    if (addr != 0x40 && // offset register
        addr != 0x04 && // some index
        addr_mod != 0x20 && addr_mod != 0x28 && addr_mod != 0x00 &&
        addr_mod != 0x08 && addr_mod != 0x10 && addr_mod != 0x18 &&
        addr_mod != 0x30) {
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Unknown write at 0x" HWADDR_FMT_plx
                      " of value 0x%" PRIx64 " size=%u offset==0x%08x\n",
                      addr, data, size, offset);
    }

    // Might not include SEPOS output, as it's not initialized like e.g.
    // SEPD.
    if (addr_mod != 0x30) {
        return;
    }

    struct sep_message m = { 0 };
    uint64_t trace_id = *(uint64_t *)&s->debug_trace_regs[addr - 0x30];
    uint64_t arg2 = *(uint64_t *)&s->debug_trace_regs[addr - 0x28];
    uint64_t arg3 = *(uint64_t *)&s->debug_trace_regs[addr - 0x20];
    uint64_t arg4 = *(uint64_t *)&s->debug_trace_regs[addr - 0x18];
    uint64_t arg5 = *(uint64_t *)&s->debug_trace_regs[addr - 0x10];
    uint64_t tid = *(uint64_t *)&s->debug_trace_regs[addr - 0x08];
    uint64_t time = *(uint64_t *)&s->debug_trace_regs[addr - 0x00];
    DBGLOG("\nDEBUG_TRACE: Debug:"
           " 0x%" PRIx64 " 0x%" PRIx64 " 0x%" PRIx64
           " 0x%" PRIx64 " 0x%" PRIx64 " 0x%" PRIx64
           " %" PRIu64 "\n",
           trace_id, arg2, arg3, arg4, arg5, tid, time);
    const char *tid_str = sepos_return_module_thread_string(s->chip_id, tid);
    switch (trace_id) {
    case 0x82000004: { // SEP L4 task switch
        // %s instead of %c%c%c%c because the names will be nullbytes sometimes.
        uint64_t old_taskname = bswap32(arg2);
        uint64_t new_taskname = bswap32(arg4);
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: SEP "
                      "L4 task switch: old task thread name: 0x%02" PRIx64
                      "(%s) old task id: 0x%05" PRIx64
                      " new task thread name: 0x%02" PRIx64 "(%s) "
                      "arg5: 0x%02" PRIx64 "\n", tid, tid_str, arg2,
                      (char*)&old_taskname, arg3, arg4, (char*)&new_taskname,
                      arg5);
        break;
    }
    case 0x82010004: // panic
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: SEP "
                      "module panicked\n",
                      tid, tid_str);
        break;
    case 0x82030004: // initialize_ool_page
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: "
                      "initialize_ool_page:"
                      " obj_id: 0x%02" PRIx64 " address: 0x%02" PRIx64 "\n",
                      tid, tid_str, arg2, arg3);
        break;
    case 0x82040005: // before SEP_IO__Control
    case 0x82040006: // after SEP_IO__Control
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: %s "
                      "SEP_IO__Control Sending message to other module:"
                      " fromto: 0x%02" PRIx64 " method: 0x%02" PRIx64 " data0: 0x%02" PRIx64 " "
                      "data1: 0x%02" PRIx64 "\n",
                      tid, tid_str,
                      (trace_id == 0x82040005) ? "Before" : "After", arg2, arg3,
                      arg4, arg5);
        break;
    case 0x82050005: // SEP_SERVICE__Call: request
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: "
                      "SEP_SERVICE__Call: request:"
                      " fromto: 0x%02" PRIx64 " interface_msgid: 0x%02" PRIx64 " "
                      "method: 0x%02" PRIx64 " data0: 0x%02" PRIx64 "\n",
                      tid, tid_str, arg2, arg3, arg4, arg5);
        break;
    case 0x82050006: // SEP_SERVICE__Call: response
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: "
                      "SEP_SERVICE__Call: response:"
                      " fromto: 0x%02" PRIx64 " interface_msgid: 0x%02" PRIx64 " "
                      "method: 0x%02" PRIx64 " status/data0: 0x%02" PRIx64 "\n",
                      tid, tid_str, arg2, arg3, arg4, arg5);
        break;
    case 0x82060004: // entered workloop function
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: SEP "
                      "module entered workloop function:"
                      " handlers0: 0x%02" PRIx64 " handlers1: 0x%02" PRIx64 " arg5: "
                      "0x%02" PRIx64 " arg6: 0x%02" PRIx64 "\n",
                      tid, tid_str, arg2, arg3, arg4, arg5);
        break;
    case 0x82060010: // workloop function: interface_msgid==0xfffe after
                     // receiving
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: SEP "
                      "module workloop function:"
                      " interface_msgid==0xfffe after receiving: "
                      "data0: 0x%02" PRIx64 "\n",
                      tid, tid_str, arg2);
        break;
    case 0x82060014: // workloop function: before handlers0 handler
        qemu_log_mask(
            LOG_UNIMP,
            "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: SEP module "
            "workloop function: before handlers0 handler:"
            " handler_index: 0x%02" PRIx64 " data0: 0x%02" PRIx64 " data1: 0x%02" PRIx64 " "
            "data2: 0x%02" PRIx64 "\n",
            tid, tid_str, arg2, arg3, arg4, arg5);
        break;
    case 0x82060018: // workloop function: handlers0: handler not found,
                     // panic
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: SEP module "
                      "workloop function: handlers0: handler not found, panic:"
                      " interface_msgid: 0x%02" PRIx64 " method: 0x%02" PRIx64 " data0: "
                      "0x%02" PRIx64 " "
                      "data1: 0x%02" PRIx64 "\n",
                      tid, tid_str, arg2, arg3, arg4, arg5);
        break;
    case 0x8206001C: // workloop function: interface_msgid==0xFFFE
                     // before handler
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: SEP "
                      "module workloop function:"
                      " interface_msgid==0xfffe before handler: data0: "
                      "0x%02" PRIx64 " handler: 0x%02" PRIx64 "\n",
                      tid, tid_str, arg2, arg3);
        break;
    case 0x82080005: // 0x82080005==before Rpc_Call
    case 0x82080006: // 0x82080006==after Rpc_Call
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: %s "
                      "Rpc_Call Sending message to other module:"
                      " fromto: 0x%02" PRIx64 " interface_msgid: 0x%02" PRIx64 " ool: "
                      "0x%02" PRIx64 " method: 0x%02" PRIx64 "\n",
                      tid, tid_str,
                      (trace_id == 0x82080005) ? "Before" : "After", arg2, arg3,
                      arg4, arg5);
        break;
    case 0x8208000D: // before Rpc_Wait
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: Before "
                      "Rpc_Wait Receiving message from other module\n",
                      tid, tid_str);
        break;
    case 0x8208000E: // after Rpc_Wait
        qemu_log_mask(
            LOG_UNIMP,
            "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: After "
            "Rpc_Wait "
            "Receiving message from other module:"
            " fromto: 0x%02" PRIx64 " interface_msgid: 0x%02" PRIx64 " ool: 0x%02" PRIx64 " "
            "method: 0x%02" PRIx64 "\n",
            tid, tid_str, arg2, arg3, arg4, arg5);
        break;
    case 0x82080019: // before Rpc_WaitFrom
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: Before "
                      "Rpc_WaitFrom Receiving message from other module:"
                      " arg2: 0x%02" PRIx64 "\n",
                      tid, tid_str, arg2);
        break;
    case 0x8208001A: // after Rpc_WaitFrom
        qemu_log_mask(
            LOG_UNIMP,
            "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: After "
            "Rpc_WaitFrom Receiving message from other module:"
            " fromto: 0x%02" PRIx64 " interface_msgid: 0x%02" PRIx64 " ool: 0x%02" PRIx64 " "
            "method: 0x%02" PRIx64 "\n",
            tid, tid_str, arg2, arg3, arg4, arg5);
        break;
    case 0x82080011: // before Rpc_ReturnWait
    case 0x82080012: // after Rpc_ReturnWait
        qemu_log_mask(
            LOG_UNIMP,
            "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: %s "
            "Rpc_ReturnWait Receiving message from other module:"
            " fromto: 0x%02" PRIx64 " interface_msgid: 0x%02" PRIx64 " ool: 0x%02" PRIx64 " "
            "method: 0x%02" PRIx64 "\n",
            tid, tid_str, (trace_id == 0x82080011) ? "Before" : "After", arg2,
            arg3, arg4, arg5);
        break;
    case 0x82080014: // before Rpc_Return return response
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: "
                      "Before Rpc_Return return response:"
                      " fromto: 0x%02" PRIx64 " interface_msgid: 0x%02" PRIx64 " ool: "
                      "0x%02" PRIx64 " method: 0x%02" PRIx64 "\n",
                      tid, tid_str, arg2, arg3, arg4, arg5);
        break;
    case 0x8208001D: // before Rpc_WaitNotify
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: Before "
                      "Rpc_WaitNotify:"
                      " Rpc_WaitNotify_arg2 != 0: Rpc_WaitNotify_arg1: "
                      "0x%02" PRIx64 "\n",
                      tid, tid_str, arg2);
        break;
    case 0x8208001e: // after Rpc_WaitNotify
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: "
                      "After Rpc_WaitNotify:"
                      " svc_0x5_0_func_arg2 != 0: svc_0x5_0_func_arg1: "
                      "0x%02" PRIx64 " L4_MR0: 0x%02" PRIx64 "\n",
                      tid, tid_str, arg2, arg3);
        break;
    case 0x82140004: // _dispatch_thread_main__intr/SEPD interrupt
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: "
                      "_dispatch_thread_main__intr/SEPD interrupt "
                      "trace_id 0x%02" PRIx64 ":"
                      " arg2: 0x%02" PRIx64 " arg3: 0x%02" PRIx64 " arg4: 0x%02" PRIx64 " "
                      "arg5: 0x%02" PRIx64 "\n",
                      tid, tid_str, trace_id, arg2, arg3, arg4, arg5);
        break;
    case 0x82140014: // SEP_Driver__Close
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: "
                      "SEP_Driver__Close:"
                      " module_name_int: 0x%02" PRIx64 " fromto: 0x%02" PRIx64 " "
                      "response_data0: 0x%02" PRIx64 "\n",
                      tid, tid_str, arg2, arg3, arg5);
        break;
    case 0x82140024: // *_enable_powersave_arg2/SEP_Driver__SetPowerState
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: "
                      "SEP_Driver__SetPowerState:"
                      " function called: enable_powersave?: 0x%02" PRIx64 " "
                      "is_powersave_enabled: 0x%02" PRIx64 " field_cc3: 0x%02" PRIx64 "\n",
                      tid, tid_str, arg2, arg3, arg4);
        break;
    case 0x82140031: // SEPD_thread_handler:
                     // SEP_Driver__before_InterruptAsync
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: "
                      "SEPD_thread_handler: before_InterruptAsync:"
                      " arg2: 0x%02" PRIx64 "\n",
                      tid, tid_str, arg2);
        break;
    case 0x82140032: // SEPD_thread_handler:
                     // SEP_Driver__after_InterruptAsync
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: "
                      "SEPD_thread_handler: after_InterruptAsync\n",
                      tid, tid_str);
        break;
    case 0x82140195: // AESS_message_received: before
                     // AESS_keywrap_cmd_0x02
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: "
                      "AESS_message_received: before AESS_keywrap_cmd_0x02:"
                      " data0_low: 0x%02" PRIx64 " data0_high: 0x%02" PRIx64 " data1_low: "
                      "0x%02" PRIx64 " data1_high: 0x%02" PRIx64 "\n",
                      tid, tid_str, arg2, arg3, arg4, arg5);
        break;
    case 0x82140196: // AESS_message_received: after
                     // AESS_keywrap_cmd_0x02
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: "
                      "AESS_message_received: after AESS_keywrap_cmd_0x02:"
                      " status: 0x%02" PRIx64 "\n",
                      tid, tid_str, arg2);
        break;
    case 0x82140324: // SEP_Driver__Mailbox_Rx
        memcpy((void *)&m + 0x00, &s->debug_trace_regs[offset + 0x88],
               sizeof(uint32_t));
        memcpy((void *)&m + 0x04, &s->debug_trace_regs[offset + 0x90],
               sizeof(uint32_t));
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Description: (tid: 0x%05" PRIx64 "/%s): "
                      "SEP_Driver__Mailbox_Rx:"
                      " endpoint: 0x%02x tag: 0x%02x opcode: "
                      "0x%02x(%u) param: 0x%02x data: 0x%02x\n",
                      tid, tid_str, m.endpoint, m.tag, m.opcode, m.opcode,
                      m.param, m.data);
        break;
    case 0x82140328: // SEP_Driver__Mailbox_RxMessageQueue
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: "
                      "SEP_Driver__Mailbox_RxMessageQueue:"
                      " endpoint: 0x%02" PRIx64 " opcode: 0x%02" PRIx64 " arg4: "
                      "0x%02" PRIx64 " arg5: 0x%02" PRIx64 "\n",
                      tid, tid_str, arg2, arg3, arg4, arg5);
        break;
    case 0x82140334: // SEP_Driver__Mailbox_ReadMsgFetch
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: "
                      "SEP_Driver__Mailbox_ReadMsgFetch:"
                      " endpoint: 0x%02" PRIx64 " data: 0x%02" PRIx64 " data2: 0x%02" PRIx64 " "
                      "read_msg.data[0]: 0x%02" PRIx64 "\n",
                      tid, tid_str, arg2, arg3, arg4, arg5);
        break;
    case 0x82140338: // SEP_Driver__Mailbox_ReadBlocked
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: "
                      "SEP_Driver__Mailbox_ReadBlocked:"
                      " for_TRNG_ASC0_ASC1_read_0 returned False: "
                      "data0: 0x%02" PRIx64 "\n",
                      tid, tid_str, arg2);
        break;
    case 0x8214033C: // SEP_Driver__Mailbox_ReadComplete
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: "
                      "SEP_Driver__Mailbox_ReadComplete:"
                      " for_TRNG_ASC0_ASC1_read_0 returned True: "
                      "data0: 0x%02" PRIx64 "\n",
                      tid, tid_str, arg2);
        break;
    case 0x82140340: // SEP_Driver__Mailbox_Tx
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: "
                      "SEP_Driver__Mailbox_Tx:"
                      " function_13 returned True:  arg2: 0x%02" PRIx64 " "
                      "arg3: 0x%02" PRIx64 " arg4: 0x%02" PRIx64 " arg5: 0x%02" PRIx64 "\n",
                      tid, tid_str, arg2, arg3, arg4, arg5);
        break;
    case 0x82140344: // SEP_Driver__Mailbox_TxStall
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: "
                      "SEP_Driver__Mailbox_TxStall:"
                      " function_13 returned False: arg2: 0x%02" PRIx64 " "
                      "arg3: 0x%02" PRIx64 " arg4: 0x%02" PRIx64 " arg5: 0x%02" PRIx64 "\n",
                      tid, tid_str, arg2, arg3, arg4, arg5);
        break;
    case 0x82140348: // mod_ASC0_ASC1_function_message_received:
                     // method_0x4131/Mailbox_OOL_In
    case 0x8214034C: // mod_ASC0_ASC1_function_message_received:
                     // Mailbox_OOL_Out
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: SEP "
                      "mod_ASC0_ASC1_function_message_received "
                      "SEP_Driver: Mailbox_OOL_%s:"
                      " arg2: 0x%02" PRIx64 " arg3: 0x%02" PRIx64 " arg4: 0x%02" PRIx64 "\n",
                      tid, tid_str, (trace_id == 0x82140348) ? "In" : "Out",
                      arg2, arg3, arg4);
        break;
    case 0x82140360: // SEP_Driver__Mailbox_Wake
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: "
                      "SEP_Driver__Mailbox_Wake:"
                      " current value: registers[0x4108]: 0x%08" PRIx64 " "
                      "SEP_message_incoming: %" PRIu64 "\n",
                      tid, tid_str, arg2, arg3);
        break;
    case 0x82140364: // SEP_Driver__Mailbox_NoData
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: "
                      "SEP_Driver__Mailbox_NoData:"
                      " current value: registers[0x4108]: 0x%08" PRIx64 "\n",
                      tid, tid_str, arg2);
        break;
    case 0x82140964: // PMGR_message_received
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: "
                      "PMGR_message_received:"
                      " fromto: 0x%02" PRIx64 " data0: 0x%02" PRIx64 " data1: 0x%02" PRIx64 "\n",
                      tid, tid_str, arg2, arg3, arg4);
        break;
    case 0x82140968: // PMGR_enable_clock
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: "
                      "PMGR_enable_clock:"
                      " enable_clock: 0x%02" PRIx64 "\n",
                      tid, tid_str, arg2);
        break;
    default: // Unknown trace value
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Description: tid: 0x%05" PRIx64 "/%s: "
                      "Unknown trace_id 0x%02" PRIx64 ":"
                      " arg2: 0x%02" PRIx64 " arg3: 0x%02" PRIx64 " arg4: 0x%02" PRIx64 " "
                      "arg5: 0x%02" PRIx64 "\n",
                      tid, tid_str, trace_id, arg2, arg3, arg4, arg5);
        break;
    }
}

static uint64_t debug_trace_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    AppleSEPState *s = APPLE_SEP(opaque);
    uint64_t ret = 0;

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->cpu), stderr, CPU_DUMP_CODE);
#endif
    if (!s->shmbuf_base) {
        qemu_log_mask(
            LOG_UNIMP,
            "DEBUG_TRACE: SHMBUF_BASE==NULL: Unknown read at 0x" HWADDR_FMT_plx
            " size=%u\n",
            addr, size);
        return 0;
    }
    switch (addr) {
    case 0x0:
        return 0xffffffff; // negated trace exclusion mask for wrapper
    case 0x4: // some index
    case 0x18: // unknown0
    case 0x40: // unknown1
        goto jump_default;
    case 0x1c:
        return 0x0; // disable trace mask for inner function
    case 0x20:
        return 0xffffffff; // trace mask for inner function
    default:
        qemu_log_mask(LOG_UNIMP,
                      "DEBUG_TRACE: Unknown read at 0x" HWADDR_FMT_plx
                      " size=%u ret==0x%" PRIx64 "\n",
                      addr, size, ret);
    jump_default:
        memcpy(&ret, &s->debug_trace_regs[addr], size);
        break;
    }
    return ret;
}

static const MemoryRegionOps debug_trace_reg_ops = {
    .write = debug_trace_reg_write,
    .read = debug_trace_reg_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 8,
    .impl.min_access_size = 1,
    .impl.max_access_size = 8,
    .valid.unaligned = false,
};


#define REG_TRNG_FIFO_OUTPUT_BASE (0x00)
#define REG_TRNG_FIFO_OUTPUT_END (0x0C)
#define REG_TRNG_STATUS (0x10)
#define TRNG_STATUS_FILLED BIT(0)
#define TRNG_STATUS_UNKNOWN0 BIT(8)
#define REG_TRNG_CONFIG (0x14)
#define TRNG_CONFIG_INTERRUPTS_ENABLED BIT(10)
#define TRNG_CONFIG_ENABLED BIT(19)
#define TRNG_CONFIG_PERSONALISED BIT(20)
#define REG_TRNG_AES_KEY_BASE (0x40)
#define REG_TRNG_AES_KEY_END (0x5C)
#define REG_TRNG_ECID_LOW (0x60)
#define REG_TRNG_ECID_HI (0x64)
#define REG_TRNG_COUNTER_LOW (0x68)
#define REG_TRNG_COUNTER_HI (0x6c)

static void trng_regs_reg_write(void *opaque, hwaddr addr, uint64_t data,
                                unsigned size)
{
    MachineState *machine = MACHINE(qdev_get_machine());
    AppleSEPState *sep;
    AppleTRNGState *s;
    uint32_t interrupts_enabled;

    sep = APPLE_SEP(
        object_property_get_link(OBJECT(machine), "sep", &error_fatal));
    AppleA7IOP *a7iop = APPLE_A7IOP(sep);
#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(sep->cpu), stderr, CPU_DUMP_CODE);
#endif

    s = (AppleTRNGState *)opaque;

#if 0
    qemu_log_mask(LOG_UNIMP,
                  "TRNG_REGS: Write at 0x" HWADDR_FMT_plx
                  " of value 0x%" PRIx64 "\n",
                  addr, data);
#endif

    switch (addr) {
    case REG_TRNG_FIFO_OUTPUT_BASE ... REG_TRNG_FIFO_OUTPUT_END:
        if ((s->offset_0x70 & 0x40) != 0) {
            data = bswap32(data);
        }
        memcpy(s->fifo + (addr - REG_TRNG_FIFO_OUTPUT_BASE), &data, size);
        if (addr == REG_TRNG_FIFO_OUTPUT_END &&
            ((s->offset_0x70 & 0x40) != 0)) {
            QCryptoCipher *cipher;

            cipher = qcrypto_cipher_new(QCRYPTO_CIPHER_ALGO_AES_256,
                                        QCRYPTO_CIPHER_MODE_ECB, s->key,
                                        sizeof(s->key), &error_abort);
            g_assert_nonnull(cipher);
            qcrypto_cipher_encrypt(cipher, s->fifo, s->fifo, sizeof(s->fifo),
                                   &error_abort);
            qcrypto_cipher_free(cipher);
        }
        break;
    case REG_TRNG_STATUS:
        interrupts_enabled = (s->config & TRNG_CONFIG_INTERRUPTS_ENABLED) != 0;
        uint32_t filled = (data & TRNG_STATUS_FILLED) != 0;
        if (filled && (s->offset_0x70 & 0xc0) == 0) {
            qcrypto_random_bytes(s->fifo, sizeof(s->fifo), NULL);
            // memset(s->fifo, 0xaa, sizeof(s->fifo));
            // memset(s->fifo, 0xbb, sizeof(s->fifo));
        }
        break;
    case REG_TRNG_CONFIG: {
        uint32_t old_interrupts_enabled =
            (s->config & TRNG_CONFIG_INTERRUPTS_ENABLED) != 0;
        s->config = (uint32_t)data;
        qemu_log_mask(
            LOG_UNIMP,
            "TRNG_REGS: REG_TRNG_CONFIG/OFFSET_0x14 write at 0x" HWADDR_FMT_plx
            " of value 0x%" PRIx64 "\n",
            addr, data);
        interrupts_enabled = (data & TRNG_CONFIG_INTERRUPTS_ENABLED) != 0;

        if (!old_interrupts_enabled && interrupts_enabled) {
            s->config |= TRNG_CONFIG_ENABLED;
            apple_a7iop_interrupt_status_push(a7iop->iop_mailbox,
                                              0x10003); // TRNG
        }
        s->config |= TRNG_CONFIG_ENABLED;
        break;
    }
    case REG_TRNG_AES_KEY_BASE ... REG_TRNG_AES_KEY_END:
        if ((s->offset_0x70 & 0xc0) != 0) {
            data = bswap32(data);
        }
        memcpy(s->key + (addr - REG_TRNG_AES_KEY_BASE), &data, size);
        break;
    case REG_TRNG_ECID_LOW:
        if ((s->offset_0x70 & 0x80) != 0) {
            data = bswap32(data);
        }
        s->ecid &= 0xFFFFFFFF00000000;
        s->ecid |= data & 0xFFFFFFFF;
        break;
    case REG_TRNG_ECID_HI:
        if ((s->offset_0x70 & 0x80) != 0) {
            data = bswap32(data);
        }
        s->ecid &= 0x00000000FFFFFFFF;
        s->ecid |= (data & 0xFFFFFFFF) << 32;
        break;
    case REG_TRNG_COUNTER_LOW:
        if ((s->offset_0x70 & 0x80) != 0) {
            data = bswap32(data);
        }
        s->counter &= 0xFFFFFFFF00000000;
        s->counter |= data & 0xFFFFFFFF;
        break;
    case REG_TRNG_COUNTER_HI:
        if ((s->offset_0x70 & 0x80) != 0) {
            data = bswap32(data);
        }
        s->counter &= 0x00000000FFFFFFFF;
        s->counter |= (data & 0xFFFFFFFF) << 32;
        if ((s->offset_0x70 & 0x80) != 0) {
            uint8_t seed_material[DRBG_CTR_AES256_SEED_SIZE] = { 0 };
            memcpy(seed_material + 0x0, s->key, 0x20);
            memcpy(seed_material + 0x20, &s->ecid, 0x8);
            memcpy(seed_material + 0x28, &s->counter, 0x8);
            if (s->ctr_drbg_init) {
                s->ctr_drbg_init = 0;
                drbg_ctr_aes256_init(&s->ctr_drbg_rng, seed_material);
                memset(s->fifo, 0, 0x10);
            } else {
                drbg_ctr_aes256_update(&s->ctr_drbg_rng.key, &s->ctr_drbg_rng.V,
                                       seed_material);
                drbg_ctr_aes256_random(&s->ctr_drbg_rng, 0x10, s->fifo);
            }
        }
        break;
    case 0x70:
        s->offset_0x70 = data;
        if ((s->offset_0x70 & 0x80) != 0) {
            s->ctr_drbg_init = 1;
        } else if ((s->offset_0x70 & 0x40) == 0) {
            memset(s->key, 0, sizeof(s->key));
        }
        // don't do the encryption here
        break;
    default:
#if 0
        qemu_log_mask(LOG_UNIMP,
                      "TRNG_REGS: Unknown write at 0x" HWADDR_FMT_plx
                      " of value 0x%" PRIx64 "\n",
                      addr, data);
#endif
        break;
    }
}

static uint64_t trng_regs_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    MachineState *machine = MACHINE(qdev_get_machine());
    AppleSEPState *sep;
    AppleTRNGState *s;
    uint64_t ret = 0;

    sep = APPLE_SEP(
        object_property_get_link(OBJECT(machine), "sep", &error_fatal));
    AppleA7IOP *a7iop = APPLE_A7IOP(sep);
#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(sep->cpu), stderr, CPU_DUMP_CODE);
#endif

    s = (AppleTRNGState *)opaque;
    uint32_t interrupts_enabled =
        (s->config & TRNG_CONFIG_INTERRUPTS_ENABLED) != 0;

    switch (addr) {
    case REG_TRNG_FIFO_OUTPUT_BASE ... REG_TRNG_FIFO_OUTPUT_END:
        memcpy(&ret, s->fifo + (addr - REG_TRNG_FIFO_OUTPUT_BASE), size);
        if ((s->offset_0x70 & 0xc0) != 0) {
            ret = bswap32(ret);
        }
        break;
        break;
    case REG_TRNG_STATUS:
        // ret = TRNG_STATUS_FILLED;
        ret = TRNG_STATUS_FILLED | TRNG_STATUS_UNKNOWN0;
        break;
    case REG_TRNG_CONFIG:
        ret = s->config;
        // ret = TRNG_CONFIG_PERSONALISED;
        if (interrupts_enabled) {
            apple_a7iop_interrupt_status_push(a7iop->iop_mailbox,
                                              0x10003); // TRNG
        }
        break;
    case 0x78: // (value & 0x180000) == 0 == panic
        ret = 0x180000;
        break;
    case REG_TRNG_AES_KEY_BASE ... REG_TRNG_AES_KEY_END:
        memcpy(&ret, s->key + (addr - REG_TRNG_AES_KEY_BASE), size);
        break;
    case REG_TRNG_ECID_LOW:
        ret = s->ecid & 0xFFFFFFFF;
        break;
    case REG_TRNG_ECID_HI:
        ret = (s->ecid & 0xFFFFFFFF00000000) >> 32;
        break;
    case REG_TRNG_COUNTER_LOW:
        ret = s->counter & 0xFFFFFFFF;
        break;
    case REG_TRNG_COUNTER_HI:
        ret = (s->counter & 0xFFFFFFFF00000000) >> 32;
        break;
    case 0x70:
        ret = s->offset_0x70;
        break;
    default:
#if 0
        qemu_log_mask(LOG_UNIMP,
                      "TRNG_REGS: Unknown read at 0x" HWADDR_FMT_plx "\n",
                      addr);
#endif
        break;
    }
#if 0
    qemu_log_mask(LOG_UNIMP,
                  "TRNG_REGS: Read at 0x" HWADDR_FMT_plx
                  " ret: 0x%" PRIx64 "\n",
                  addr, ret);
#endif
    return ret;
}

static const MemoryRegionOps trng_regs_reg_ops = {
    .write = trng_regs_reg_write,
    .read = trng_regs_reg_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.unaligned = false,
};


////

const char *sepos_powerstate_name(uint64_t powerstate_offset);

const char *sepos_powerstate_name(uint64_t powerstate_offset)
{
    switch (powerstate_offset) {
    case 0x20: // mod_PKA ; PKA0 ; arg8 is 0xc8
        return "PKA0";
    case 0x28:
        return "TRNG";
    case 0x30: // PKA1
        return "PKA1";
    case 0x48:
        return "I2C";
    case 0x58:
        return "KEY";
    case 0x60:
        return "EISP";
    case 0x68:
        return "SEPD";
    default:
        break;
    }
    return "Unknown";
}


static void pmgr_base_reg_write(void *opaque, hwaddr addr, uint64_t data,
                                unsigned size)
{
    AppleSEPState *s = APPLE_SEP(opaque);

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
    case 0x20: // mod_PKA ; PKA0 ; arg8 is 0xc8
    case 0x28: // mod_TRNG
    case 0x30: // PKA1
    case 0x48: // mod_I2C
    case 0x58: // mod_KEY
    case 0x60: // mod_EISP
    case 0x68: // mod_SEPD
        qemu_log_mask(
            LOG_UNIMP,
            "SEP PMGR_BASE: PowerState %s write before at 0x" HWADDR_FMT_plx
            " with value 0x%" PRIx64 "\n",
            sepos_powerstate_name(addr), addr, data);
        /*
            LIKE AP PMGR
            data | 0x80000000 == RESET
            data | 0x.f == ENABLE
            data | 0x.4 == POWER_SAVE
            data | 0xf. == ENABLED
            data | 0x4. == POWER_SAVE_ACTIVATED?
        */
        data = ((data & 0xf) << 4) | (data & 0xf);
        if ((data & 0xf) == 0xf) {
            if (addr == 0x58) {
                apple_a7iop_interrupt_status_push(APPLE_A7IOP(s)->iop_mailbox,
                                                  0x10000); // KEY
            }
            if (addr == 0x48) {
                apple_a7iop_interrupt_status_push(APPLE_A7IOP(s)->iop_mailbox,
                                                  0x10002); // I2C
            }
            if (addr == 0x28) {
                // apple_a7iop_interrupt_status_push(APPLE_A7IOP(s)->iop_mailbox,
                // 0x10003); // TRNG
            }
            if (addr == 0x68) {
                //////apple_a7iop_interrupt_status_push(APPLE_A7IOP(s)->iop_mailbox,
                /// 0x10003); // TRNG
            }
            // apple_a7iop_interrupt_status_push(APPLE_A7IOP(s)->iop_mailbox,
            // 0x10005); // AES_SEP
            // apple_a7iop_interrupt_status_push(APPLE_A7IOP(s)->iop_mailbox,
            // 0x10007); // GPIO
            if (addr == 0x20) {
                ////apple_a7iop_interrupt_status_push(APPLE_A7IOP(s)->iop_mailbox,
                /// 0x1000a); // PKA
                apple_a7iop_interrupt_status_push(APPLE_A7IOP(s)->iop_mailbox,
                                                  0x1000b); // PKA
            }
            if (addr == 0x30) {
                //////apple_a7iop_interrupt_status_push(APPLE_A7IOP(s)->iop_mailbox,
                /// 0x1000a); // PKA2???
            }
        }

        qemu_log_mask(
            LOG_UNIMP,
            "SEP PMGR_BASE: PowerState %s write after at 0x" HWADDR_FMT_plx
            " with value 0x%" PRIx64 "\n",
            sepos_powerstate_name(addr), addr, data);
        goto jump_default;
    case 0x8000:
        // the resulting values should only reset on SoC reset
        if ((data & 1) != 0) {
            s->pmgr_fuse_changer_bit0_was_set = true;
        }
        if ((data & 2) != 0) {
            s->pmgr_fuse_changer_bit1_was_set = true;
        }
        qemu_log_mask(LOG_UNIMP,
                      "SEP PMGR_BASE: fuse change write at 0x" HWADDR_FMT_plx
                      " with value 0x%" PRIx64 "\n",
                      addr, data);
        goto jump_default;
    default:
#if 1
        qemu_log_mask(LOG_UNIMP,
                      "SEP PMGR_BASE: Unknown write at 0x" HWADDR_FMT_plx
                      " with value 0x%" PRIx64 "\n",
                      addr, data);
#endif
    jump_default:
        memcpy(&s->pmgr_base_regs[addr], &data, size);
        break;
    }
}

static uint64_t pmgr_base_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    AppleSEPState *s = APPLE_SEP(opaque);
    uint64_t ret = 0;

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->cpu), stderr, CPU_DUMP_CODE);
#endif
    memcpy(&ret, &s->pmgr_base_regs[addr], size);
    switch (addr) {
    case 0x20: // mod_PKA ; PKA0 ; arg8 is 0xc8
    case 0x28: // mod_TRNG
    case 0x30: // PKA1
    case 0x48: // mod_I2C
    case 0x58: // mod_KEY
    case 0x60: // mod_EISP
    case 0x68: // mod_SEPD
#if 1
        qemu_log_mask(LOG_UNIMP,
                      "SEP PMGR_BASE: PowerState %s read at 0x" HWADDR_FMT_plx
                      " with value 0x%" PRIx64 "\n",
                      sepos_powerstate_name(addr), addr, ret);
#endif
        break;
    case 0x8200:
#ifdef SEP_ENABLE_TRACE_BUFFER
        enable_trace_buffer(s); // for T8015
#endif
        goto jump_default;
    default:
    jump_default:
#if 1
        qemu_log_mask(LOG_UNIMP,
                      "SEP PMGR_BASE: Unknown read at 0x" HWADDR_FMT_plx
                      " with value 0x%" PRIx64 "\n",
                      addr, ret);
#endif
        break;
    }

    return ret;
}

static const MemoryRegionOps pmgr_base_reg_ops = {
    .write = pmgr_base_reg_write,
    .read = pmgr_base_reg_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.unaligned = false,
};


static void key_base_reg_write(void *opaque, hwaddr addr, uint64_t data,
                               unsigned size)
{
    AppleSEPState *s = APPLE_SEP(opaque);

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
    case 0x8: // command or storage index: 0x20-0x26, 0x30-0x31, 0x04 (without
              // input)
        /*
        cmds:
        0x0/0x1: wrapping key primary/secondary cmd7_0x4
        0x2/0x3: auth key primary/secondary cmd7_0x5
        0x6/0x7: cmd7_0x8
        0x8/0x9: cmd7_0x9
        0xa/0xb: sub key primary/secondary cmd7_0x6
        0xc: cmd7_0xb
        0xd: cmd7_0xc
        0xe/0xf: cmd7_0xa
        0x10..0x16: something about Ks and interfaces cmd7_0x3
        0x18..0x1e: send data2==data_size_qwords of data cmd7_0x2(cmd7_0x7)
        0x3f: first 0x40 bytes of random data cmd7_0x7
        0x40: second 0x40 bytes of random data cmd7_0x7
        */
        qemu_log_mask(LOG_UNIMP,
                      "SEP KEY_BASE: Offset 0x" HWADDR_FMT_plx
                      ": Execute Command/Storage Index: cmd 0x%" PRIx64
                      "\n",
                      addr, data);
        goto jump_default;
    case 0x308 ... 0x344: // 0x40 bytes of output from TRNG
        qemu_log_mask(LOG_UNIMP,
                      "SEP KEY_BASE: Offset 0x" HWADDR_FMT_plx
                      ": Input: cmd 0x%" PRIx64 "\n",
                      addr, data);
        goto jump_default;
    default:
    jump_default:
        memcpy(&s->key_base_regs[addr], &data, size);
        qemu_log_mask(LOG_UNIMP,
                      "SEP KEY_BASE: Unknown write at 0x" HWADDR_FMT_plx
                      " with value 0x%" PRIx64 "\n",
                      addr, data);
        break;
    }
}

static uint64_t key_base_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    AppleSEPState *s = APPLE_SEP(opaque);
    uint64_t ret = 0;

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
    default:
        memcpy(&ret, &s->key_base_regs[addr], size);
        qemu_log_mask(LOG_UNIMP,
                      "SEP KEY_BASE: Unknown read at 0x" HWADDR_FMT_plx "\n",
                      addr);
        break;
    }

    return ret;
}

static const MemoryRegionOps key_base_reg_ops = {
    .write = key_base_reg_write,
    .read = key_base_reg_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.unaligned = false,
};


static void key_fcfg_reg_write(void *opaque, hwaddr addr, uint64_t data,
                               unsigned size)
{
    AppleSEPState *s = APPLE_SEP(opaque);
    AppleA7IOP *a7iop = APPLE_A7IOP(s);

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
    case 0x0:
        // if (data == 0x3)
        {
#if 0
        if (data == 0x101) {
            //apple_mbox_set_custom0(s->mbox, 0x40003);
            //apple_mbox_set_custom0(s->mbox, 0);
            //apple_a7iop_interrupt_status_push(a7iop->iop_mailbox, 0x40003);
            apple_a7iop_interrupt_status_push(a7iop->iop_mailbox, 0x40000);
            //apple_a7iop_interrupt_status_push(a7iop->iop_mailbox, 0x40001);
            //apple_a7iop_interrupt_status_push(a7iop->iop_mailbox, 0x40002);
            //apple_a7iop_interrupt_status_push(a7iop->iop_mailbox, 0x40003);
            MachineState *machine = MACHINE(qdev_get_machine());
            T8030MachineState *tms = T8030_MACHINE(machine);
            DeviceState *gpio = NULL;
            gpio = DEVICE(object_property_get_link(OBJECT(machine), "sep_gpio", &error_fatal));
            qemu_set_irq(qdev_get_gpio_in(gpio, 0), true);
            //((uint32_t *)s->progress_regs)[0x00] = 0xdeadbee8ull;
        }
#endif
#if 0
        qemu_log_mask(LOG_UNIMP,
                      "SEP KEY_FCFG: TEST0 0x" HWADDR_FMT_plx
                      " with value 0x%" PRIx64 "\n",
                      addr, data);
#endif
            // CPUState *cs = CPU(s->cpu);
            // cpu_dump_state(cs, stderr, CPU_DUMP_CODE);
            // usleep(500);
        }

        goto jump_default;
    case 0x4:
        // if (data == 0x3)
        {
#if 0
        qemu_log_mask(LOG_UNIMP,
                      "SEP KEY_FCFG: TEST1 0x" HWADDR_FMT_plx
                      " with value 0x%" PRIx64 "\n",
                      addr, data);
#endif
            // CPUState *cs = CPU(s->cpu);
            // cpu_dump_state(cs, stderr, CPU_DUMP_CODE);
        }

        goto jump_default;
    case 0x10:
        // if ((data & 1) != 0)
        if (data == 0x1) {
            ////((uint32_t*)s->key_fcfg_regs)[0x00 / 4] |= (1 << 31) | (1 << 0);
            //((uint32_t *)s->key_fcfg_regs)[0x00 / 4] = (1 << 31) | (1 << 0);
            ((uint32_t *)s->key_base_regs)[0x00 / 4] = (1 << 31) | (1 << 0);
        }
        goto jump_default;
    case 0x14:
        qemu_log_mask(LOG_UNIMP,
                      "SEP KEY_FCFG: vals 0x" HWADDR_FMT_plx
                      " with value 0x%" PRIx64 "\n",
                      addr, data);
        if (data == 0xffff) {
            s->key_fcfg_offset_0x14_index = 0x0;
            memset(s->key_fcfg_offset_0x14_values, 0, sizeof(s->key_fcfg_offset_0x14_values));
        }
        uint8_t index = s->key_fcfg_offset_0x14_index;
        uint8_t index_limit = sizeof(s->key_fcfg_offset_0x14_values)/sizeof(s->key_fcfg_offset_0x14_values[0]);
        index = (index < index_limit) ? index : 0;
        s->key_fcfg_offset_0x14_values[index] = data & 0xffff;
        s->key_fcfg_offset_0x14_index++;
        goto jump_default;
    default:
    jump_default:
        memcpy(&s->key_fcfg_regs[addr], &data, size);
#if 0
        qemu_log_mask(LOG_UNIMP,
                      "SEP KEY_FCFG: Unknown write at 0x" HWADDR_FMT_plx
                      " with value 0x%" PRIx64 "\n",
                      addr, data);
#endif
        break;
    }
}

static uint64_t key_fcfg_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    AppleSEPState *s = APPLE_SEP(opaque);
    uint64_t ret = 0;
    uint8_t key_fcfg_offset_0x14_index = 0;
    uint8_t key_fcfg_offset_0x14_index_limit = 0;

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
    case 0x14:
        // it'll complain otherwise:
        // ../hw/arm/apple-silicon/sep.c:1541:9: warning: label followed by a
        // declaration is a C23 extension [-Wc23-extensions]
        key_fcfg_offset_0x14_index = s->key_fcfg_offset_0x14_index;
        key_fcfg_offset_0x14_index_limit =
                                     sizeof(s->key_fcfg_offset_0x14_values)/
                                     sizeof(s->key_fcfg_offset_0x14_values[0]);
        key_fcfg_offset_0x14_index = (key_fcfg_offset_0x14_index <
                                      key_fcfg_offset_0x14_index_limit)
                                     ? key_fcfg_offset_0x14_index : 0;
        ret = ((uint32_t)key_fcfg_offset_0x14_index << 16) |
              s->key_fcfg_offset_0x14_values[key_fcfg_offset_0x14_index];
        qemu_log_mask(LOG_UNIMP,
                      "SEP KEY_FCFG: vals read at 0x" HWADDR_FMT_plx
                      " ret: 0x%" PRIx64 "\n",
                      addr, ret);
        break;
    case 0x18: // for SKG ; 0x4 | (value & 0x3)
        // ret = 0x4 | 0x0; // when AMK is disabled
        ret = 0x4 | 0x1; // when AMK is enabled
        qemu_log_mask(LOG_UNIMP,
                      "SEP KEY_FCFG: AMK read at 0x" HWADDR_FMT_plx
                      " ret: 0x%" PRIx64 "\n",
                      addr, ret);
        break;
    default:
        memcpy(&ret, &s->key_fcfg_regs[addr], size);
        qemu_log_mask(LOG_UNIMP,
                      "SEP KEY_FCFG: Unknown read at 0x" HWADDR_FMT_plx
                      " ret: 0x%" PRIx64 "\n",
                      addr, ret);
        break;
    }

    return ret;
}

static const MemoryRegionOps key_fcfg_reg_ops = {
    .write = key_fcfg_reg_write,
    .read = key_fcfg_reg_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.unaligned = false,
};


static void moni_base_reg_write(void *opaque, hwaddr addr, uint64_t data,
                                unsigned size)
{
    AppleSEPState *s = APPLE_SEP(opaque);

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
    default:
        memcpy(&s->moni_base_regs[addr], &data, size);
        qemu_log_mask(LOG_UNIMP,
                      "SEP MONI_BASE: Unknown write at 0x" HWADDR_FMT_plx
                      " with value 0x%" PRIx64 "\n",
                      addr, data);
        break;
    }
}

static uint64_t moni_base_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    AppleSEPState *s = APPLE_SEP(opaque);
    uint64_t ret = 0;

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
    default:
        memcpy(&ret, &s->moni_base_regs[addr], size);
        qemu_log_mask(LOG_UNIMP,
                      "SEP MONI_BASE: Unknown read at 0x" HWADDR_FMT_plx "\n",
                      addr);
        break;
    }

    return ret;
}

static const MemoryRegionOps moni_base_reg_ops = {
    .write = moni_base_reg_write,
    .read = moni_base_reg_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.unaligned = false,
};


static void moni_thrm_reg_write(void *opaque, hwaddr addr, uint64_t data,
                                unsigned size)
{
    AppleSEPState *s = APPLE_SEP(opaque);

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
    default:
        memcpy(&s->moni_thrm_regs[addr], &data, size);
        qemu_log_mask(LOG_UNIMP,
                      "SEP MONI_THRM: Unknown write at 0x" HWADDR_FMT_plx
                      " with value 0x%" PRIx64 "\n",
                      addr, data);
        break;
    }
}

static uint64_t moni_thrm_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    AppleSEPState *s = APPLE_SEP(opaque);
    uint64_t ret = 0;

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
    default:
        memcpy(&ret, &s->moni_thrm_regs[addr], size);
        qemu_log_mask(LOG_UNIMP,
                      "SEP MONI_THRM: Unknown read at 0x" HWADDR_FMT_plx "\n",
                      addr);
        break;
    }

    return ret;
}

static const MemoryRegionOps moni_thrm_reg_ops = {
    .write = moni_thrm_reg_write,
    .read = moni_thrm_reg_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.unaligned = false,
};


static void eisp_base_reg_write(void *opaque, hwaddr addr, uint64_t data,
                                unsigned size)
{
    AppleSEPState *s = APPLE_SEP(opaque);

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
    default:
        memcpy(&s->eisp_base_regs[addr], &data, size);
        qemu_log_mask(LOG_UNIMP,
                      "SEP EISP_BASE: Unknown write at 0x" HWADDR_FMT_plx
                      " with value 0x%" PRIx64 "\n",
                      addr, data);
        break;
    }
}

static uint64_t eisp_base_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    AppleSEPState *s = APPLE_SEP(opaque);
    uint64_t ret = 0;

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
    default:
        memcpy(&ret, &s->eisp_base_regs[addr], size);
        qemu_log_mask(LOG_UNIMP,
                      "SEP EISP_BASE: Unknown read at 0x" HWADDR_FMT_plx "\n",
                      addr);
        break;
    }

    return ret;
}

static const MemoryRegionOps eisp_base_reg_ops = {
    .write = eisp_base_reg_write,
    .read = eisp_base_reg_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    //.valid.max_access_size = 8, // when loading t8030 v14.8 SEP, TCG hflags
    // mismatch (current:(0x00000021,0x0000000000104004)
    // rebuilt:(0x00000021,0x0000000000104000)
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    //.impl.max_access_size = 8, // when loading t8030 v14.8 SEP, TCG hflags
    // mismatch (current:(0x00000021,0x0000000000104004)
    // rebuilt:(0x00000021,0x0000000000104000)
    .valid.unaligned = false,
};


static void eisp_hmac_reg_write(void *opaque, hwaddr addr, uint64_t data,
                                unsigned size)
{
    AppleSEPState *s = APPLE_SEP(opaque);

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
    default:
        memcpy(&s->eisp_hmac_regs[addr], &data, size);
        qemu_log_mask(LOG_UNIMP,
                      "SEP EISP_HMAC: Unknown write at 0x" HWADDR_FMT_plx
                      " with value 0x%" PRIx64 "\n",
                      addr, data);
        break;
    }
}

static uint64_t eisp_hmac_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    AppleSEPState *s = APPLE_SEP(opaque);
    uint64_t ret = 0;

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
    default:
        memcpy(&ret, &s->eisp_hmac_regs[addr], size);
        qemu_log_mask(LOG_UNIMP,
                      "SEP EISP_HMAC: Unknown read at 0x" HWADDR_FMT_plx "\n",
                      addr);
        break;
    }

    return ret;
}

static const MemoryRegionOps eisp_hmac_reg_ops = {
    .write = eisp_hmac_reg_write,
    .read = eisp_hmac_reg_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.unaligned = false,
};

typedef struct {
    const uint8_t enc[0x30];
    const uint8_t dec[0x30];
} AESS_enc_dec_dict_t;

static const AESS_enc_dec_dict_t
    enc_dec_dict[] = {
        { { 0xe7, 0x06, 0x9c, 0x22, 0x51, 0x1e, 0xb6, 0xce, 0x03, 0xf4,
            0xbb, 0xa2, 0x3a, 0x6a, 0xf3, 0x34, 0x44, 0xc8, 0x1f, 0x5c,
            0xfb, 0xa7, 0x7e, 0x46, 0x07, 0xaf, 0x2a, 0x11, 0xd7, 0x01,
            0x77, 0x31, 0xe2, 0xf7, 0xd4, 0x7f, 0x37, 0x07, 0x2e, 0x37,
            0x7e, 0xec, 0xd0, 0xf4, 0x83, 0xb5, 0x5a, 0xab },
          { 0x16, 0xcd, 0x11, 0x71, 0x35, 0x26, 0x23, 0x35, 0xce, 0x60,
            0xf3, 0xd6, 0xf9, 0x18, 0xef, 0xf6, 0xa8, 0x14, 0x30, 0x56,
            0x60, 0x7f, 0x3b, 0x3b, 0xb3, 0xcb, 0x68, 0x2d, 0xe8, 0x5a,
            0xb5, 0xb7, 0x47, 0x61, 0xe3, 0x89, 0x00, 0xd5, 0x74, 0xe8,
            0xd4, 0x1c, 0xbf, 0xcc, 0x0e, 0x46, 0x3a, 0x62 } },
        { { 0x1e, 0x34, 0x48, 0x92, 0xd3, 0xa7, 0x37, 0x0b, 0x8f, 0x5e,
            0x44, 0xd9, 0xc9, 0x7f, 0x88, 0x3e, 0x42, 0x54, 0xa7, 0x2b,
            0x65, 0xb3, 0xa1, 0x7e, 0x50, 0x54, 0x9c, 0xc2, 0x07, 0x35,
            0x2c, 0x9c, 0x1b, 0x24, 0xac, 0xd1, 0x07, 0xf0, 0x32, 0x27,
            0xb3, 0x78, 0xcc, 0x4d, 0x7f, 0xdf, 0xfe, 0xf0 },
          { 0xde, 0xfa, 0xfe, 0x73, 0x8c, 0x0d, 0x5d, 0x5d, 0xb9, 0x66,
            0x9b, 0x9c, 0x01, 0x36, 0xe8, 0xc7, 0xea, 0xdf, 0xac, 0xb6,
            0x42, 0x4a, 0x11, 0x6a, 0xe7, 0xfd, 0x43, 0xa0, 0x2a, 0x31,
            0xe5, 0x68, 0x9d, 0x10, 0x4c, 0x27, 0xca, 0xbf, 0x4f, 0x92,
            0xaf, 0x3e, 0x28, 0xdc, 0xb4, 0x1a, 0x3f, 0xf5 } },
        { { 0x7d, 0x75, 0x88, 0x04, 0xd1, 0x83, 0xa4, 0x63, 0x00, 0xf0,
            0x55, 0xdd, 0x39, 0x58, 0xa8, 0xdc, 0xf9, 0x54, 0xfe, 0xe3,
            0xac, 0x91, 0x95, 0x44, 0xcc, 0xd1, 0x00, 0x65, 0x63, 0x20,
            0xbb, 0x7d, 0xff, 0x20, 0x7c, 0x56, 0xa1, 0x7a, 0x10, 0x58,
            0x71, 0x40, 0x5f, 0x89, 0xf7, 0x73, 0x65, 0x16 },
          { 0xff, 0x0d, 0x97, 0x30, 0xe0, 0x84, 0x85, 0xe2, 0x45, 0xb6,
            0x01, 0x1b, 0x9a, 0xfe, 0x34, 0x1f, 0xed, 0x64, 0x1c, 0x4b,
            0x8c, 0xbd, 0xab, 0xd0, 0xa9, 0x76, 0x9a, 0xbf, 0xc1, 0xd3,
            0x15, 0xc6, 0x36, 0x57, 0xcd, 0x8c, 0xa9, 0x2c, 0x5b, 0x33,
            0x0b, 0x99, 0x6c, 0x86, 0x2b, 0x2e, 0x27, 0xf7 } },
        { { 0xbd, 0x07, 0x45, 0x2f, 0xe7, 0xd8, 0xe7, 0xf7, 0xea, 0x2f,
            0xa0, 0x2e, 0x31, 0x24, 0xf0, 0xfe, 0xd4, 0xa6, 0xeb, 0x86,
            0x50, 0x56, 0xdb, 0x41, 0xce, 0x0f, 0x79, 0x79, 0x34, 0x75,
            0x52, 0x0b, 0x5a, 0x96, 0xb1, 0x53, 0x43, 0x6b, 0xf7, 0x48,
            0x0b, 0x09, 0x75, 0x47, 0x16, 0x67, 0x03, 0xe5 },
          { 0x72, 0x57, 0x44, 0xc1, 0xf7, 0xba, 0xc4, 0x1c, 0x46, 0xdc,
            0x1a, 0x15, 0x92, 0x73, 0x38, 0x83, 0x96, 0x71, 0x46, 0x74,
            0x44, 0xf3, 0xf5, 0x34, 0xa8, 0xe1, 0x19, 0x14, 0xb0, 0xaf,
            0x7c, 0x22, 0x26, 0xdc, 0x0b, 0xfb, 0x6d, 0x3b, 0x6a, 0x49,
            0x0b, 0xbf, 0xe1, 0x38, 0x0f, 0xdd, 0xeb, 0xe5 } },
        // HACK: return the same key as 0x7d75..., because the emulator doesn't
        // see the actual keyslot of the re-encrypted and (badly) manifested
        // file as being valid for whatever reason
        { { 0xA8, 0xB9, 0xAC, 0x16, 0x3B, 0x27, 0xFD, 0xB7, 0xE1, 0x4E,
            0xAD, 0x47, 0xC4, 0x61, 0xDE, 0x49, 0xE2, 0x85, 0x8F, 0x7A,
            0xE7, 0x30, 0x82, 0xC2, 0xD5, 0x0D, 0xF9, 0xE6, 0x6F, 0x5D,
            0x97, 0x7C, 0x1F, 0xF5, 0x89, 0x46, 0x32, 0x70, 0xC2, 0xDF,
            0x6B, 0x15, 0xBB, 0x4A, 0x5C, 0xB3, 0xDC, 0x76 },
          { 0xff, 0x0d, 0x97, 0x30, 0xe0, 0x84, 0x85, 0xe2, 0x45, 0xb6,
            0x01, 0x1b, 0x9a, 0xfe, 0x34, 0x1f, 0xed, 0x64, 0x1c, 0x4b,
            0x8c, 0xbd, 0xab, 0xd0, 0xa9, 0x76, 0x9a, 0xbf, 0xc1, 0xd3,
            0x15, 0xc6, 0x36, 0x57, 0xcd, 0x8c, 0xa9, 0x2c, 0x5b, 0x33,
            0x0b, 0x99, 0x6c, 0x86, 0x2b, 0x2e, 0x27, 0xf7 } },
        //{{}, {}},
        //{NULL, NULL},
    };


static int aess_decryption_dict(AppleAESSState *s, uint8_t *out, uint8_t *in,
                                bool encrypt)
{
#define COMPARE_SIZE 0x10
    const uint8_t *arr_ptr_src = NULL;
    const uint8_t *arr_ptr_dst = NULL;
    for (int i = 0; i < sizeof(enc_dec_dict) / sizeof(AESS_enc_dec_dict_t);
         i++) {
        DBGLOG("%s: test0: %u: %p %p %u\n", __func__, i, enc_dec_dict[i].enc,
               enc_dec_dict[i].dec, encrypt);
        if (encrypt) {
            arr_ptr_src = enc_dec_dict[i].dec;
            arr_ptr_dst = enc_dec_dict[i].enc;
        } else {
            arr_ptr_src = enc_dec_dict[i].enc;
            arr_ptr_dst = enc_dec_dict[i].dec;
        }
        for (int j = 0; j < 0x30; j += COMPARE_SIZE) {
            HEXDUMP("val0", &arr_ptr_src[j], COMPARE_SIZE);
            HEXDUMP("val1", in, COMPARE_SIZE);
            DBGLOG("\n");
            if (!memcmp(&arr_ptr_src[j], in, COMPARE_SIZE)) {
#if 0
                if (
                    (j == 0x00 && buffer_is_zero(s->iv_dec, sizeof(s->iv_dec))) ||
                    (j != 0x00 && !memcmp(s->iv_dec, &arr_ptr_src[j-0x10], sizeof(s->iv_dec)))
                )
#endif
                {
                    memcpy(out, &arr_ptr_dst[j], COMPARE_SIZE);
                    return true;
                }
            }
        }
    }
    return false;
}

static QCryptoCipherAlgo get_aes_cipher_alg(int flags)
{
    switch (flags & (SEP_AESS_CMD_FLAG_KEYSIZE_AES128 |
                     SEP_AESS_CMD_FLAG_KEYSIZE_AES192 |
                     SEP_AESS_CMD_FLAG_KEYSIZE_AES256)) {
    case SEP_AESS_CMD_FLAG_KEYSIZE_AES128:
        return QCRYPTO_CIPHER_ALGO_AES_128;
    case SEP_AESS_CMD_FLAG_KEYSIZE_AES192:
        return QCRYPTO_CIPHER_ALGO_AES_192;
    case SEP_AESS_CMD_FLAG_KEYSIZE_AES256:
        return QCRYPTO_CIPHER_ALGO_AES_256;
    default:
        break;
    }
    g_assert_not_reached();
    return 0;
}

static void xor_32bit_value(uint8_t *dest, uint32_t val, int size)
{ // size in dwords
    // TODO: ASAN complains about uint32_t*, wants uint16_t* or even uint8_t* ;;
    // was most likely about two single bool's between array, probably fixed.
    uint32_t *ptr = (uint32_t *)dest;
    for (int i = 0; i < size; i++) {
        *ptr ^= val;
        ptr++;
    }
}

// TODO: This is 100% wrong, but it works anyhow/anyway.
// Somewhen, I'll have to handle keyunwrap (if that exists) and PKA. For the PKA
// ECDH command, reuse code from SSC.

static void aess_keywrap_uid(AppleAESSState *s, uint8_t *in, uint8_t *out,
                             QCryptoCipherAlgo cipher_alg)
{ // for keywrap only
    // TODO: Second half of output might be CMAC!!!
    g_assert_cmpuint(cipher_alg, ==, QCRYPTO_CIPHER_ALGO_AES_256);
    QCryptoCipher *cipher;
    uint32_t normalized_cmd = SEP_AESS_CMD_WITHOUT_FLAGS(s->command);
    size_t key_len = qcrypto_cipher_get_key_len(cipher_alg);
    size_t data_len = 0x20;
    g_assert_cmpuint(data_len, ==, 0x20);
    uint8_t used_key[0x20] = { 0 };
    if (normalized_cmd == 0x02 && s->keywrap_uid0_enabled) {
        memcpy(used_key, (uint8_t *)s->keywrap_key_uid0,
               sizeof(used_key)); // for UUID
    } else if (normalized_cmd == 0x12 && s->keywrap_uid1_enabled) {
        memcpy(used_key, (uint8_t *)s->keywrap_key_uid1,
               sizeof(used_key)); // for UUID
    } else if (normalized_cmd == 0x02 || normalized_cmd == 0x12) {
        memcpy(used_key, (uint8_t *)AESS_UID_SEED_NOT_ENABLED,
               sizeof(used_key));
    } else {
        g_assert_not_reached();
    }
    // TODO: Dirty hack, so iteration_register being set/unset shouldn't result
    // in the same output keys.
    xor_32bit_value(&used_key[0x10], s->reg_0x14_keywrap_iterations_counter,
                    0x8 / 4); // seed_bits are only for keywrap
    DBGLOG("%s: s->command: 0x%02x normalized_cmd: 0x%02x cipher_alg: %u; "
           "key_len: %lu; iterations: %u, seed_bits: 0x%02x, "
           "reg_0x18_keydisable: 0x%02x\n", __func__, s->command,
           normalized_cmd, cipher_alg, key_len,
           s->reg_0x14_keywrap_iterations_counter, s->seed_bits,
           s->reg_0x18_keydisable);
    HEXDUMP("aess_keywrap_uid: used_key", used_key, sizeof(used_key));
    HEXDUMP("aess_keywrap_uid: in", in, data_len);
    cipher = qcrypto_cipher_new(cipher_alg, QCRYPTO_CIPHER_MODE_CBC, used_key,
                                key_len, &error_abort);
    g_assert_nonnull(cipher);
    uint8_t iv[0x10] = { 0 };
    memset(iv, 0x00, sizeof(iv));
    qcrypto_cipher_setiv(cipher, iv, sizeof(iv), &error_abort);
    uint8_t enc_temp[0x20] = { 0 };
    memcpy(enc_temp, in, sizeof(enc_temp));
    // TODO: iteration register is actually for the iterations inside the
    // algorithm, not how often the algorihm is being called.
    do {
        qcrypto_cipher_encrypt(cipher, enc_temp, enc_temp, sizeof(enc_temp),
                               &error_abort);
    } while (s->reg_0x14_keywrap_iterations_counter--);
    memcpy(out, enc_temp, data_len);
    HEXDUMP("aess_keywrap_uid: out1", out, data_len);
    s->reg_0x14_keywrap_iterations_counter = 0;
    qcrypto_cipher_free(cipher);
}

static int aess_get_custom_keywrap_index(uint32_t cmd)
{
    switch (cmd) {
    case 0x01:
    case 0x06:
        return 0;
    case 0x41:
    case 0x46:
        return 1;
    case 0x81:
    case 0x08:
    case 0x88:
        return 2;
    case 0xc1:
    case 0x48:
    case 0xc8:
        return 3;
    default:
        g_assert_not_reached();
        // return -1;
    }
}

static bool check_register_0x18_keydisable_bit_invalid(AppleAESSState *s)
{
    ////uint32_t normalized_cmd = SEP_AESS_CMD_WITHOUT_FLAGS(s->command);
    ////uint32_t cmd = s->command;
    uint32_t cmd = SEP_AESS_CMD_WITHOUT_KEYSIZE(s->command);
    bool reg_0x18_keydisable_bit0 = (s->reg_0x18_keydisable & 0x1) != 0;
    bool reg_0x18_keydisable_bit1 = (s->reg_0x18_keydisable & 0x2) != 0;
    bool reg_0x18_keydisable_bit3 = (s->reg_0x18_keydisable & 0x8) != 0;
    bool reg_0x18_keydisable_bit4 = (s->reg_0x18_keydisable & 0x10) != 0;
    ////switch (normalized_cmd)
    switch (cmd) {
    // case 0x: // driver_op == 0x09 (cmd 0x00, invalid)
    case 0x0C:
    case 0x4C:
        // cmd 0x0C or 0x4C might be driver_op 0x09, if it would exist.
        return reg_0x18_keydisable_bit4;
    // driver_op == 0x0A/0x0d (cmds 0x00/0x00, both are invalid)
    case 0x09: // driver_op 0x0A would be most likely cmd 0x09, if using it in
               // the _operate function would be allowed
    case 0x0A: // driver_op 0x0D would be most likely cmd 0x0A, if using it in
               // the _operate function would be allowed
        return reg_0x18_keydisable_bit0;
    // driver_op == 0x0B/0x0e (cmds 0x49/0x00, 0x0E is invalid)
    case 0x49:
    case 0x4A: // driver_op 0x0E would be most likely cmd 0x4A, if using it in
               // the _operate function would be allowed
        return reg_0x18_keydisable_bit1;
    // driver_op == 0x13/0x14 (cmds 0x0D/0x00, 0x14 is invalid)
    case 0x0D: // 0x0D and 0x4D, are those actually implemented in real
               // hardware?
    case 0x4D: // driver_op 0x14 would be most likely cmd 0x4D, if using it in
               // the _operate function would be allowed
        return reg_0x18_keydisable_bit3;
    // driver_op == 0x23/0x24 (cmds 0x50/0x90)
    case 0x50:
    case 0x90:
        // driver_ops 0x23/0x24 are not available on iOS 12, but they're on iOS
        // 14
        return reg_0x18_keydisable_bit3;
    default:
        break;
    }
    return false;
}

static void aess_handle_cmd(AppleAESSState *s)
{
    bool use_aes256 = (s->command & SEP_AESS_CMD_FLAG_KEYSIZE_AES256) != 0;
    bool keyselect_non_gid0 =
        SEP_AESS_CMD_FLAG_KEYSELECT_GID1_CUSTOM(s->command) != 0;
    bool keyselect_gid1 = (s->command & SEP_AESS_CMD_FLAG_KEYSELECT_GID1) != 0;
    bool keyselect_custom = (s->command & SEP_AESS_CMD_FLAG_KEYSELECT_CUSTOM) != 0;
    uint32_t normalized_cmd = SEP_AESS_CMD_WITHOUT_FLAGS(s->command);
    QCryptoCipherAlgo cipher_alg = get_aes_cipher_alg(s->command);
    size_t key_len = qcrypto_cipher_get_key_len(cipher_alg);
    bool zero_iv_two_blocks_encryption = false;
    bool register_0x18_keydisable_bit_invalid =
        check_register_0x18_keydisable_bit_invalid(s);
    bool valid_command = true;
    bool invalid_parameters = register_0x18_keydisable_bit_invalid;
    s->interrupt_status = 0;
#if 1
    memset(s->out_full, 0,
           sizeof(s->out_full)); // not correct behavior, but SEPFW likes to
                                 // complain if it doesn't expect the output to
                                 // be zero, so keep it.
#endif
#if 1
    if (!keyselect_non_gid0 &&
        normalized_cmd ==
            SEP_AESS_COMMAND_0xb) /* not GID1 && not Custom */ // ignore the
                                                               // keysize flags
                                                               // here
    {
        {
            memset(s->key_256_in, 0, sizeof(s->key_256_in));
            memcpy(s->key_256_in, s->in_full, sizeof(s->in_full));
        }
    }
#endif
    else if (!keyselect_non_gid0 &&
             (normalized_cmd == 0x2 ||
              normalized_cmd ==
                  0x12)) /* Not GID1 && not Custom */ // Always AES256!!
    {
#if 1
        cipher_alg = QCRYPTO_CIPHER_ALGO_AES_256;
        key_len = qcrypto_cipher_get_key_len(
            cipher_alg); // VERY important, otherwise key_len would be too short
                         // in case that flag 0x200 is missing.
        // keyselect_gid1 = true; // variable has no use here
        //  key wrapping/deriving data
        uint8_t key_wrap_data_in[0x20] = { 0 };
        uint8_t key_wrap_data_out[0x20] = { 0 };
        memcpy(key_wrap_data_in, s->in_full, key_len);
        // aess_encrypt_decrypt_uid(s, key_wrap_data_in, key_wrap_data_out,
        // cipher_alg, true);
        ////aess_keywrap_uid(s, key_wrap_data_in, key_wrap_data_out, cipher_alg,
        /// false);
        // aess_keywrap_uid(s, key_wrap_data_in, key_wrap_data_out, cipher_alg,
        // true);
        aess_keywrap_uid(s, key_wrap_data_in, key_wrap_data_out, cipher_alg);
        // qcrypto_random_bytes(key_wrap_data_out, sizeof(key_wrap_data_out),
        // NULL); // For testing if random output breaks stuff.
        memcpy(s->out_full, key_wrap_data_out, key_len);
#endif
    }
#if 1
    else if (
        normalized_cmd == SEP_AESS_COMMAND_ENCRYPT_CBC ||
        normalized_cmd == SEP_AESS_COMMAND_DECRYPT_CBC ||
        normalized_cmd == SEP_AESS_COMMAND_ENCRYPT_CBC_FORCE_CUSTOM_AES256 ||
        normalized_cmd ==
            SEP_AESS_COMMAND_ENCRYPT_CBC_ONLY_NONCUSTOM_FORCE_CUSTOM_AES256) /* GID0 || GID1 || Custom */
    {
        bool custom_encryption = false;
        uint32_t original_command = s->command;
        DBGLOG("%s: original_command 0x%03x ; ", __func__, original_command);
        HEXDUMP("s->in_full", s->in_full, sizeof(s->in_full));
        if (normalized_cmd ==
            SEP_AESS_COMMAND_ENCRYPT_CBC_ONLY_NONCUSTOM_FORCE_CUSTOM_AES256) {
            if (keyselect_custom) // 0x80
                goto jump_return; // valid: 0x206, 0x246; invalid: 0x286, 0x2c6
            normalized_cmd = SEP_AESS_COMMAND_ENCRYPT_CBC_FORCE_CUSTOM_AES256;
        }
        if (normalized_cmd ==
            SEP_AESS_COMMAND_ENCRYPT_CBC_FORCE_CUSTOM_AES256) {
            if (!keyselect_custom) {
                zero_iv_two_blocks_encryption = true;
            }
            custom_encryption = true;
            // use_aes256 = true; // variable only used for gid decryption
            keyselect_non_gid0 = true;
            keyselect_gid1 = false;
            keyselect_custom = true;
            normalized_cmd = SEP_AESS_COMMAND_ENCRYPT_CBC;
            cipher_alg = QCRYPTO_CIPHER_ALGO_AES_256;
            key_len = qcrypto_cipher_get_key_len(cipher_alg);
        }
        bool do_encryption = (normalized_cmd == SEP_AESS_COMMAND_ENCRYPT_CBC);
        if (!keyselect_non_gid0) { // GID
            if (use_aes256) {
                uint8_t dict_out[0x10] = { 0 };
                int found =
                    aess_decryption_dict(s, dict_out, s->in_dec, do_encryption);
                if (found) {
                    DBGLOG("%s: aess_decryption_dict: Found it! cmd=0x%x\n",
                           __func__, s->command);
                    memcpy(s->out, dict_out, sizeof(s->out));
                    goto jump_return;
                }
            }
        }
        uint8_t used_key[0x20] = { 0 };
        if (custom_encryption) {
            int custom_keywrap_index =
                aess_get_custom_keywrap_index(s->command & 0xff);
            if (s->custom_key_index_enabled[custom_keywrap_index]) {
                memcpy(used_key, s->custom_key_index[custom_keywrap_index],
                       sizeof(used_key));
            } else {
                memset(used_key, 0, sizeof(used_key));
            }
        } else if (keyselect_custom) { /* Custom takes precedence over GID0 or
                                          GID1 */
            memcpy(used_key, s->key_256_in, sizeof(used_key)); // for custom
        } else {
            if (register_0x18_keydisable_bit_invalid) {
                memcpy(used_key, (uint8_t *)AESS_KEY_FOR_DISABLED_KEY,
                       sizeof(used_key));
            } else if (keyselect_gid1) {
                memcpy(used_key, (uint8_t *)AESS_GID1,
                       sizeof(used_key)); // for GID1
            } else {
                memcpy(used_key, (uint8_t *)AESS_GID0,
                       sizeof(used_key)); // for GID0
            }
        }
        QCryptoCipher *cipher;
        cipher = qcrypto_cipher_new(cipher_alg, QCRYPTO_CIPHER_MODE_CBC,
                                    used_key, key_len, &error_abort);
        g_assert_nonnull(cipher);
        uint8_t iv[0x10] = { 0 };
        uint8_t in[0x10] = { 0 };
        if (do_encryption) {
            memcpy(iv, s->iv, sizeof(iv));
            memcpy(in, s->in, sizeof(in));
            //} else if (normalized_cmd == SEP_AESS_COMMAND_DECRYPT_CBC) {
        } else {
            memcpy(iv, s->iv_dec, sizeof(iv));
            memcpy(in, s->in_dec, sizeof(in));
        }
        if (zero_iv_two_blocks_encryption) {
            memset(iv, 0, sizeof(iv));
            qcrypto_cipher_setiv(
                cipher, iv, sizeof(iv),
                &error_abort); // sizeof(iv) == 0x10 on 256 and 128
            qcrypto_cipher_encrypt(cipher, s->in_full, s->out_full,
                                   sizeof(s->in_full), &error_abort);
            // if ((s->command & 0xf) == 0x9)
        } else if (do_encryption) {
            qcrypto_cipher_setiv(
                cipher, iv, sizeof(iv),
                &error_abort); // sizeof(iv) == 0x10 on 256 and 128
            qcrypto_cipher_encrypt(cipher, s->in, s->out, sizeof(s->in),
                                   &error_abort);
            memcpy(s->tag_out, iv, sizeof(iv));
        } else {
            qcrypto_cipher_decrypt(cipher, in, s->tag_out, sizeof(in),
                                   &error_abort);
            qcrypto_cipher_setiv(
                cipher, iv, sizeof(iv),
                &error_abort); // sizeof(iv) == 0x10 on 256 and 128
            qcrypto_cipher_decrypt(cipher, in, s->out, sizeof(in),
                                   &error_abort);
        }
        qcrypto_cipher_free(cipher);
    }
#endif
#if 1
    else if (normalized_cmd ==
             0x00) // cmd 0x40 == sync seed_bits for keywrap cmd 0x2 ; effect
                   // for wrap/UID, no effect for GID/custom?
    {
        if (keyselect_gid1) {
            memcpy(s->keywrap_key_uid0, (uint8_t *)AESS_UID0,
                   sizeof(s->keywrap_key_uid0)); // for UUID
            xor_32bit_value(&s->keywrap_key_uid0[0x8], s->seed_bits,
                            0x8 / 4); // seed_bits are only for keywrap
            ////xor_32bit_value(&s->keywrap_key_uid0[0x18],
            /// s->reg_0x18_keydisable, 0x8/4);
            // NOT AFFECTED by REG_0x18???
            s->keywrap_uid0_enabled = true;
            qemu_log_mask(LOG_UNIMP,
                          "SEP AESS_BASE: %s: Copied seed_bits for uid0 0x%x\n",
                          __func__, s->seed_bits);
        }
    }
#endif
#if 1
    else if (normalized_cmd ==
             0x10) // cmd 0x50 == sync seed_bits for keywrap cmd 0x12
    {
        if (keyselect_gid1) {
            // this is conditional memcpy is actually needed, because the result
            // will change if reg_0x18_bit3 is set
            if (invalid_parameters) {
                memcpy(s->keywrap_key_uid1, (uint8_t *)AESS_UID_SEED_INVALID,
                       sizeof(s->keywrap_key_uid1));
            } else {
                memcpy(s->keywrap_key_uid1, (uint8_t *)AESS_UID1,
                       sizeof(s->keywrap_key_uid1)); // for UUID
            }
            // this xor should happen, even if invalid_parameters is activated
            xor_32bit_value(&s->keywrap_key_uid1[0x8], s->seed_bits,
                            0x8 / 4); // seed_bits are only for keywrap
            ////// NOT AFFECTED by REG_0x18???
            /// xor_32bit_value(&s->keywrap_key_uid1[0x18],
            /// s->reg_0x18_keydisable, 0x8/4);
            // actually affected by reg_0x18?
            s->keywrap_uid1_enabled = true;
            qemu_log_mask(LOG_UNIMP,
                          "SEP AESS_BASE: %s: Copied seed_bits for uid1 0x%x\n",
                          __func__, s->seed_bits);
        }
    }
#endif
#if 1
    else if (normalized_cmd ==
             0x1) // sync/set key for command 0x206(0x201), 0x246(0x241),
                  // 0x208/0x288(0x281), 0x248/0x2c8(0x2c1)
    {
        int custom_keywrap_index = aess_get_custom_keywrap_index(s->command & 0xff);
        memcpy(s->custom_key_index[custom_keywrap_index], s->in_full,
               sizeof(s->custom_key_index[custom_keywrap_index]));
        xor_32bit_value(
            s->custom_key_index[custom_keywrap_index], 0xdeadbeef,
            0x20 /
                4); // unset (real zero-key) != zero-key set (not real zero-key)
        s->custom_key_index_enabled[custom_keywrap_index] = true;
        qemu_log_mask(
            LOG_UNIMP,
            "SEP AESS_BASE: %s: sync/set key command 0x%02x s->command 0x%02x\n",
            __func__, normalized_cmd, s->command);
    }
#endif
// TODO: other sync commands: 0x205(0x201), 0x204(0x281), 0x245(0x241),
// 0x244(0x2c1)
#if 0
    else if (normalized_cmd == 0x...)
    {
    }
#endif
#if 1
    else {
        qemu_log_mask(LOG_UNIMP, "SEP AESS_BASE: %s: Unknown command 0x%02x\n",
                      __func__, s->command);
        // valid_command = false;
    }
#endif

////s->status |= (1 << 1); // TODO: only on success^H^H^Hfailure
jump_return:
    invalid_parameters |= !valid_command;
    s->interrupt_status =
        ((invalid_parameters << 1) | (s->interrupt_status & 0x2)) |
        (valid_command << 0); // ???? bit1 clear, bit0 set
    ////s->interrupt_status = (invalid_parameters << 1) | (valid_command << 0);
    ///// ????
    /// bit1 clear, bit0 set
    // s->interrupt_status = (0 << 1) | (1 << 0); // ???? bit1 clear, bit0 set
    ////s->interrupt_status = (1 << 1) | (1 << 0); // set from 3 to 1 after the
    /// next read
}

static void aess_base_reg_write(void *opaque, hwaddr addr, uint64_t data,
                                unsigned size)
{
    MachineState *machine = MACHINE(qdev_get_machine());
    AppleSEPState *sep = APPLE_SEP(
        object_property_get_link(OBJECT(machine), "sep", &error_fatal));
    AppleAESSState *s;
    s = (AppleAESSState *)opaque;

#ifdef ENABLE_CPU_DUMP_STATE
    qemu_log_mask(LOG_UNIMP, "\n");
    cpu_dump_state(CPU(sep->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
    case SEP_AESS_REGISTER_STATUS: // Status
        s->status = data;
        if ((s->status & SEP_AESS_REGISTER_STATUS_RUN_COMMAND) != 0) {
            aess_handle_cmd(s);
        }
        goto jump_default;
    case SEP_AESS_REGISTER_COMMAND: // Command
        data &= 0x3ff; // for T8020
        s->command = data;
        goto jump_default;
    case SEP_AESS_REGISTER_INTERRUPT_STATUS: // Interrupt Status
        if ((data & 0x1) != 0) {
            s->interrupt_status &= ~0x1;
        }
        goto jump_default;
    case SEP_AESS_REGISTER_INTERRUPT_ENABLED: // Interrupt Enabled
        data &= 0x3;
        s->interrupt_enabled = data;
        goto jump_default;
    case SEP_AESS_REGISTER_0x14_KEYWRAP_ITERATIONS_COUNTER: // has affect on
                                                            // keywrap
        s->reg_0x14_keywrap_iterations_counter = data;
        goto jump_default;
    case SEP_AESS_REGISTER_0x18_KEYDISABLE: // has affect on keywrap
        data |= s->reg_0x18_keydisable;
        data &= 0x1b;
        s->reg_0x18_keydisable = data;
        goto jump_default;
    case SEP_AESS_REGISTER_SEED_BITS: // seed_bits ;; has affect on keywrap ;;
                                      // offset 0x1c == flags offset: stores
                                      // flags, like if the device has been
                                      // demoted (bit 30). On T8010, the bits
                                      // are between 28 and 31, on T8020, the
                                      // bits are between 27 and 31.
        data &= ~s->seed_bits_lock;
        data |= s->seed_bits & s->seed_bits_lock;
        s->seed_bits = data;
        goto jump_default;
    case SEP_AESS_REGISTER_SEED_BITS_LOCK: // seed_bits_lock ;; has no affect on
                                           // keywrap?
        data |= s->seed_bits_lock; // don't allow unsetting
        s->seed_bits_lock = data;
        goto jump_default;
    case SEP_AESS_REGISTER_IV ... SEP_AESS_REGISTER_IV + 0xc: // IV
    case 0x100 ... 0x10c: // IV T8015
        memcpy(&s->iv[addr & 0xf], &data, 4);
        goto jump_default;
    case SEP_AESS_REGISTER_IN ... SEP_AESS_REGISTER_IN + 0xc: // IN
    case 0x110 ... 0x11c: // IN T8015
        memcpy(&s->in[addr & 0xf], &data, 4);
        goto jump_default;
    // AES engine?: case 0xa4: 0x40 bytes from TRNG
    default:
        memcpy(&sep->aess_base_regs[addr], &data, size);
    jump_default:
#if 0
        qemu_log_mask(LOG_UNIMP,
                      "SEP AESS_BASE: Unknown write at 0x" HWADDR_FMT_plx
                      " with value 0x%" PRIx64 "\n",
                      addr, data);
#endif
        break;
    }
}

static uint64_t aess_base_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    MachineState *machine = MACHINE(qdev_get_machine());
    AppleSEPState *sep = APPLE_SEP(
        object_property_get_link(OBJECT(machine), "sep", &error_fatal));
    AppleAESSState *s;
    s = (AppleAESSState *)opaque;
    uint64_t ret = 0;

#ifdef ENABLE_CPU_DUMP_STATE
    qemu_log_mask(LOG_UNIMP, "\n");
    cpu_dump_state(CPU(sep->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
    case SEP_AESS_REGISTER_STATUS: // Status
        s->status &= ~(1 << 1);
        s->status |= 0x100; // ???
        ret = s->status;
        goto jump_default;
    case SEP_AESS_REGISTER_COMMAND: // Command
        ret = s->command;
        goto jump_default;
    case SEP_AESS_REGISTER_INTERRUPT_STATUS: // Interrupt Status
        ret = s->interrupt_status;
        goto jump_default;
    case SEP_AESS_REGISTER_INTERRUPT_ENABLED: // Interrupt Enabled
        ret = s->interrupt_enabled;
        goto jump_default;
    case SEP_AESS_REGISTER_0x14_KEYWRAP_ITERATIONS_COUNTER:
        ret = s->reg_0x14_keywrap_iterations_counter;
        goto jump_default;
    case SEP_AESS_REGISTER_0x18_KEYDISABLE:
        ret = s->reg_0x18_keydisable;
        goto jump_default;
    case SEP_AESS_REGISTER_SEED_BITS: // seed_bits
        ret = s->seed_bits;
        goto jump_default;
    case SEP_AESS_REGISTER_SEED_BITS_LOCK: // seed_bits_lock
        ret = s->seed_bits_lock;
        goto jump_default;
    case SEP_AESS_REGISTER_IV ... SEP_AESS_REGISTER_IV + 0xc: // IV
        ////case 0x100 ... 0x10c: // IV T8015 ; is this also being read?
        memcpy(&ret, &s->iv[addr & 0xf], 4);
        goto jump_default;
    case SEP_AESS_REGISTER_IN ... SEP_AESS_REGISTER_IN + 0xc: // IN
        ////case 0x110 ... 0x11c: // IN T8015 ; is this also being read?
        memcpy(&ret, &s->in[addr & 0xf], 4);
        goto jump_default;
    case SEP_AESS_REGISTER_TAG_OUT ... SEP_AESS_REGISTER_TAG_OUT +
        0xc: // TAG OUT
        memcpy(&ret, &s->tag_out[addr & 0xf], 4);
        goto jump_default;
    case SEP_AESS_REGISTER_OUT ... SEP_AESS_REGISTER_OUT + 0xc: // OUT
        memcpy(&ret, &s->out[addr & 0xf], 4);
        goto jump_default;
    case 0xe4: // ????
        ret = 0x0;
        goto jump_default;
    case 0x280: // ????
        ret = 0x1;
        goto jump_default;
    default:
        memcpy(&ret, &sep->aess_base_regs[addr], size);
    jump_default:
#if 0
        qemu_log_mask(LOG_UNIMP,
                      "SEP AESS_BASE: Unknown read at 0x" HWADDR_FMT_plx
                      " with value 0x%" PRIx64 "\n",
                      addr, ret);
#endif
        break;
    }

    return ret;
}

static const MemoryRegionOps aess_base_reg_ops = {
    .write = aess_base_reg_write,
    .read = aess_base_reg_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.unaligned = false,
};

static void aesh_base_reg_write(void *opaque, hwaddr addr, uint64_t data,
                               unsigned size)
{
    AppleSEPState *s = APPLE_SEP(opaque);

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
    default:
        memcpy(&s->aesh_base_regs[addr], &data, size);
    jump_default:
#if 0
        qemu_log_mask(LOG_UNIMP,
                      "SEP AESH_BASE: Unknown write at 0x" HWADDR_FMT_plx
                      " with value 0x%" PRIx64 "\n",
                      addr, data);
#endif
        break;
    }
}

static uint64_t aesh_base_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    AppleSEPState *s = APPLE_SEP(opaque);
    uint64_t ret = 0;

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
    default:
        memcpy(&ret, &s->aesh_base_regs[addr], size);
    jump_default:
#if 0
        qemu_log_mask(LOG_UNIMP,
                      "SEP AESH_BASE: Unknown read at 0x" HWADDR_FMT_plx
                      " with value 0x%" PRIx64 "\n",
                      addr, ret);
#endif
        break;
    }

    return ret;
}

static const MemoryRegionOps aesh_base_reg_ops = {
    .write = aesh_base_reg_write,
    .read = aesh_base_reg_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.unaligned = false,
};

static void pka_base_reg_write(void *opaque, hwaddr addr, uint64_t data,
                               unsigned size)
{
    MachineState *machine = MACHINE(qdev_get_machine());
    AppleSEPState *sep = APPLE_SEP(
        object_property_get_link(OBJECT(machine), "sep", &error_fatal));
    ApplePKAState *s;
    s = (ApplePKAState *)opaque;

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(sep->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
    case 0x0: // maybe command
        // values: 0x4/0x8/0x10/0x20/0x40/0x80/0x100
        goto jump_default;
    case 0x4: // maybe status_out0
#if 0
        s->status0 = data;
        if (s->status0 == 0x1) {
            s->status_in0 = 1;
        }
#endif
        goto jump_default;
    case 0x40: // img4out DGST clock
        s->img4out_dgst_clock = data;
        goto jump_default;
    case 0x60 ... 0x7c: // img4out DGST data
        goto jump_default;
    case 0x80 ... 0x9c: // some data
        goto jump_default;
    case 0x800: // chip revision clock
        s->chip_revision_clock = data;
        goto jump_default;
    case 0x820: // chip revision data
        goto jump_default;
    case 0x840: // chipid ecid misc clock
        s->chipid_ecid_misc_clock = data;
        goto jump_default;
    case 0x860 ... 0x864: // ecid data
        goto jump_default;
    case 0x868: // unknown0 data
        goto jump_default;
    case 0x86c: // unknown1 data
        goto jump_default;
    case 0x870: // chipid data
        goto jump_default;
    default:
        memcpy(&sep->pka_base_regs[addr], &data, size);
    jump_default:
#if 0
        qemu_log_mask(LOG_UNIMP,
                      "SEP PKA_BASE: Unknown write at 0x" HWADDR_FMT_plx
                      " with value 0x%" PRIx64 "\n",
                      addr, data);
#endif
        break;
    }
}

static uint64_t pka_base_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    MachineState *machine = MACHINE(qdev_get_machine());
    AppleSEPState *sep = APPLE_SEP(
        object_property_get_link(OBJECT(machine), "sep", &error_fatal));
    ApplePKAState *s;
    s = (ApplePKAState *)opaque;
    uint64_t ret = 0;

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(sep->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
    case 0x8: // maybe status_in0
#if 0
        if (s->status0 == 0x1) {
            ret = 0x1;
        }
#endif
#if 0
        ret = s->status_in0;
        if (s->status_in0 == 1) {
            s->status_in0 = 0;
        }
#endif
        goto jump_default;
    case 0x40: // img4out DGST clock
        ret = s->img4out_dgst_clock;
        goto jump_default;
    case 0x800: // chip revision clock
        ret = s->chip_revision_clock;
        goto jump_default;
    case 0x840: // chipid ecid misc clock
        ret = s->chipid_ecid_misc_clock;
        goto jump_default;
    default:
        memcpy(&ret, &sep->pka_base_regs[addr], size);
    jump_default:
#if 0
        qemu_log_mask(LOG_UNIMP,
                      "SEP PKA_BASE: Unknown read at 0x" HWADDR_FMT_plx
                      " with value 0x%" PRIx64 "\n",
                      addr, ret);
#endif
        break;
    }

    return ret;
}

static const MemoryRegionOps pka_base_reg_ops = {
    .write = pka_base_reg_write,
    .read = pka_base_reg_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.unaligned = false,
};

static void pka_tmm_reg_write(void *opaque, hwaddr addr, uint64_t data,
                               unsigned size)
{
    AppleSEPState *s = APPLE_SEP(opaque);

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
    default:
        memcpy(&s->pka_tmm_regs[addr], &data, size);
    jump_default:
#if 0
        qemu_log_mask(LOG_UNIMP,
                      "SEP PKA_TMM: Unknown write at 0x" HWADDR_FMT_plx
                      " with value 0x%" PRIx64 "\n",
                      addr, data);
#endif
        break;
    }
}

static uint64_t pka_tmm_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    AppleSEPState *s = APPLE_SEP(opaque);
    uint64_t ret = 0;

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
    default:
        memcpy(&ret, &s->pka_tmm_regs[addr], size);
    jump_default:
#if 0
        qemu_log_mask(LOG_UNIMP,
                      "SEP PKA_TMM: Unknown read at 0x" HWADDR_FMT_plx
                      " with value 0x%" PRIx64 "\n",
                      addr, ret);
#endif
        break;
    }

    return ret;
}

static const MemoryRegionOps pka_tmm_reg_ops = {
    .write = pka_tmm_reg_write,
    .read = pka_tmm_reg_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.unaligned = false,
};

static void misc0_reg_write(void *opaque, hwaddr addr, uint64_t data,
                            unsigned size)
{
    AppleSEPState *s = APPLE_SEP(opaque);

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
    // Maybe the SHA engine: case 0xb4: 0x40 bytes from TRNG
    default:
        memcpy(&s->misc0_regs[addr], &data, size);
        qemu_log_mask(LOG_UNIMP,
                      "SEP MISC0: Unknown write at 0x" HWADDR_FMT_plx
                      " with value 0x%" PRIx64 "\n",
                      addr, data);
        break;
    }
}

static uint64_t misc0_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    AppleSEPState *s = APPLE_SEP(opaque);
    uint64_t ret = 0;

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
    case 0xc: // ???? bit1 clear, bit0 set
        return (0 << 1) | (1 << 0);
    case 0xf4: // ????
        return 0x0;
    default:
        memcpy(&ret, &s->misc0_regs[addr], size);
        qemu_log_mask(LOG_UNIMP,
                      "SEP MISC0: Unknown read at 0x" HWADDR_FMT_plx "\n",
                      addr);
        break;
    }

    return ret;
}

static const MemoryRegionOps misc0_reg_ops = {
    .write = misc0_reg_write,
    .read = misc0_reg_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.unaligned = false,
};

static void misc2_reg_write(void *opaque, hwaddr addr, uint64_t data,
                            unsigned size)
{
    AppleSEPState *s = APPLE_SEP(opaque);

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
    // Some engine?: case 0x28: 0x8 bytes from TRNG
    default:
        memcpy(&s->misc2_regs[addr], &data, size);
        qemu_log_mask(LOG_UNIMP,
                      "SEP MISC2: Unknown write at 0x" HWADDR_FMT_plx
                      " with value 0x%" PRIx64 "\n",
                      addr, data);
        break;
    }
}

static uint64_t misc2_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    AppleSEPState *s = APPLE_SEP(opaque);
    uint64_t ret = 0;

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
    case 0x24: // ????
        return 0x0;
    default:
        memcpy(&ret, &s->misc2_regs[addr], size);
        qemu_log_mask(LOG_UNIMP,
                      "SEP MISC2: Unknown read at 0x" HWADDR_FMT_plx "\n",
                      addr);
        break;
    }

    return ret;
}

static const MemoryRegionOps misc2_reg_ops = {
    .write = misc2_reg_write,
    .read = misc2_reg_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.unaligned = false,
};

static void boot_monitor_reg_write(void *opaque, hwaddr addr, uint64_t data,
                            unsigned size)
{
    AppleSEPState *s = APPLE_SEP(opaque);

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
    case 0x04: // some status flag, bit0
        data &= ~(1 << 0); // reset bit0 for read
        QEMU_FALLTHROUGH;
    case 0x08: // maybe some command0?
    case 0x10: // maybe some command1?
    case 0x20: // load address low
    case 0x24: // load address high
    case 0x28: // end address low
    case 0x2c: // end address high
    case 0x30: // unknown1 address low
    case 0x34: // unknown1 address high
    case 0x38: // unknown2 address low
    case 0x3c: // unknown2 address high
    case 0x40: // unknown0 address low
    case 0x44: // unknown0 address high
    case 0x48: // randomness low
    case 0x4c: // randomness high
        //goto jump_default;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "SEP Boot Monitor: Unknown write at 0x" HWADDR_FMT_plx
                      " with value 0x%" PRIx64 "\n",
                      addr, data);
    jump_default:
        memcpy(&s->boot_monitor_regs[addr], &data, size);
        break;
    }
}

static uint64_t boot_monitor_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    AppleSEPState *s = APPLE_SEP(opaque);
    uint64_t ret = 0;

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
    case 0x04: // some status flag, bit0
        goto jump_default;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "SEP Boot Monitor: Unknown read at 0x" HWADDR_FMT_plx "\n",
                      addr);
    jump_default:
        memcpy(&ret, &s->boot_monitor_regs[addr], size);
        break;
    }

    return ret;
}

static const MemoryRegionOps boot_monitor_reg_ops = {
    .write = boot_monitor_reg_write,
    .read = boot_monitor_reg_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.unaligned = false,
};


void enable_trace_buffer(AppleSEPState *s)
{
    qemu_log_mask(
        LOG_UNIMP,
        "SEP MISC4: Enable Trace Buffer: s->shmbuf_base: 0x" HWADDR_FMT_plx
        "\n",
        s->shmbuf_base);
    if (!s->shmbuf_base)
        return;
    AddressSpace *nsas = &address_space_memory;
    typedef struct QEMU_PACKED {
        uint32_t name;
        uint32_t size;
        uint64_t offset;
    } shm_region_t;
#ifdef SEP_ENABLE_OVERWRITE_SHMBUF_OBJECTS
    shm_region_t shm_region_TRAC = { 0 };
    g_assert_cmpuint(sizeof(shm_region_TRAC), ==, 0x10);
    shm_region_TRAC.name = 'TRAC';
    shm_region_TRAC.size = s->debug_trace_size;
    shm_region_TRAC.offset = s->trace_buffer_base_offset;
    shm_region_t shm_region_null = { 0 };
    g_assert_cmpuint(sizeof(shm_region_null), ==, 0x10);
    shm_region_null.name = 'null';
    uint32_t region_SCOT_size = 0x4000;
    address_space_write(nsas, s->shmbuf_base + 0x14, MEMTXATTRS_UNSPECIFIED,
                        &region_SCOT_size, sizeof(region_SCOT_size));
    address_space_write(nsas, s->shmbuf_base + 0x20, MEMTXATTRS_UNSPECIFIED,
                        &shm_region_TRAC, sizeof(shm_region_TRAC));
    address_space_write(nsas, s->shmbuf_base + 0x30, MEMTXATTRS_UNSPECIFIED,
                        &shm_region_null, sizeof(shm_region_null));
    address_space_set(nsas, s->shmbuf_base + 0xc000 + 0x20, 0,
                      region_SCOT_size - 0x20,
                      MEMTXATTRS_UNSPECIFIED); // clean up SCOT a bit
#endif
    typedef struct QEMU_PACKED {
        uint64_t name;
        uint64_t size;
        uint8_t maybe_permissions; // 0x04/0x06/0x16 // (arg5 & 1) != 0
                                   // create_object panic? ;; maybe permissions
        uint8_t arg6; // 0x00/0x02/0x06 // >= 0x03 create_object panic?
        uint8_t arg7; // 0x01/0x02/0x03/0x04/0x05/0x0d/0x0e/0x0f/0x10 // if
                      // (arg7 != 0) create_object data_346d0 checking block ;;
                      // maybe module_index
        uint8_t pad0;
        uint32_t unkn1; // maybe segment name like _dat, _asc, STAK, TEXT, PMGR
                        // or _hep.
        uint64_t phys;
        uint32_t phys_module_name; // phys module name like EISP
        uint32_t phys_region_name; // phys region name like BASE
        uint64_t virt_mapping_next; // sepos_virt_mapping_t
        uint64_t virt_mapping_previous; // sepos_virt_mapping_t.next or
                                        // object_mappings_t.virt_mapping_next
        uint64_t acl_next; // sepos_acl_t
        uint64_t acl_previous; // sepos_acl_t.next or object_mappings_t.acl_next
    } object_mappings_t;
    typedef struct QEMU_PACKED {
        uint64_t object_mapping; // object_mappings_t
        uint64_t maybe_virt_base;
        uint8_t sending_pid;
        uint8_t maybe_permissions; // maybe permissions ;; data0
        uint8_t maybe_subregion; // 0x00/0x01/0x02 ;; data1
        uint8_t pad0;
        uint32_t pad1;
        uint64_t module_next; // sepos_virt_mapping_t
        uint64_t module_previous; // sepos_virt_mapping_t.next
        uint64_t all_next; // sepos_virt_mapping_t
        uint64_t all_previous; // sepos_virt_mapping_t.all_next
    } sepos_virt_mapping_t;
    typedef struct QEMU_PACKED {
        uint32_t maybe_module_id; // 0x2/0x3/0x4/10001
        uint32_t acl; // 0x4/0x6/0x14/0x16
        uint64_t next; // sepos_acl_t
        uint64_t previous; // sepos_acl_t.next
    } sepos_acl_t;
    object_mappings_t object_mapping_TRAC = { 0 };
    g_assert_cmpuint(sizeof(object_mapping_TRAC), ==, 0x48);
    sepos_acl_t acl_for_TRAC = { 0 };
    g_assert_cmpuint(sizeof(acl_for_TRAC), ==, 0x18);
    sepos_virt_mapping_t virt_mapping_for_TRAC = { 0 };
    g_assert_cmpuint(sizeof(virt_mapping_for_TRAC), ==, 0x38);

// SEPOS_PHYS_BASEs: not in runtime, but while in SEPROM. Same on T8020
// (0x340611ba8-0x11ba8)
// get this with gdb, prerequisite is disabling aslr(?):
// b *0x<sepos_module_start_function> ; gva2gpa 0x<sepos_module_start_function>
// the result minus <sepos_module_start_function>
// maybe it's not that easy to disable the SEPOS module ASLR under iOS 15:
// so instead make breakpoints for the second (or both) eret and do "si".
#define SEPOS_PHYS_BASE_T8015 0x3404a4000ull
#define SEPOS_PHYS_BASE_T8020_IOS14 0x340600000ull
#define SEPOS_PHYS_BASE_T8020_IOS15 0x340710000ull
#define SEPOS_PHYS_BASE_T8030_IOS14 0x340634000ull
// for T8020/T8030 SEPFW of early 14 and 14.7.1
#define SEPOS_OBJECT_MAPPING_BASE_VERSION_IOS14 0x198d0
#define SEPOS_OBJECT_MAPPING_BASE_VERSION_IOS15 0x1d748
#define SEPOS_OBJECT_MAPPING_INDEX 7
// #define SEPOS_VIRT_MAPPING_BASE 0x282d0
// #define SEPOS_VIRT_MAPPING_INDEX 555
// for T8020/T8030 SEPFW of early 14 and 14.7.1
#define SEPOS_ACL_BASE_VERSION_IOS14 0x140d0
#define SEPOS_ACL_BASE_VERSION_IOS15 0x18348
#define SEPOS_ACL_INDEX 19

    uint64_t sepos_phys_base = 0x0;
    uint64_t sepos_object_mapping_base = 0x0;
    uint64_t sepos_acl_base = 0x0;
#ifdef SEP_USE_IOS14_OVERRIDE
    sepos_object_mapping_base = SEPOS_OBJECT_MAPPING_BASE_VERSION_IOS14;
    sepos_acl_base = SEPOS_ACL_BASE_VERSION_IOS14;
#else
    sepos_object_mapping_base = SEPOS_OBJECT_MAPPING_BASE_VERSION_IOS15;
    sepos_acl_base = SEPOS_ACL_BASE_VERSION_IOS15;
#endif
    if (s->chip_id == 0x8015) {
        sepos_phys_base = SEPOS_PHYS_BASE_T8015;
    } else if (s->chip_id >= 0x8020) {
#ifdef SEP_USE_IOS14_OVERRIDE
        //sepos_phys_base = SEPOS_PHYS_BASE_T8020_IOS14;
        sepos_phys_base = SEPOS_PHYS_BASE_T8030_IOS14;
#else
        sepos_phys_base = SEPOS_PHYS_BASE_T8020_IOS15;
#endif
    }
#ifdef SEP_USE_IOS14_OVERRIDE
    object_mapping_TRAC.name = 'TRAC';
    object_mapping_TRAC.size = s->debug_trace_size;
    object_mapping_TRAC.maybe_permissions = 0x06;
    object_mapping_TRAC.arg6 = 0x00;
    object_mapping_TRAC.arg7 = 0x01;
    object_mapping_TRAC.unkn1 = '_dat';
    object_mapping_TRAC.phys = s->shmbuf_base + s->trace_buffer_base_offset;
    object_mapping_TRAC.virt_mapping_previous =
        sepos_object_mapping_base +
        (sizeof(object_mappings_t) * SEPOS_OBJECT_MAPPING_INDEX) +
        offsetof(object_mappings_t, virt_mapping_next);
    // object_mapping_TRAC.virt_mapping_next = SEPOS_VIRT_MAPPING_BASE +
    // (sizeof(sepos_virt_mapping_t) * SEPOS_VIRT_MAPPING_INDEX);
    // object_mapping_TRAC.virt_mapping_previous = SEPOS_VIRT_MAPPING_BASE +
    // (sizeof(sepos_virt_mapping_t) * SEPOS_VIRT_MAPPING_INDEX) +
    // offsetof(sepos_virt_mapping_t, module_next);
    object_mapping_TRAC.acl_next =
        sepos_acl_base + (sizeof(sepos_acl_t) * SEPOS_ACL_INDEX);
    object_mapping_TRAC.acl_previous = sepos_acl_base +
                                       (sizeof(sepos_acl_t) * SEPOS_ACL_INDEX) +
                                       offsetof(sepos_acl_t, next);
    address_space_write(
        nsas,
        sepos_phys_base + sepos_object_mapping_base +
            (sizeof(object_mappings_t) * SEPOS_OBJECT_MAPPING_INDEX),
        MEMTXATTRS_UNSPECIFIED, &object_mapping_TRAC,
        sizeof(object_mapping_TRAC));
    acl_for_TRAC.maybe_module_id = 10001;
    ////acl_for_TRAC.maybe_module_id = 55; // non-existant
    acl_for_TRAC.acl = 0x6;
    acl_for_TRAC.previous =
        sepos_object_mapping_base +
        (sizeof(object_mappings_t) * SEPOS_OBJECT_MAPPING_INDEX) +
        offsetof(object_mappings_t, acl_next);
    address_space_write(nsas,
                        sepos_phys_base + sepos_acl_base +
                            (sizeof(sepos_acl_t) * SEPOS_ACL_INDEX),
                        MEMTXATTRS_UNSPECIFIED, &acl_for_TRAC,
                        sizeof(acl_for_TRAC));
#endif
    // alternative bypass as if_module_AAES_Debu_or_SEPD is also used by other
    // functions, more restrictive.
    uint32_t value32_nop = 0xd503201f; // nop
    uint64_t bypass_offset = 0;
    if (s->chip_id >= 0x8020) {
#ifdef SEP_USE_IOS14_OVERRIDE
        //bypass_offset = 0x11bb0; // T8020 iOS14
        bypass_offset = 0x11b34; // T8030 iOS14
#else
        bypass_offset = 0x12fb4; // T8020 iOS15
#endif
    } else if (s->chip_id == 0x8015) {
        // T8015's SEPFW SEPOS is not reachable from SEPROM, it's LZVN
        // compressed.
        bypass_offset = 0x11c2c; // T8015
    }
    address_space_write(nsas, sepos_phys_base + bypass_offset,
                        MEMTXATTRS_UNSPECIFIED, &value32_nop,
                        sizeof(value32_nop));
}

static void apple_sep_send_message(AppleSEPState *s, uint8_t ep, uint8_t tag,
                                   uint8_t op, uint8_t param, uint32_t data)
{
    AppleA7IOP *a7iop;
    AppleA7IOPMessage *sent_msg;
    SEPMessage *sent_sep_msg;

    a7iop = APPLE_A7IOP(s);

    sent_msg = g_new0(AppleA7IOPMessage, 1);
    sent_sep_msg = (SEPMessage *)sent_msg->data;
    sent_sep_msg->ep = ep;
    sent_sep_msg->tag = tag;
    sent_sep_msg->op = op;
    sent_sep_msg->param = param;
    sent_sep_msg->data = data;
    ////apple_a7iop_send_ap(a7iop, sent_msg);
    apple_a7iop_send_iop(a7iop, sent_msg);
}

static void progress_reg_write(void *opaque, hwaddr addr, uint64_t data,
                            unsigned size)
{
    AppleSEPState *s = APPLE_SEP(opaque);
    struct sep_message sep_msg = { 0 };

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
    case 0x4:
        if (data ==
            0xf2e31133) // iBoot would send those requests. iOS warns about the
                        // responses, because it doesn't expect them.
        {
            sep_msg.endpoint = 0xff;

            sep_msg.opcode = 3; // kOpCode_GenerateNonce
            sep_msg.tag = 0x67;
            ////sep_msg.opcode = 4; // kOpCode_ReportNonceWord
            // memcpy(msg0->data, sep_msg, 16);
            // apple_mbox_inbox_push(s->mbox, msg0);
            // IOP_LOG_MSG(s->mbox, "SEP MISC4: Sent fake
            // SEPROM_Opcode3/kOpCode_GenerateNonce", msg0);
            // apple_mbox_send_inbox_control_message(s->mbox, 0, sep_msg.raw);
            apple_sep_send_message(s, 0xff, 0x67, 3, 0x00, 0x00);
            qemu_log_mask(
                LOG_UNIMP,
                "SEP Progress: Sent fake SEPROM_Opcode3/kOpCode_GenerateNonce\n");


            sep_msg.opcode = 17; // Opcode 17
            sep_msg.tag = 0x0;
            sep_msg.data = 0x8000; // SEPFW on iOS 14.0/14.4.2 for T8020, if I
                                   // found the correct data in Ghidra.
            // apple_mbox_send_inbox_control_message(s->mbox, 0, sep_msg.raw);
            apple_sep_send_message(s, 0xff, 0x0, 17, 0x00, 0x8000);
            qemu_log_mask(LOG_UNIMP, "SEP MISC4: Sent fake SEPROM_Opcode17\n");
        }
        if ((data == 0xFC4A2CAC || data == 0xeee6ba79) && (s->chip_id >= 0x8020)) // Enable Trace Buffer
        {
            // Only works for T8020, because the T8015 SEPOS is compressed.
#ifdef SEP_ENABLE_TRACE_BUFFER
            enable_trace_buffer(s);
#endif
        }
        break;
    case 0x8:
        if (data == 0x23BFDFE7) {
            hwaddr phys_addr = 0x0;
            // easy way of retrieving the sepb random_0 address T8020:
            // b *0x340000000 ; p/x $x0+0x80 == e.g. 0x340736380
            // easy way of retrieving the sepb random_0 address T8030:
            // go to the first SYS_ACC_PWR_DN_SAVE read in the kernel, and
            // then do p/x $x0+0x80 == e.g. 0x3407ca380
            if (s->chip_id == 0x8015) {
                phys_addr = 0x34015FD40ull; // T8015
            } else if (s->chip_id >= 0x8020) {
#ifdef SEP_USE_IOS14_OVERRIDE
                //phys_addr = 0x340736380ull; // T8020 iOS 14
                phys_addr = 0x3407ca380ull; // T8030 iOS 14
#else
                phys_addr = 0x34086e380ull; // T8020 iOS 15
#endif
            } else {
                // g_assert_not_reached();
            }
            if (phys_addr) {
                AddressSpace *nsas = &address_space_memory;
                // The first 16bytes of SEPB.random_0 are being used for SEPOS'
                // ASLR. GDB's awatch refuses to tell me where it ends up, so
                // here you go, I'm just zeroing that shit. == This disables
                // ASLR for SEPOS apps
                // Future iOS versions might use more than 16 bytes, so zero
                // the whole field here.
#ifdef SEP_DISABLE_ASLR
                address_space_set(nsas, phys_addr, 0, 0x40,
                                  MEMTXATTRS_UNSPECIFIED); // phys_SEPB + 0x80;
                                                           // pc==0x240005BAC
#endif
            }
        }
        if (data == 0x41a7 && (s->chip_id >= 0x8015)) {
            DBGLOG("%s: SEPFW_copy_test0: 0x" HWADDR_FMT_plx " 0x%" PRIx64 "\n",
                   __func__, s->sep_fw_addr, s->sep_fw_size);
            AddressSpace *nsas = &address_space_memory;
#ifdef SEP_ENABLE_HARDCODED_FIRMWARE
            address_space_write(nsas, s->sep_fw_addr, MEMTXATTRS_UNSPECIFIED,
                                s->sepfw_data, s->sep_fw_size);
#endif
            // g_free(sep_fw);
        }
#if 1
        // if (data == 0x6A5D128D && (s->chip_id == 0x8015))
        if (data == 0x6A5D128D) {
            AppleA7IOPMessage *msg = NULL;
            msg = apple_a7iop_inbox_peek(APPLE_A7IOP(s)->iop_mailbox);
            if (msg) {
                memcpy(&sep_msg.raw, msg->data, 8);
                uint64_t shmbuf_base = (uint64_t)sep_msg.data << 12;
                qemu_log_mask(LOG_UNIMP,
                              "%s: SHMBUF_TEST0: trace_data8:0x%" PRIx64 ": "
                              "shmbuf=0x" HWADDR_FMT_plx
                              ": ep=0x%02x, tag=0x%02x, opcode=0x%02x(%u), "
                              "param=0x%02x, data=0x%08x\n",
                              APPLE_A7IOP(s)->iop_mailbox->role, data,
                              shmbuf_base, sep_msg.endpoint, sep_msg.tag,
                              sep_msg.opcode, sep_msg.opcode, sep_msg.param,
                              sep_msg.data);
                int debug_trace_mmio_index = -1;
                if (s->chip_id == 0x8015) {
                    debug_trace_mmio_index = 11;
                } else if (s->chip_id >= 0x8020) {
                    debug_trace_mmio_index = 14;
                }
                if (debug_trace_mmio_index != -1) {
                    s->shmbuf_base = shmbuf_base;
                    uint64_t tracebuf_mmio_addr =
                        shmbuf_base + s->trace_buffer_base_offset;
                    qemu_log_mask(
                        LOG_UNIMP,
                        "%s: SHMBUF_TEST1: tracbuf=0x" HWADDR_FMT_plx "\n",
                        APPLE_A7IOP(s)->iop_mailbox->role, tracebuf_mmio_addr);
// if SEP_ENABLE_DEBUG_TRACE_MAPPING
// TODO: T8020 isn't handled here anymore, but T8015 probably still should.
// endif
                }
            }
        }
#endif
        if (data == 0x23BFDFE7 && (s->chip_id == 0x8015)) {
#define LVL3_BASE_COPYFROM 0x24090c000ull
            AddressSpace *nsas = &address_space_memory;
            uint64_t pagetable_val = 0;
            for (uint64_t page_addr = 0x340000000ull;
                 page_addr < 0x342000000ull; page_addr += 0x4000) {
                pagetable_val = page_addr | 0x603;
                address_space_write(nsas,
                                    LVL3_BASE_COPYFROM +
                                        (((page_addr >> 14) & 0x7FF) * 8),
                                    MEMTXATTRS_UNSPECIFIED, &pagetable_val,
                                    sizeof(pagetable_val));
            }
        }
        break;
    case 0x0:
        memcpy(&s->progress_regs[addr], &data, size);
#if 0
        qemu_log_mask(LOG_UNIMP,
                      "SEP Progress: Progress_0 write at 0x" HWADDR_FMT_plx
                      " with value 0x%" PRIx64 "\n",
                      addr, data);
#endif
        if (data == 0xDEADBEE0) {
            qemu_irq_lower(qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_IRQ));
        }
        if (data == 0xDEADBEE1) {
            qemu_irq_lower(qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_FIQ));
        }
        if (data == 0xDEADBEE2) {
            qemu_irq_lower(qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_VIRQ));
        }
        if (data == 0xDEADBEE3) {
            qemu_irq_lower(qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_VFIQ));
        }

        if (data == 0xDEADBEE4) {
            qemu_irq_raise(qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_IRQ));
        }
        if (data == 0xDEADBEE5) {
            qemu_irq_raise(qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_FIQ));
        }
        if (data == 0xDEADBEE6) {
            qemu_irq_raise(qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_VIRQ));
        }
        if (data == 0xDEADBEE7) {
            qemu_irq_raise(qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_VFIQ));
        }
        if (data == 0xCAFE1337) {
            uint32_t i = 0;
            for (i = 0x10000; i < 0x10200; i++) {
                if (i == 0x10008 || i == 0x1002c)
                    continue;
                apple_a7iop_interrupt_status_push(APPLE_A7IOP(s)->iop_mailbox,
                                                  i);
            }
            for (i = 0x40000; i < 0x40100; i++) {
                if (i == 0x40000)
                    continue;
                apple_a7iop_interrupt_status_push(APPLE_A7IOP(s)->iop_mailbox,
                                                  i);
            }
            for (i = 0x70000; i < 0x70400; i++) {
                // if (i == 0x70001)
                //     continue;
                apple_a7iop_interrupt_status_push(APPLE_A7IOP(s)->iop_mailbox,
                                                  i);
            }
        }
        break;
    case 0x3370:
        memcpy(&s->progress_regs[addr], &data, size);
        qemu_log_mask(LOG_UNIMP,
                      "SEP Progress: Progress_1 write at 0x" HWADDR_FMT_plx
                      " with value 0x%" PRIx64 "\n",
                      addr, data);
        // apple_mbox_set_custom0(s->mbox, data);
        apple_a7iop_interrupt_status_push(APPLE_A7IOP(s)->iop_mailbox, data);
        break;
    // case 0x4:
    // case 0x8:
    case 0x114:
    case 0x214:
    case 0x218:
    case 0x21c:
    case 0x220:
    case 0x2d8:
    case 0x2dc:
    case 0x2e0: // ecid low
    case 0x2e4: // ecid high
    case 0x2e8: // board-id
    case 0x2ec: // chip-id
    case 0x314:
    case 0x318:
    case 0x31c:
        memcpy(&s->progress_regs[addr], &data, size);
        break;
    default:
        // jump_default:
        memcpy(&s->progress_regs[addr], &data, size);
#if 0
        qemu_log_mask(LOG_UNIMP,
                      "SEP Progress: Unknown write at 0x" HWADDR_FMT_plx
                      " with value 0x%" PRIx64 "\n",
                      addr, data);
#endif
        break;
    }
}

static uint64_t progress_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    AppleSEPState *s = APPLE_SEP(opaque);
    uint64_t ret = 0;

#ifdef ENABLE_CPU_DUMP_STATE
    cpu_dump_state(CPU(s->cpu), stderr, CPU_DUMP_CODE);
#endif
    switch (addr) {
    default:
        memcpy(&ret, &s->progress_regs[addr], size);
#if 0
        qemu_log_mask(LOG_UNIMP,
                      "SEP Progress: Unknown read at 0x" HWADDR_FMT_plx "\n",
                      addr);
#endif
        break;
    }

    return ret;
}

static const MemoryRegionOps progress_reg_ops = {
    .write = progress_reg_write,
    .read = progress_reg_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.unaligned = false,
};

static void apple_sep_cpu_reset_work(CPUState *cpu, run_on_cpu_data data);
#if 0

static void apple_sep_cpu_reset_work2(CPUState *cpu, run_on_cpu_data data)
{
    vaddr load_addr = (vaddr)data.host_ptr;
    DBGLOG("apple_sep_cpu_reset_work2: before cpu_set_pc: base=0x" HWADDR_FMT_plx
           "\n",
           load_addr);
    cpu_set_pc(cpu, load_addr);
}
#endif

#if 1
static void apple_sep_cpu_moni_jump(CPUState *cpu, run_on_cpu_data data)
{
    vaddr load_addr = data.target_ptr;
    DBGLOG("%s: before cpu_set_pc: base=0x" HWADDR_FMT_plx "\n", __func__, load_addr);
    cpu_set_pc(cpu, load_addr);
}
#endif

#if 0
static void apple_sep_cpu_moni_jump(CPUState *cpu, run_on_cpu_data data)
{
    // can raise an exception here "0x340005c0c: movi    v0.2d, #0x0" because of the reset
    AppleSEPState *s = data.host_ptr;
    vaddr load_addr = *(vaddr*)&s->boot_monitor_regs[0x20];
    // "crn, crm, op0, op1, op2" instead of "p_op0, p_op1, p_crn, p_crm, p_op2"
    //const ARMCPRegInfo *ri = get_arm_cp_reginfo(s->cpu->cp_regs, ENCODE_AA64_CP_REG(CP_REG_ARM64_SYSREG_CP, 15, 2, 3, 7, 0));
    //assert(ri);
    //vaddr local_SYS_ACC_PWR_DN_SAVE = read_raw_cp_reg(&s->cpu->env, ri);
    //cpu_reset(cpu);
    DBGLOG("%s: before cpu_set_pc: base=0x" HWADDR_FMT_plx "\n", __func__, load_addr);
    cpu_set_pc(cpu, load_addr);
    //write_raw_cp_reg(&s->cpu->env, ri, local_SYS_ACC_PWR_DN_SAVE); // is static
    //raw_write(&s->cpu->env, ri, local_SYS_ACC_PWR_DN_SAVE);
}
#endif




static void apple_sep_iop_start(AppleA7IOP *s)
{
    // some race conditions might happen before, during and/or after the jump.
    AppleSEPState *sep;

    sep = APPLE_SEP(s);

    trace_apple_sep_iop_start(s->iop_mailbox->role);

    apple_a7iop_set_cpu_status(s, apple_a7iop_get_cpu_status(s) &
                                      ~CPU_STATUS_IDLE);

    vaddr load_addr = *(vaddr *)&sep->boot_monitor_regs[0x20];
    // Don't prevent two successive calls of this, it is actually intended,
    // once by seprom and once by sepfw.
    // rely on apple_a7iop_set_cpu_ctrl for "(cpu_ctrl & SEP_BOOT_MONITOR_RUN)"
    if (sep->modern && load_addr != 0)
    {
#if 1
        qemu_log_mask(LOG_UNIMP,
                      "%s: have load_addr 0x" HWADDR_FMT_plx "\n",
                      __func__, load_addr);
#endif
        //vaddr old_base = sep->base;
        //sep->base = load_addr;
        //cpu_reset(CPU(sep->cpu)); 
        //run_on_cpu(CPU(sep->cpu), apple_sep_cpu_reset_work, RUN_ON_CPU_HOST_PTR(sep));
        //async_safe_run_on_cpu(CPU(sep->cpu), apple_sep_cpu_reset_work, RUN_ON_CPU_HOST_PTR(sep));
        //async_safe_run_on_cpu(CPU(sep->cpu), apple_sep_cpu_moni_jump, RUN_ON_CPU_HOST_PTR(sep));
        async_safe_run_on_cpu(CPU(sep->cpu), apple_sep_cpu_moni_jump, RUN_ON_CPU_TARGET_PTR(load_addr));
        //sep->base = old_base;
        // HACK: because cpu_set_pc skips over the first instruction.
        //ARM_CPU(sep->cpu)->env.xregs[1] = load_addr;
        //cpu_pause(CPU(sep->cpu));
        //cpu_set_pc(CPU(sep->cpu), load_addr);
        //cpu_resume(CPU(sep->cpu));
    }
}

static void apple_sep_iop_wakeup(AppleA7IOP *s)
{
    AppleSEPState *sep;

    sep = APPLE_SEP(s);

    trace_apple_sep_iop_wakeup(s->iop_mailbox->role);

    apple_a7iop_set_cpu_status(s, apple_a7iop_get_cpu_status(s) &
                                      ~CPU_STATUS_IDLE);

    // TODO
}
static const AppleA7IOPOps apple_sep_iop_ops = {
    .start = apple_sep_iop_start,
    .wakeup = apple_sep_iop_wakeup,
};

void create_eeprom_entry(uint32_t eeprom_index, uint32_t unkn0,
                         uint32_t counter, uint8_t type, uint8_t length,
                         uint8_t *data_in, uint8_t *eeprom_out);

AppleSEPState *apple_sep_create(DTBNode *node, MemoryRegion *ool_mr, vaddr base,
                                uint32_t cpu_id, uint32_t build_version,
                                bool modern, uint32_t chip_id)
{
    DeviceState *dev;
    AppleA7IOP *a7iop;
    AppleSEPState *s;
    SysBusDevice *sbd;
    DTBProp *prop;
    uint64_t *reg;
    uint32_t i;

    dev = qdev_new(TYPE_APPLE_SEP);
    a7iop = APPLE_A7IOP(dev);
    s = APPLE_SEP(dev);
    sbd = SYS_BUS_DEVICE(dev);

    prop = dtb_find_prop(node, "reg");
    g_assert_nonnull(prop);
    reg = (uint64_t *)prop->data;

    apple_a7iop_init(a7iop, "SEP", reg[1],
                     modern ? APPLE_A7IOP_V4 : APPLE_A7IOP_V2, &apple_sep_iop_ops, NULL);
    s->base = base;
    s->modern = modern;
    s->chip_id = chip_id;

    if (s->chip_id >= 0x8020) {
        s->shmbuf_base = SEP_SHMBUF_BASE;
        s->trace_buffer_base_offset = 0x10000;
        s->debug_trace_size = 0x10000;
    } else if (s->chip_id == 0x8015) {
        g_assert_not_reached();
        s->shmbuf_base = 0; // is dynamic
        s->trace_buffer_base_offset = 0x10000;
        s->debug_trace_size = 0x10000;
    } else {
        s->shmbuf_base = 0;
        s->trace_buffer_base_offset = 0;
        s->debug_trace_size = 0;
    }

    MemoryRegion *mr0 = g_new0(MemoryRegion, 1);
    memory_region_init_alias(mr0, OBJECT(s), "sep_dma", ool_mr, 0, SEP_DMA_MAPPING_SIZE);
    if (modern) {
        s->cpu = ARM_CPU(apple_a13_cpu_create(NULL, g_strdup("sep-cpu"), cpu_id,
                                              0, -1, 'P'));
        memory_region_add_subregion(&APPLE_A13(s->cpu)->memory, 0, mr0);
    } else {
        s->cpu = ARM_CPU(apple_a9_create(NULL, g_strdup("sep-cpu"), cpu_id, 0));
        object_property_set_bool(OBJECT(s->cpu), "aarch64", false, NULL);
        unset_feature(&s->cpu->env, ARM_FEATURE_AARCH64);
        memory_region_add_subregion(&APPLE_A9(s->cpu)->memory, 0, mr0);
    }
    if (s->chip_id >= 0x8020) {
        // hack to make SEP_ENABLE_OVERWRITE_SHMBUF_OBJECTS work properly
        MemoryRegion *mr1 = g_new0(MemoryRegion, 1);
        memory_region_init_alias(mr1, OBJECT(s), "sep_shmbuf_hdr", ool_mr, s->shmbuf_base, 0x4000);
        memory_region_add_subregion(get_system_memory(), s->shmbuf_base, mr1);
    }
    object_property_set_uint(OBJECT(s->cpu), "rvbar", s->base & ~0xFFF, NULL);
    object_property_add_child(OBJECT(dev), DEVICE(s->cpu)->id, OBJECT(s->cpu));

    // AKF_MBOX reg is handled using the device tree
    // XPRT_{PMSC,FUSE,MISC} regs are handled in t8030.c
    memory_region_init_io(&s->pmgr_base_mr, OBJECT(dev), &pmgr_base_reg_ops, s,
                          "sep.pmgr_base", PMGR_BASE_REG_SIZE); // T8030
    sysbus_init_mmio(sbd, &s->pmgr_base_mr);
    memory_region_init_io(&s->trng_regs_mr, OBJECT(dev), &trng_regs_reg_ops,
                          &s->trng_state, "sep.trng_regs",
                          TRNG_REGS_REG_SIZE); // T8030
    sysbus_init_mmio(sbd, &s->trng_regs_mr);
    memory_region_init_io(&s->key_base_mr, OBJECT(dev), &key_base_reg_ops, s,
                          "sep.key_base", KEY_BASE_REG_SIZE); // T8030
    sysbus_init_mmio(sbd, &s->key_base_mr);
    memory_region_init_io(&s->key_fcfg_mr, OBJECT(dev), &key_fcfg_reg_ops,
                          s, "sep.key_fcfg", KEY_FCFG_REG_SIZE); // T8030
    sysbus_init_mmio(sbd, &s->key_fcfg_mr);
    memory_region_init_io(&s->moni_base_mr, OBJECT(dev), &moni_base_reg_ops,
                          s, "sep.moni_base", MONI_BASE_REG_SIZE); // T8030
    sysbus_init_mmio(sbd, &s->moni_base_mr);
    memory_region_init_io(&s->moni_thrm_mr, OBJECT(dev), &moni_thrm_reg_ops,
                          s, "sep.moni_thrm", MONI_THRM_REG_SIZE); // T8030
    sysbus_init_mmio(sbd, &s->moni_thrm_mr);
    // EISP_BASE T8020/T8030
    memory_region_init_io(&s->eisp_base_mr, OBJECT(dev), &eisp_base_reg_ops,
                          s, "sep.eisp_base", EISP_BASE_REG_SIZE);
    sysbus_init_mmio(sbd, &s->eisp_base_mr);
    memory_region_init_io(&s->eisp_hmac_mr, OBJECT(dev), &eisp_hmac_reg_ops,
                          s, "sep.eisp_hmac", EISP_HMAC_REG_SIZE); // T8030
    sysbus_init_mmio(sbd, &s->eisp_hmac_mr);
    memory_region_init_io(&s->aess_base_mr, OBJECT(dev), &aess_base_reg_ops,
                          &s->aess_state, "sep.aess_base",
                          AESS_BASE_REG_SIZE); // AESS_BASE T8030
    sysbus_init_mmio(sbd, &s->aess_base_mr);
    memory_region_init_io(&s->aesh_base_mr, OBJECT(dev), &aesh_base_reg_ops,
                          s, "sep.aesh_base", AESH_BASE_REG_SIZE); // T8030
    sysbus_init_mmio(sbd, &s->aesh_base_mr);
    memory_region_init_io(&s->pka_base_mr, OBJECT(dev), &pka_base_reg_ops,
                          &s->pka_state, "sep.pka_base",
                          PKA_BASE_REG_SIZE); // PKA_BASE T8030
    sysbus_init_mmio(sbd, &s->pka_base_mr);
    memory_region_init_io(&s->pka_tmm_mr, OBJECT(dev), &pka_tmm_reg_ops,
                          s, "sep.pka_tmm", PKA_TMM_REG_SIZE); // T8030
    sysbus_init_mmio(sbd, &s->pka_tmm_mr);
    memory_region_init_io(&s->misc0_mr, OBJECT(dev), &misc0_reg_ops, s,
                          "sep.misc0", MISC0_REG_SIZE);
    sysbus_init_mmio(sbd, &s->misc0_mr);
    memory_region_init_io(&s->misc2_mr, OBJECT(dev), &misc2_reg_ops, s,
                          "sep.misc2", MISC2_REG_SIZE);
    sysbus_init_mmio(sbd, &s->misc2_mr);
    memory_region_init_io(&s->progress_mr, OBJECT(dev), &progress_reg_ops, s,
                          "sep.progress",
                          PROGRESS_REG_SIZE); // Encrypted progress counter?
    sysbus_init_mmio(sbd, &s->progress_mr);
    memory_region_init_io(&s->boot_monitor_mr, OBJECT(dev),
                          &boot_monitor_reg_ops, s,
                          "sep.boot_monitor", BOOT_MONITOR_REG_SIZE); // T8030
    sysbus_init_mmio(sbd, &s->boot_monitor_mr);
#ifdef SEP_ENABLE_DEBUG_TRACE_MAPPING
    // TODO: Let's think about something for T8015
    memory_region_init_io(&s->debug_trace_mr, OBJECT(dev), &debug_trace_reg_ops,
                          s, "sep.debug_trace",
                          s->debug_trace_size); // Debug trace printing
    memory_region_add_subregion(&APPLE_A13(s->cpu)->memory, s->shmbuf_base +
                                s->trace_buffer_base_offset, &s->debug_trace_mr);
#endif

    DTBNode *child = dtb_get_node(node, "iop-sep-nub");
    g_assert_nonnull(child);

    MachineState *machine = MACHINE(qdev_get_machine());
    DeviceState *gpio = NULL;
    uint32_t sep_gpio_pins = 0x4;
    uint32_t sep_gpio_int_groups = 0x1;
    gpio = apple_custom_gpio_create((char *)"sep_gpio", 0x10000, sep_gpio_pins,
                                    sep_gpio_int_groups);
    g_assert_nonnull(gpio);
    sysbus_mmio_map(SYS_BUS_DEVICE(gpio), 0, 0x2414c0000ull); // T8030
    s->aess_state.chip_id = s->chip_id;

    for (i = 0; i < sep_gpio_int_groups; i++) {
        // sysbus_connect_irq(SYS_BUS_DEVICE(gpio), i,
        // qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_IRQ));
    }
    for (i = 0; i < sep_gpio_pins; i++) {
        // qdev_connect_gpio_out(gpio, i, qdev_get_gpio_in(DEVICE(s->cpu),
        // ARM_CPU_IRQ));
    }
    object_property_add_child(OBJECT(machine), "sep_gpio", OBJECT(gpio));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(gpio), &error_fatal);
    SysBusDevice *i2c = NULL;
    i2c = apple_i2c_create("sep_i2c");
    g_assert_nonnull(i2c);
    object_property_add_child(OBJECT(machine), "sep_i2c", OBJECT(i2c));
    sysbus_mmio_map(i2c, 0, 0x241480000ull); // T8030
    sysbus_realize_and_unref(i2c, &error_fatal);
    uint64_t eeprom0_size = 64 * KiB;
    if (s->chip_id >= 0x8020) {
        eeprom0_size = 2 * KiB; // 0x800 bytes
    }
    uint8_t *eeprom0_init = g_malloc0(eeprom0_size);
    memset(eeprom0_init, 0x00, eeprom0_size);

    typedef struct QEMU_PACKED {
        uint8_t valid_amnm;
        uint8_t amnm[0x30];
        uint8_t valid_snon;
        uint8_t snon[0x14];
    } amnm_snon_entry_t;
    amnm_snon_entry_t amnm_snon_entry = { 0 };
    g_assert_cmphex(sizeof(amnm_snon_entry), ==,
                    0x46); // g_assert_cmphex or g_assert_cmpuint?
    uint8_t data0_in[0x10] = { 0 };
    uint8_t data1_in[0x46] = { 0 };
    uint8_t data2_in[0x8] = { 0 };
    memset(data0_in, 0xaa, sizeof(data0_in));
    memset(data1_in, 0xbb, sizeof(data1_in));
    memset(data2_in, 0xcc, sizeof(data2_in));
    uint8_t amnm[0x30] = { 0xde, 0xad, 0xbe, 0xef, 0xde, 0xad, 0xbe, 0xef,
                           0xde, 0xad, 0xbe, 0xef, 0xde, 0xad, 0xbe, 0xef,
                           0xde, 0xad, 0xbe, 0xef, 0xde, 0xad, 0xbe, 0xef,
                           0xde, 0xad, 0xbe, 0xef, 0xde, 0xad, 0xbe, 0xef,
                           0xde, 0xad, 0xbe, 0xef, 0xde, 0xad, 0xbe, 0xef,
                           0xde, 0xad, 0xbe, 0xef, 0xde, 0xad, 0xbe, 0xef };
    uint8_t snon[0x14] = { 0xfe, 0xed, 0xfa, 0xce, 0xfe, 0xed, 0xfa,
                           0xce, 0xfe, 0xed, 0xfa, 0xce, 0xfe, 0xed,
                           0xfa, 0xce, 0xfe, 0xed, 0xfa, 0xce };
    amnm_snon_entry.valid_amnm = 1;
    memcpy(amnm_snon_entry.amnm, amnm, sizeof(amnm));
    amnm_snon_entry.valid_snon = 1;
    memcpy(amnm_snon_entry.snon, snon, sizeof(snon));
    memcpy(data1_in, (uint8_t *)&amnm_snon_entry, sizeof(amnm_snon_entry));
    create_eeprom_entry(0x0, 0x0, 0x0, 0x01, 0x20, data0_in,
                        eeprom0_init); // hmac-check, reuse key for later ;
                                       // handler_xART_0x65_0x7540
    create_eeprom_entry(0x1, 0x0, 0x1, 0x02, 0x56, data1_in,
                        eeprom0_init); // amnm and snon
    create_eeprom_entry(0x2, 0x0, 0x2, 0x03, 0x18, data2_in,
                        eeprom0_init); // put data into wrapper2
    DriveInfo *dinfo_eeprom = drive_get_by_index(IF_PFLASH, 0);
    g_assert_nonnull(dinfo_eeprom);
    BlockBackend *blk_eeprom = blk_by_legacy_dinfo(dinfo_eeprom);
    g_assert_nonnull(blk_eeprom);
    EEPROMState *eeprom0 = AT24C_EE(
        at24c_eeprom_init_rom_blk(APPLE_I2C(i2c)->bus, 0x51, eeprom0_size,
                                  eeprom0_init, eeprom0_size, 2, blk_eeprom));
    g_assert_nonnull(eeprom0);
#if 0
    if (buffer_is_zero(&eeprom0->mem[0 << 8], 0x100)) {
        // not needed when memcmp_validstrs14 is patched
        memcpy(&eeprom0->mem[0 << 8], &eeprom0_init[0 << 8], 0x100);
        memcpy(&eeprom0->mem[1 << 8], &eeprom0_init[1 << 8], 0x100);
        memcpy(&eeprom0->mem[2 << 8], &eeprom0_init[2 << 8], 0x100);
        if (eeprom0->blk) {
            blk_pwrite(eeprom0->blk, 0 << 8, 0x100, &eeprom0_init[0 << 8], 0);
            blk_pwrite(eeprom0->blk, 1 << 8, 0x100, &eeprom0_init[1 << 8], 0);
            blk_pwrite(eeprom0->blk, 2 << 8, 0x100, &eeprom0_init[2 << 8], 0);
        }
    }
#endif
    s->eeprom0 = eeprom0;
    if (s->chip_id >= 0x8020) {
        DriveInfo *dinfo_ssc = drive_get_by_index(IF_PFLASH, 1);
        g_assert_nonnull(dinfo_ssc);
        BlockBackend *blk_ssc = blk_by_legacy_dinfo(dinfo_ssc);
        g_assert_nonnull(blk_ssc);
        AppleSSCState *ssc = apple_ssc_create(machine, 0x71);
        g_assert_nonnull(ssc);
        s->ssc_state = ssc;
        s->ssc_state->aess_state = &s->aess_state;
        qdev_prop_set_drive_err(DEVICE(s->ssc_state), "drive", blk_ssc,
                                &error_fatal);
        blk_set_perm(blk_ssc, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                     BLK_PERM_ALL, &error_fatal);
    }

#if 1
    s->ool_mr = ool_mr;
    g_assert_nonnull(s->ool_mr);
    g_assert_nonnull(
        object_property_add_const_link(OBJECT(s), "ool-mr", OBJECT(s->ool_mr)));
    s->ool_as = g_new0(AddressSpace, 1);
    g_assert_nonnull(s->ool_as);
    address_space_init(s->ool_as, s->ool_mr, "sep.ool");
#endif

    return s;
}

static void apple_sep_cpu_reset_work(CPUState *cpu, run_on_cpu_data data)
{
    AppleSEPState *s = data.host_ptr;
    cpu_reset(cpu);
    DBGLOG("apple_sep_cpu_reset_work: before cpu_set_pc: base=0x" HWADDR_FMT_plx
           "\n",
           s->base);
    cpu_set_pc(cpu, s->base);
}

static void apple_sep_realize(DeviceState *dev, Error **errp)
{
    AppleSEPState *s;
    AppleSEPClass *sc;

    s = APPLE_SEP(dev);
    sc = APPLE_SEP_GET_CLASS(dev);
    if (sc->parent_realize) {
        sc->parent_realize(dev, errp);
    }
    qdev_realize(DEVICE(s->cpu), NULL, errp);
    s->irq_or = qdev_new(TYPE_OR_IRQ);
    object_property_add_child(OBJECT(dev), "irq-or", OBJECT(s->irq_or));
    qdev_prop_set_uint16(s->irq_or, "num-lines", 16);
    qdev_realize_and_unref(s->irq_or, NULL, errp);
    if (*errp) {
        return;
    }
    qdev_connect_gpio_out(s->irq_or, 0,
                          qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_IRQ));
    qdev_connect_gpio_out(DEVICE(s->cpu), GTIMER_PHYS,
                          qdev_get_gpio_in(s->irq_or, 0));
    qdev_connect_gpio_out_named(DEVICE(APPLE_A7IOP(s)->iop_mailbox),
                                APPLE_A7IOP_IOP_IRQ, 0,
                                qdev_get_gpio_in(s->irq_or, 1));
    // qdev_connect_gpio_out_named(DEVICE(APPLE_A7IOP(s)->ap_mailbox),
    // APPLE_A7IOP_IOP_IRQ, 0, qdev_get_gpio_in(s->irq_or, 2));
}

static void aess_reset(AppleAESSState *s)
{
    s->status = 0;
    s->command = 0;
    s->interrupt_status = 0;
    s->interrupt_enabled = 0;
    s->reg_0x14_keywrap_iterations_counter = 0;
    s->reg_0x18_keydisable = 0;
    s->seed_bits = 0;
    s->seed_bits_lock = 0;
    //
    s->keywrap_uid0_enabled = false;
    s->keywrap_uid1_enabled = false;
    memset(s->keywrap_key_uid0, 0, sizeof(s->keywrap_key_uid0));
    memset(s->keywrap_key_uid1, 0, sizeof(s->keywrap_key_uid1));
    memset(s->custom_key_index, 0, sizeof(s->custom_key_index));
    memset(s->custom_key_index_enabled, 0, sizeof(s->custom_key_index_enabled));
}

static void pka_reset(ApplePKAState *s)
{
    s->status0 = 0;
    s->status_in0 = 0;
    s->img4out_dgst_clock = 0;
    s->chip_revision_clock = 0;
    s->chipid_ecid_misc_clock = 0;
}


static void map_sepfw(AppleSEPState *s)
{
    DBGLOG("%s: entered function\n", __func__);
    AddressSpace *nsas = &address_space_memory;
    // Apparently needed because of a bug occurring on XNU
    // clear lowest 0x4000 bytes as well, because they shouldn't contain any
    // valid data
    address_space_set(nsas, 0x0, 0, SEPFW_MAPPING_SIZE, MEMTXATTRS_UNSPECIFIED);
#ifdef SEP_ENABLE_HARDCODED_FIRMWARE
    address_space_rw(nsas, 0x4000ULL, MEMTXATTRS_UNSPECIFIED,
                     (uint8_t *)s->sepfw_data, s->sep_fw_size, true);
#endif
}

static void apple_sep_reset_hold(Object *obj, ResetType type)
{
    AppleSEPState *s;
    AppleSEPClass *sc;

    s = APPLE_SEP(obj);
    sc = APPLE_SEP_GET_CLASS(obj);

    if (sc->parent_phases.hold != NULL) {
        sc->parent_phases.hold(obj, type);
    }
    s->key_fcfg_offset_0x14_index = 0;
    memset(s->key_fcfg_offset_0x14_values, 0, sizeof(s->key_fcfg_offset_0x14_values));
    s->pmgr_fuse_changer_bit0_was_set = false;
    s->pmgr_fuse_changer_bit1_was_set = false;
    memset(s->pmgr_base_regs, 0, sizeof(s->pmgr_base_regs));
    memset(s->key_base_regs, 0, sizeof(s->key_base_regs));
    memset(s->key_fcfg_regs, 0, sizeof(s->key_fcfg_regs));
    memset(s->moni_base_regs, 0, sizeof(s->moni_base_regs));
    memset(s->moni_thrm_regs, 0, sizeof(s->moni_thrm_regs));
    memset(s->eisp_base_regs, 0, sizeof(s->eisp_base_regs));
    memset(s->eisp_hmac_regs, 0, sizeof(s->eisp_hmac_regs));
    memset(s->aess_base_regs, 0, sizeof(s->aess_base_regs));
    memset(s->aesh_base_regs, 0, sizeof(s->aesh_base_regs));
    memset(s->pka_base_regs, 0, sizeof(s->pka_base_regs));
    memset(s->pka_tmm_regs, 0, sizeof(s->pka_tmm_regs));
    memset(s->misc0_regs, 0, sizeof(s->misc0_regs));
    memset(s->misc2_regs, 0, sizeof(s->misc2_regs));
    memset(s->boot_monitor_regs, 0, sizeof(s->boot_monitor_regs));
    memset(s->progress_regs, 0, sizeof(s->progress_regs));
    memset(s->debug_trace_regs, 0, sizeof(s->debug_trace_regs));

    aess_reset(&s->aess_state);
    pka_reset(&s->pka_state);
    // apple_ssc_reset is being called, but not here.
    run_on_cpu(CPU(s->cpu), apple_sep_cpu_reset_work, RUN_ON_CPU_HOST_PTR(s));
    map_sepfw(s);
}

static void apple_sep_class_init(ObjectClass *klass, void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);
    AppleSEPClass *sc = APPLE_SEP_CLASS(klass);
    device_class_set_parent_realize(dc, apple_sep_realize, &sc->parent_realize);
    resettable_class_set_parent_phases(rc, NULL, apple_sep_reset_hold, NULL,
                                       &sc->parent_phases);
    dc->desc = "Apple SEP";
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo apple_sep_info = {
    .name = TYPE_APPLE_SEP,
    .parent = TYPE_APPLE_A7IOP,
    .instance_size = sizeof(AppleSEPState),
    .class_size = sizeof(AppleSEPClass),
    .class_init = apple_sep_class_init,
};

static void apple_sep_register_types(void)
{
    type_register_static(&apple_sep_info);
}

type_init(apple_sep_register_types);


void create_eeprom_entry(uint32_t eeprom_index, uint32_t unkn0,
                         uint32_t counter, uint8_t type, uint8_t length,
                         uint8_t *data_in, uint8_t *eeprom_out)
{
    g_assert_true(qcrypto_hmac_supports(QCRYPTO_HASH_ALGO_SHA256));

    typedef struct QEMU_PACKED {
        uint32_t unkn0; // 0x00 ;; ignored? ;; maybe needs to be increasing.
        uint32_t counter; // 0x04 ;; value offset 0x00-0x03
        uint8_t type; // 0x08 ;; <= 0x3 ;; value offset 0x04
        uint8_t length; // 0x09 ;; value offset 0x05 ;; 0x20 == wrapper0/0x1 ;;
                        // 0x56 == wrapper1/0x2 ;; 0x18 == wrapper2/0x3
        uint16_t unkn4_zero0; // 0x0a ;; needs to be zero?
        uint16_t crc16_entry; // 0x0c ;; value offset 0x06-0x07
        uint16_t crc16_header; // 0x0e ;; crcmod's crc-ccitt-false
    } eeprom_entry_t;
    eeprom_entry_t eeprom_entry = { 0 };
    g_assert_cmphex(sizeof(eeprom_entry), ==, 0x10);
    eeprom_entry.unkn0 = unkn0;
    eeprom_entry.counter = counter;
    eeprom_entry.type = type;
    eeprom_entry.length = length;
    eeprom_entry.unkn4_zero0 = 0x00;

    uint32_t entry_length_without_hmac = eeprom_entry.length - 0x10;
    uint8_t aess_out_for_key[32] = { 0 };
    uint8_t hmac_in[0x57] = { 0 };
    QCryptoHmac *hmac = NULL;
    uint8_t *result = NULL;
    size_t resultlen = 0;
    int ret = 0;

    hmac = qcrypto_hmac_new(QCRYPTO_HASH_ALGO_SHA256,
                            (const uint8_t *)aess_out_for_key,
                            sizeof(aess_out_for_key), &error_fatal);
    g_assert_nonnull(hmac);

    hmac_in[0] = eeprom_entry.type;
    memcpy(&hmac_in[1], data_in, entry_length_without_hmac);
    ret = qcrypto_hmac_bytes(hmac, (const char *)hmac_in,
                             entry_length_without_hmac + 1, &result, &resultlen,
                             &error_fatal);
    g_assert_cmpuint(ret, ==, 0);

    uint32_t eeprom_offset = eeprom_index << 8;
    memset(&eeprom_out[eeprom_offset + 0x40], 0x00, eeprom_entry.length);
    memcpy(&eeprom_out[eeprom_offset + 0x40 + 0x00], data_in,
           entry_length_without_hmac); // plain data
    memcpy(&eeprom_out[eeprom_offset + 0x40 + entry_length_without_hmac],
           result, 0x10); // data from HMAC-SHA256: 0x01 + plain value

    eeprom_entry.crc16_entry = crc_ccitt_false(
        0xffff, &eeprom_out[eeprom_offset + 0x40], eeprom_entry.length);
    eeprom_entry.crc16_header =
        crc_ccitt_false(0xffff, (uint8_t *)&eeprom_entry, 0xe);

    memcpy(&eeprom_out[eeprom_offset], (uint8_t *)&eeprom_entry,
           sizeof(eeprom_entry));

    qcrypto_hmac_free(hmac);

    g_free(result);
}

static int apple_ssc_event(I2CSlave *s, enum i2c_event event)
{
    AppleSSCState *ssc = APPLE_SSC(s);

    switch (event) {
    case I2C_START_SEND:
        qemu_log_mask(LOG_UNIMP, "apple_ssc_event: I2C_START_SEND\n");
        break;
    case I2C_FINISH:
        qemu_log_mask(LOG_UNIMP, "apple_ssc_event: I2C_FINISH\n");
        break;
    case I2C_START_RECV:
        qemu_log_mask(LOG_UNIMP, "apple_ssc_event: I2C_START_RECV\n");
        break;
    case I2C_NACK:
        qemu_log_mask(LOG_UNIMP, "apple_ssc_event: I2C_NACK\n");
        break;
    default:
        return -1;
    }
    return 0;
}

#define SSC_REQUEST_SIZE_CMD_0x0 (0x84)
#define SSC_REQUEST_SIZE_CMD_0x1 (0x74)
#define SSC_REQUEST_SIZE_CMD_0x2 (0x4)
#define SSC_REQUEST_SIZE_CMD_0x3 (0x34)
#define SSC_REQUEST_SIZE_CMD_0x4 (0x14)
#define SSC_REQUEST_SIZE_CMD_0x5 (0x54)
#define SSC_REQUEST_SIZE_CMD_0x6 (0x14)
#define SSC_REQUEST_SIZE_CMD_0x7 (0x4)
#define SSC_REQUEST_SIZE_CMD_0x8 (0x4)
#define SSC_REQUEST_SIZE_CMD_0x9 (0x4)

#define SSC_RESPONSE_SIZE_CMD_0x0 (0xC4)
#define SSC_RESPONSE_SIZE_CMD_0x1 (0x74)
#define SSC_RESPONSE_SIZE_CMD_0x2 (0x4)
#define SSC_RESPONSE_SIZE_CMD_0x3 (0x14)
#define SSC_RESPONSE_SIZE_CMD_0x4 (0x54)
#define SSC_RESPONSE_SIZE_CMD_0x5 (0x14)
#define SSC_RESPONSE_SIZE_CMD_0x6 (0x34)
#define SSC_RESPONSE_SIZE_CMD_0x7 (0x78)
#define SSC_RESPONSE_SIZE_CMD_0x8 (0x4)
#define SSC_RESPONSE_SIZE_CMD_0x9 (0x2F)

static uint8_t ssc_request_sizes[] = {
    SSC_REQUEST_SIZE_CMD_0x0, SSC_REQUEST_SIZE_CMD_0x1,
    SSC_REQUEST_SIZE_CMD_0x2, SSC_REQUEST_SIZE_CMD_0x3,
    SSC_REQUEST_SIZE_CMD_0x4, SSC_REQUEST_SIZE_CMD_0x5,
    SSC_REQUEST_SIZE_CMD_0x6, SSC_REQUEST_SIZE_CMD_0x7,
    SSC_REQUEST_SIZE_CMD_0x8, SSC_REQUEST_SIZE_CMD_0x9
};

static uint8_t INFOSTR_AKE_SESSIONSEED[] = "AKE_SessionSeed\n";
static uint8_t INFOSTR_AKE_MACKEY[] = "AKE_MACKey\n\n\n\n\n\n";
static uint8_t INFOSTR_AKE_EXTRACTORKEY[] = "AKE_ExtractorKey";

static bool is_keyslot_valid(struct AppleSSCState *ssc_state,
                             uint8_t kbkdf_index)
{
    bool ret;

    ret = !buffer_is_zero(&ssc_state->ecc_keys[kbkdf_index],
                          sizeof(struct ecc_scalar));
    ret &= !buffer_is_zero(&ssc_state->kbkdf_keys[kbkdf_index],
                           sizeof(ssc_state->kbkdf_keys[kbkdf_index]));

    DBGLOG("%s: kbkdf_index: %d ; ecc_keys_item_size: 0x%lX ; "
           "kbkdf_keys_item_size: 0x%lX\n",
           __func__, kbkdf_index, sizeof(struct ecc_scalar),
           sizeof(ssc_state->kbkdf_keys[kbkdf_index]));
    return ret;
}

static int aes_ccm_crypt(struct AppleSSCState *ssc_state, uint8_t kbkdf_index,
                         uint8_t *prefix, int payload_len, uint8_t *data,
                         uint8_t *out, int encrypt, int response_key)
{
    struct ccm_aes256_ctx aes;
    uint32_t counter_be = cpu_to_be32(ssc_state->kbkdf_counter[kbkdf_index]);
    uint8_t nonce[AES_CCM_NONCE_LENGTH] = { 0 };
    uint8_t auth[AES_CCM_AUTH_LENGTH] = { 0 };
    uint8_t tmp_in[AES_CCM_MAX_DATA_LENGTH] = { 0 };
    uint8_t tmp_out[AES_CCM_MAX_DATA_LENGTH] = { 0 };
    uint8_t *key = NULL;
    int status = 0;
#if 0
    // SEPFW role
    if (encrypt) {
        key = &ssc_state->kbkdf_keys[kbkdf_index][KBKDF_KEY_REQUEST_KEY_OFFSET];
        ssc_state->kbkdf_counter[kbkdf_index]++;
    } else {
        key = &ssc_state->kbkdf_keys[kbkdf_index][KBKDF_KEY_RESPONSE_KEY_OFFSET];
    }
#endif
#if 1
    // SSC role
    // if (encrypt)
    if (response_key) {
        key =
            &ssc_state->kbkdf_keys[kbkdf_index][KBKDF_KEY_RESPONSE_KEY_OFFSET];
    } else {
        key = &ssc_state->kbkdf_keys[kbkdf_index][KBKDF_KEY_REQUEST_KEY_OFFSET];
        ssc_state->kbkdf_counter[kbkdf_index]++;
    }
#endif

    memcpy(auth, prefix, MSG_PREFIX_LENGTH);
    memcpy(&auth[MSG_PREFIX_LENGTH], &counter_be, AES_CCM_COUNTER_LENGTH);
    memcpy(nonce, &ssc_state->kbkdf_keys[kbkdf_index][KBKDF_KEY_SEED_OFFSET],
           KBKDF_KEY_SEED_LENGTH);
    memcpy(&nonce[KBKDF_KEY_SEED_LENGTH], &counter_be, AES_CCM_COUNTER_LENGTH);
    ccm_aes256_set_key(&aes, key);
    if (encrypt) {
        ccm_aes256_encrypt_message(
            &aes, AES_CCM_NONCE_LENGTH, nonce, AES_CCM_AUTH_LENGTH, auth,
            AES_CCM_TAG_LENGTH, AES_CCM_TAG_LENGTH + payload_len, tmp_out,
            data);
        // data[0x20]-tag[0x10] => tag[0x10]-data[0x20]
        memcpy(out, &tmp_out[payload_len], AES_CCM_TAG_LENGTH);
        memcpy(&out[AES_CCM_TAG_LENGTH], tmp_out, payload_len);
    } else {
        DBGLOG("counter_be: 0x%08x\n", counter_be);
        // tag[0x10]-data[0x20] => data[0x20]-tag[0x10]
        memcpy(tmp_in, &data[AES_CCM_TAG_LENGTH], payload_len);
        memcpy(&tmp_in[payload_len], data, AES_CCM_TAG_LENGTH);
        HEXDUMP("tmp_in__tag_plus_encdata", data,
                AES_CCM_TAG_LENGTH + payload_len);
        HEXDUMP("tmp_in__encdata_plus_tag", tmp_in,
                AES_CCM_TAG_LENGTH + payload_len);
        status = ccm_aes256_decrypt_message(
            &aes, AES_CCM_NONCE_LENGTH, nonce, AES_CCM_AUTH_LENGTH, auth,
            AES_CCM_TAG_LENGTH, payload_len, tmp_out, tmp_in);
        if (!status) {
            DBGLOG("%s: ccm_aes256_decrypt_message: DIGEST INVALID\n",
                   __func__);
        }
        memcpy(out, tmp_out, payload_len);
    }
    ////memcpy(out, tmp_out, AES_CCM_MAX_DATA_LENGTH);
    return status;
}

static int aes_cmac_prefix_public(uint8_t *key, uint8_t *prefix,
                                  uint8_t *public0, uint8_t *digest)
{
    struct cmac_aes256_ctx ctx;
    cmac_aes256_set_key(&ctx, key);
    cmac_aes256_update(&ctx, MSG_PREFIX_LENGTH, prefix);
    cmac_aes256_update(&ctx, SECP384_PUBLIC_XY_SIZE, public0);
    cmac_aes256_digest(&ctx, CMAC128_DIGEST_SIZE, digest);
    return 0;
}

static int aes_cmac_prefix_public_public(uint8_t *key, uint8_t *prefix,
                                         uint8_t *public0, uint8_t *public1,
                                         uint8_t *digest)
{
    struct cmac_aes256_ctx ctx;
    cmac_aes256_set_key(&ctx, key);
    cmac_aes256_update(&ctx, MSG_PREFIX_LENGTH, prefix);
    cmac_aes256_update(&ctx, SECP384_PUBLIC_XY_SIZE, public0);
    cmac_aes256_update(&ctx, SECP384_PUBLIC_XY_SIZE, public1);
    cmac_aes256_digest(&ctx, CMAC128_DIGEST_SIZE, digest);
    return 0;
}

static int kbkdf_generate_key(uint8_t *cmac_key, uint8_t *label,
                              uint8_t *context, uint8_t *derived, int length)
{
    struct cmac_aes256_ctx ctx;

    uint8_t digest[CMAC128_DIGEST_SIZE] = { 0 };

    int counter = 1;
    uint16_t be_len = cpu_to_be16(length * 8);
    uint8_t zero = 0;
    cmac_aes256_set_key(&ctx, cmac_key);

    for (size_t i = 0; i < length; i += CMAC128_DIGEST_SIZE) {
        uint16_t be_cnt = 0;
        be_cnt = cpu_to_be16(counter);
        cmac_aes256_update(&ctx, KBKDF_CMAC_LENGTH_SIZE, (uint8_t *)&be_cnt);
        cmac_aes256_update(&ctx, KBKDF_CMAC_LABEL_SIZE, label); // 0x10 bytes
        cmac_aes256_update(&ctx, 1, (uint8_t *)&zero);
        cmac_aes256_update(&ctx, KBKDF_CMAC_CONTEXT_SIZE,
                           context); // 0x04 bytes
        cmac_aes256_update(&ctx, KBKDF_CMAC_LENGTH_SIZE, (uint8_t *)&be_len);
        cmac_aes256_digest(&ctx, CMAC128_DIGEST_SIZE, digest);
        memcpy(&derived[i], digest, MIN(CMAC128_DIGEST_SIZE, length - i));
        counter++;
    }

    return 0;
}

static int generate_ec_priv(const char *priv, struct ecc_scalar *ecc_key,
                            struct ecc_point *ecc_pub)
{
    // TODO: use fully random value if priv == NULL
    const struct ecc_curve *ecc = nettle_get_secp_384r1();
    mpz_t temp1;

    ecc_point_init(ecc_pub, ecc);
    ecc_scalar_init(ecc_key, ecc);
    mpz_set_str(temp1, priv, 16);
    mpz_add_ui(temp1, temp1, 1);
    g_assert_cmpuint(ecc_scalar_set(ecc_key, temp1), !=, 0);

    mpz_clear(temp1);

    ///
    //
    ecc_point_mul_g(ecc_pub, ecc_key);

    // ecc_scalar_clear (ecc_key);
    // ecc_point_clear (ecc_pub);

    return 0;
}

static int output_ec_pub(struct ecc_point *ecc_pub, uint8_t *pub_xy)
{
    // const struct ecc_curve *ecc = nettle_get_secp_384r1();
    mpz_t temp1, temp2;

    ecc_point_get(ecc_pub, temp1, temp2);
    mpz_export(&pub_xy[0x00], NULL, 1, 1, 1, 0, temp1);
    mpz_export(&pub_xy[0x00 + BYTELEN_384], NULL, 1, 1, 1, 0, temp2);
    HEXDUMP("output_ec_pub: pub_x", &pub_xy[0x00], BYTELEN_384);
    HEXDUMP("output_ec_pub: pub_y", &pub_xy[0x00 + BYTELEN_384], BYTELEN_384);

    mpz_clear(temp1);
    mpz_clear(temp2);

    return 0;
}

static int input_ec_pub(struct ecc_point *ecc_pub, uint8_t *pub_xy)
{
    const struct ecc_curve *ecc = nettle_get_secp_384r1();
    mpz_t temp1, temp2;
    int ret = 0;

    HEXDUMP("input_ec_pub: pub_x", &pub_xy[0x00], BYTELEN_384);
    HEXDUMP("input_ec_pub: pub_y", &pub_xy[0x00 + BYTELEN_384], BYTELEN_384);
    mpz_import(temp1, SECP384_PUBLIC_SIZE, 1, 1, 1, 0, &pub_xy[0x00]);
    mpz_import(temp2, SECP384_PUBLIC_SIZE, 1, 1, 1, 0,
               &pub_xy[0x00 + BYTELEN_384]);
    ecc_point_init(ecc_pub, ecc);
    ret = ecc_point_set(ecc_pub, temp1, temp2);

    mpz_clear(temp1);
    mpz_clear(temp2);

    return ret;
}

static int generate_kbkdf_keys(struct AppleSSCState *ssc_state,
                               struct ecc_scalar *ecc_key,
                               struct ecc_point *ecc_pub_peer,
                               uint8_t *hmac_key, uint8_t *label,
                               uint8_t *context, uint8_t kbkdf_index)
{
    const struct ecc_curve *ecc = nettle_get_secp_384r1();
    struct ecc_point T;
    uint8_t shared_key_xy[SECP384_PUBLIC_XY_SIZE] = {
        0
    }; // shared_key == pub_x (first half)
    uint8_t derived_key[SHA256_DIGEST_SIZE] = { 0 };
    DBGLOG("generate_kbkdf_keys: label: %s\n", label); // 0x10 bytes
    DBGLOG("generate_kbkdf_keys: context: %02x%02x%02x%02x\n", context[0x00],
           context[0x01], context[0x02],
           context[0x03]); // 4 bytes

    ecc_point_init(&T, ecc);
    ecc_point_mul(&T, ecc_key, ecc_pub_peer);
    DBGLOG("generate_kbkdf_keys: shared_key==pub_x:\n");
    output_ec_pub(&T, shared_key_xy);
    ecc_point_clear(&T);

    struct hmac_sha256_ctx ctx;
    hmac_sha256_set_key(&ctx, SHA256_DIGEST_SIZE, hmac_key);
    hmac_sha256_update(&ctx, SECP384_PUBLIC_SIZE,
                       shared_key_xy); // only the first half is the shared_key
    hmac_sha256_digest(&ctx, SHA256_DIGEST_SIZE, derived_key);
    HEXDUMP("generate_kbkdf_keys: derived_key", derived_key,
            SHA256_DIGEST_SIZE);

    int err = kbkdf_generate_key(derived_key, label, context,
                                 ssc_state->kbkdf_keys[kbkdf_index],
                                 KBKDF_CMAC_OUTPUT_LEN);
    if (err) {
        DBGLOG("error: kbkdf_generate_key returned non-zero\n");
        return err;
    }
    ssc_state->kbkdf_counter[kbkdf_index] = 0;
    HEXDUMP("generate_kbkdf_keys: ssc_state->kbkdf_keys[kbkdf_index]",
            ssc_state->kbkdf_keys[kbkdf_index], KBKDF_CMAC_OUTPUT_LEN);

    return 0;
}

static void hkdf_sha256(int salt_len, uint8_t *salt, int info_len,
                        uint8_t *info, int key_len, uint8_t *key, uint8_t *out)
{
    struct hmac_sha256_ctx ctx;
    uint8_t prk[SHA256_DIGEST_SIZE];

    hmac_sha256_set_key(&ctx, salt_len, salt);
    hkdf_extract(&ctx, (nettle_hash_update_func *)hmac_sha256_update,
                 (nettle_hash_digest_func *)hmac_sha256_digest,
                 SHA256_DIGEST_SIZE, key_len, key, prk);

    hmac_sha256_set_key(&ctx, SHA256_DIGEST_SIZE, prk);
    hkdf_expand(&ctx, (nettle_hash_update_func *)hmac_sha256_update,
                (nettle_hash_digest_func *)hmac_sha256_digest,
                SHA256_DIGEST_SIZE, info_len, info, SHA256_DIGEST_SIZE, out);
}

static void aes_keys_from_sp_key(struct AppleSSCState *ssc_state,
                                 uint8_t kbkdf_index, uint8_t *prefix,
                                 uint8_t *aes_key_mackey,
                                 uint8_t *aes_key_extractorkey)
{
    // wrapping with "SP key"/"Spes"/"Lynx version 1 crypto" could be wrong.
    uint8_t hmac_key[0x20] = {0};
    memcpy(hmac_key, ssc_state->slot_hmac_key[kbkdf_index], 0x20);
    HEXDUMP("aes_keys_from_sp_key: hmac_key", hmac_key, 0x20);
    kbkdf_generate_key(hmac_key, INFOSTR_AKE_MACKEY, prefix, aes_key_mackey,
                       0x20);
    HEXDUMP("aes_keys_from_sp_key: aes_key_mackey", aes_key_mackey, 0x20);
    kbkdf_generate_key(hmac_key, INFOSTR_AKE_EXTRACTORKEY, prefix,
                       aes_key_extractorkey, 0x20);
    HEXDUMP("aes_keys_from_sp_key: aes_key_extractorkey", aes_key_extractorkey,
            0x20);
}

static void do_response_prefix(uint8_t *request, uint8_t *response,
                               uint8_t flags)
{
    memset(response, 0, SSC_MAX_RESPONSE_SIZE);
    uint8_t cmd = request[0];
    response[0] = cmd;
    if (cmd <= 0x6) {
        response[1] = request[1];
    }
    response[2] = 0;
    response[3] = flags;
}

static void clear_ecc_scalar(struct ecc_scalar *ecc_key) {
    if (!buffer_is_zero(ecc_key, sizeof(struct ecc_scalar))) {
        ecc_scalar_clear (ecc_key);
        memset(ecc_key, 0, sizeof(struct ecc_scalar));
    }
}

// TODO: Properly handle various error cases with cmd 0x0/0x1/..., like wrong
// hashes/signatures/parameters or public keys not being on the curve.

static int answer_cmd_0x0_init1(struct AppleSSCState *ssc_state,
                                uint8_t *request, uint8_t *response)
{
    DBGLOG("%s: entered function\n", __func__);
    struct ecc_point cmd0_ecpub, ecc_pub;
    struct knuth_lfib_ctx rctx;
    struct dsa_signature signature;
    uint8_t digest[BYTELEN_384] = { 0 };
    uint8_t kbkdf_index = 0; // hardcoded
    char priv_str[0x60 + 1] = { 0 };

    knuth_lfib_init(&rctx, 4711);
    dsa_signature_init(&signature);

    if (is_keyslot_valid(ssc_state, kbkdf_index)) { // shouldn't already exist
        DBGLOG("%s: invalid kbkdf_index: %u\n", __func__, kbkdf_index);
        do_response_prefix(request, response,
                           SSC_RESPONSE_FLAG_KEYSLOT_INVALID);
        return 0;
    }
    if (input_ec_pub(&cmd0_ecpub,
                     &request[MSG_PREFIX_LENGTH + SHA256_DIGEST_SIZE]) ==
        0) { // curve is invalid
        DBGLOG("%s: invalid curve\n", __func__);
        do_response_prefix(request, response, SSC_RESPONSE_FLAG_CURVE_INVALID);
        goto jump_ret;
    }
    do_response_prefix(request, response, SSC_RESPONSE_FLAG_OK);
    snprintf(
        priv_str, sizeof(priv_str),
        "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"
        "dddddddddddddddddddddddddddd%02x",
        kbkdf_index);
    // TODO: use priv_str = NULL here after the function has been reworked
    // TODO: ASAN might show a (spurious?) __gmp_default_allocate leak here. already fixed?
    int err =
        generate_ec_priv(priv_str, &ssc_state->ecc_keys[kbkdf_index], &ecc_pub);
    output_ec_pub(&ecc_pub,
                  &response[MSG_PREFIX_LENGTH + SECP384_PUBLIC_XY_SIZE]);
    ecc_point_clear(&ecc_pub);
    memcpy(ssc_state->random_hmac_key, &request[MSG_PREFIX_LENGTH],
           SHA256_DIGEST_SIZE);
    DBGLOG("INFOSTR_AKE_SESSIONSEED: %s\n", INFOSTR_AKE_SESSIONSEED);
    generate_kbkdf_keys(ssc_state, &ssc_state->ecc_keys[kbkdf_index],
                        &cmd0_ecpub, ssc_state->random_hmac_key,
                        INFOSTR_AKE_SESSIONSEED, request, kbkdf_index);

    struct sha384_ctx ctx;
    sha384_init(&ctx);
    sha384_update(&ctx, MSG_PREFIX_LENGTH, &response[0x00]); // prefix
    sha384_update(
        &ctx, SECP384_PUBLIC_XY_SIZE,
        &request[MSG_PREFIX_LENGTH + SHA256_DIGEST_SIZE]); // sw_public_xy0
    sha384_update(
        &ctx, SECP384_PUBLIC_XY_SIZE,
        &response[MSG_PREFIX_LENGTH + SECP384_PUBLIC_XY_SIZE]); // public_xy1
    sha384_update(&ctx, SHA256_DIGEST_SIZE,
                  ssc_state->random_hmac_key); // hmac_key
    sha384_digest(&ctx, BYTELEN_384, digest);
    HEXDUMP("answer_cmd_0x0_init1 digest", digest, BYTELEN_384);
    // Using non-deterministic signing here like it's probably supposed to be.
    // Don't want to implement/port deterministic signing.
    ecdsa_sign(&ssc_state->ecc_key_main, &rctx,
               (nettle_random_func *)knuth_lfib_random, BYTELEN_384, digest,
               &signature);
    mpz_export(&response[MSG_PREFIX_LENGTH + 0x00 + 0x00], NULL, 1, 1, 1, 0,
               signature.r);
    mpz_export(&response[MSG_PREFIX_LENGTH + 0x00 + SECP384_PUBLIC_SIZE], NULL,
               1, 1, 1, 0, signature.s);
    dsa_signature_clear(&signature);
jump_ret:
    ecc_point_clear(&cmd0_ecpub);
    return 0;
}

static int answer_cmd_0x1_connect_sp(struct AppleSSCState *ssc_state,
                                     uint8_t *request, uint8_t *response)
{
    DBGLOG("%s: entered function\n", __func__);
    HEXDUMP("cmd_0x01_req", request, SSC_REQUEST_SIZE_CMD_0x1);
    struct ecc_point cmd1_ecpub, ecc_pub;
    uint8_t kbkdf_index = request[1];
    char priv_str[0x60 + 1] = { 0 };

    uint8_t *cmac_req_should = &request[MSG_PREFIX_LENGTH];
    uint8_t *sw_public_xy2 = &request[MSG_PREFIX_LENGTH + AES_BLOCK_SIZE];
    DBGLOG("answer_cmd_0x1_connect_sp: kbkdf_index: %u\n", kbkdf_index);
    HEXDUMP("answer_cmd_0x1_connect_sp: req_prefix", request,
            MSG_PREFIX_LENGTH);
    HEXDUMP("answer_cmd_0x1_connect_sp: sw_public_xy2", sw_public_xy2,
            SECP384_PUBLIC_XY_SIZE);
    HEXDUMP("answer_cmd_0x1_connect_sp: cmac_req_should", cmac_req_should,
            AES_BLOCK_SIZE);
    if (kbkdf_index >= KBKDF_KEY_MAX_SLOTS) {
        DBGLOG("%s: kbkdf_index over limit: %u\n", __func__, kbkdf_index);
        do_response_prefix(request, response,
                           SSC_RESPONSE_FLAG_COMMAND_OR_FIELD_INVALID);
        return 0;
    }
    if (is_keyslot_valid(ssc_state, kbkdf_index)) { // shouldn't already exist
        DBGLOG("%s: invalid kbkdf_index: %u\n", __func__, kbkdf_index);
        do_response_prefix(request, response,
                           SSC_RESPONSE_FLAG_KEYSLOT_INVALID);
        return 0;
    }
    if (input_ec_pub(&cmd1_ecpub, sw_public_xy2) == 0) { // curve is invalid
        DBGLOG("%s: invalid curve\n", __func__);
        do_response_prefix(request, response, SSC_RESPONSE_FLAG_CURVE_INVALID);
        goto jump_ret1;
    }
    snprintf(
        priv_str, sizeof(priv_str),
        "999999999999999999999999999999999999999999999999999999999999999999"
        "9999999999999999999999999999%02x",
        kbkdf_index);
    // TODO: use priv_str = NULL here after the function has been reworked
    int err =
        generate_ec_priv(priv_str, &ssc_state->ecc_keys[kbkdf_index], &ecc_pub);
    uint8_t aes_key_mackey_req[0x20] = { 0 };
    uint8_t aes_key_extractorkey_req[0x20] = { 0 };
    aes_keys_from_sp_key(ssc_state, kbkdf_index, request, aes_key_mackey_req,
                         aes_key_extractorkey_req);
    uint8_t cmac_req_is[AES_BLOCK_SIZE] = { 0 };
    aes_cmac_prefix_public(aes_key_mackey_req, request, sw_public_xy2,
                           cmac_req_is);
    HEXDUMP("answer_cmd_0x1_connect_sp: aes_key_mackey_req", aes_key_mackey_req,
            sizeof(aes_key_mackey_req));
    HEXDUMP("answer_cmd_0x1_connect_sp: aes_key_extractorkey_req ",
            aes_key_extractorkey_req, sizeof(aes_key_extractorkey_req));
    HEXDUMP("answer_cmd_0x1_connect_sp: cmac_req_is", cmac_req_is,
            sizeof(cmac_req_is));
    if (memcmp(cmac_req_should, cmac_req_is, sizeof(cmac_req_is)) != 0) {
        DBGLOG("%s: invalid CMAC\n", __func__);
        do_response_prefix(request, response, SSC_RESPONSE_FLAG_CMAC_INVALID);
        goto jump_ret0;
    }
    do_response_prefix(request, response, SSC_RESPONSE_FLAG_OK);
    output_ec_pub(&ecc_pub, &response[MSG_PREFIX_LENGTH + AES_BLOCK_SIZE]);
    generate_kbkdf_keys(ssc_state, &ssc_state->ecc_keys[kbkdf_index],
                        &cmd1_ecpub, aes_key_extractorkey_req,
                        INFOSTR_AKE_SESSIONSEED, request, kbkdf_index);

    uint8_t *cmac_resp = &response[MSG_PREFIX_LENGTH];
    uint8_t *public_xy2 = &response[MSG_PREFIX_LENGTH + AES_BLOCK_SIZE];
    aes_cmac_prefix_public_public(aes_key_mackey_req, response, sw_public_xy2,
                                  public_xy2, cmac_resp);

    HEXDUMP("cmd_0x01_resp", response, SSC_RESPONSE_SIZE_CMD_0x1);
jump_ret0:
    ecc_point_clear(&ecc_pub);
jump_ret1:
    ecc_point_clear(&cmd1_ecpub);
    return 0;
}

static int answer_cmd_0x2_disconnect_sp(struct AppleSSCState *ssc_state,
                                        uint8_t *request, uint8_t *response)
{
    DBGLOG("%s: entered function\n", __func__);
    HEXDUMP("cmd_0x02_req", request, SSC_REQUEST_SIZE_CMD_0x2);
    uint8_t kbkdf_index = request[1];
    if (!is_keyslot_valid(ssc_state, kbkdf_index)) { // should already exist
        DBGLOG("%s: invalid kbkdf_index: %u\n", __func__, kbkdf_index);
        do_response_prefix(request, response,
                           SSC_RESPONSE_FLAG_KEYSLOT_INVALID);
        return 0;
    }
    do_response_prefix(request, response, SSC_RESPONSE_FLAG_OK);
    clear_ecc_scalar(&ssc_state->ecc_keys[kbkdf_index]);
    memset(&ssc_state->kbkdf_keys[kbkdf_index], 0,
           sizeof(ssc_state->kbkdf_keys[kbkdf_index]));
    ssc_state->kbkdf_counter[kbkdf_index] = 0;
    DBGLOG("answer_cmd_0x2_disconnect_sp: kbkdf_index: %u\n", kbkdf_index);
    return 0;
}

static int answer_cmd_0x3_metadata_write(struct AppleSSCState *ssc_state,
                                         uint8_t *request, uint8_t *response)
{
    DBGLOG("%s: entered function\n", __func__);
    HEXDUMP("cmd_0x03_req", request, SSC_REQUEST_SIZE_CMD_0x3);
    uint8_t kbkdf_index_key = request[1];
    uint8_t kbkdf_index_dataslot = request[2];
    uint8_t copy = request[3];
    DBGLOG("cmd_0x03_req: kbkdf_index_key: %u\n", kbkdf_index_key);
    DBGLOG("cmd_0x03_req: kbkdf_index_dataslot: %u\n", kbkdf_index_dataslot);
    DBGLOG("cmd_0x03_req: copy: %u\n", copy);
    ////if (copy >= SSC_REQUEST_MAX_COPIES)
    if (copy > 0) {
        DBGLOG("%s: invalid copy: %u\n", __func__, copy);
        do_response_prefix(request, response,
                           SSC_RESPONSE_FLAG_COMMAND_OR_FIELD_INVALID);
        return 0;
    }
    if (kbkdf_index_key >= KBKDF_KEY_MAX_SLOTS ||
        !is_keyslot_valid(ssc_state, kbkdf_index_key)) {
        DBGLOG("%s: invalid kbkdf_index_key: %u\n", __func__, kbkdf_index_key);
        do_response_prefix(request, response,
                           SSC_RESPONSE_FLAG_KEYSLOT_INVALID);
        return 0;
    }
    if (kbkdf_index_dataslot == 0 ||
        kbkdf_index_dataslot >= KBKDF_KEY_MAX_SLOTS) {
        DBGLOG("%s: invalid kbkdf_index_dataslot: %u\n", __func__,
               kbkdf_index_dataslot);
        do_response_prefix(request, response,
                           SSC_RESPONSE_FLAG_KEYSLOT_INVALID);
        return 0;
    }
    int blk_offset =
        (kbkdf_index_dataslot * CMD_METADATA_DATA_PAYLOAD_LENGTH *
        SSC_REQUEST_MAX_COPIES) + (copy * CMD_METADATA_DATA_PAYLOAD_LENGTH);
    int key_offset =
        (KBKDF_KEY_KEY_FILE_OFFSET * CMD_METADATA_DATA_PAYLOAD_LENGTH *
        SSC_REQUEST_MAX_COPIES) + (kbkdf_index_dataslot * KBKDF_KEY_KEY_LENGTH);
    DBGLOG("cmd_0x03_req: blk_offset: 0x%x\n", blk_offset);
    HEXDUMP("cmd_0x03_req: ssc_state->kbkdf_keys[kbkdf_index_key]",
            ssc_state->kbkdf_keys[kbkdf_index_key], KBKDF_CMAC_OUTPUT_LEN);

    uint8_t req_dec_out[CMD_METADATA_PAYLOAD_LENGTH] = { 0 };
    int err0 = aes_ccm_crypt(
        ssc_state, kbkdf_index_key, &request[0x00], CMD_METADATA_PAYLOAD_LENGTH,
        &request[MSG_PREFIX_LENGTH], req_dec_out, false, false);
    if (err0 == 0) {
        DBGLOG("%s: invalid CMAC\n", __func__);
        do_response_prefix(request, response, SSC_RESPONSE_FLAG_CMAC_INVALID);
        return 0;
    }
    do_response_prefix(request, response, SSC_RESPONSE_FLAG_OK);
    HEXDUMP("cmd_0x03_req: req_dec_out", req_dec_out,
            CMD_METADATA_PAYLOAD_LENGTH);

    memcpy(ssc_state->slot_hmac_key[kbkdf_index_dataslot], req_dec_out,
           sizeof(req_dec_out)); // 0x20 bytes ; necessary here because there
                                 // are no metadata reads (cmd 0x6) after that.

    // blk_pwrite(ssc_state->blk, blk_offset, CMD_METADATA_PAYLOAD_LENGTH,
    // req_dec_out, 0); // Is it really necessary to write the mac_key or any
    // metadata to blk_offset?
    uint8_t zeroes_0x40[CMD_METADATA_DATA_PAYLOAD_LENGTH] = { 0 };
    blk_pwrite(ssc_state->blk, blk_offset, CMD_METADATA_DATA_PAYLOAD_LENGTH,
               zeroes_0x40, 0); // clear it on metadata write, all 0x40 bytes at
                                // blk_offset. is this correct?
    blk_pwrite(ssc_state->blk, key_offset, CMD_METADATA_PAYLOAD_LENGTH,
               req_dec_out, 0);

    uint8_t resp_nop_out[1] = { 0x00 };
    HEXDUMP("cmd_0x03_resp: resp_nop_out", resp_nop_out, 1);
    int err1 =
        aes_ccm_crypt(ssc_state, kbkdf_index_key, &response[0x00], 0x0,
                      resp_nop_out, &response[MSG_PREFIX_LENGTH], true, true);
    HEXDUMP("cmd_0x03_resp", response, SSC_RESPONSE_SIZE_CMD_0x3);

    return 0;
}

static int answer_cmd_0x4_metadata_data_read(struct AppleSSCState *ssc_state,
                                             uint8_t *request,
                                             uint8_t *response)
{
    DBGLOG("%s: entered function\n", __func__);
    HEXDUMP("cmd_0x04_req", request, SSC_REQUEST_SIZE_CMD_0x4);
    uint8_t kbkdf_index = request[1];
    uint8_t copy = request[3];
    DBGLOG("cmd_0x04_req: kbkdf_index: %u\n", kbkdf_index);
    DBGLOG("cmd_0x04_req: copy: %u\n", copy);
    if (copy >= SSC_REQUEST_MAX_COPIES) {
        DBGLOG("%s: invalid copy: %u\n", __func__, copy);
        do_response_prefix(request, response,
                           SSC_RESPONSE_FLAG_COMMAND_OR_FIELD_INVALID);
        return 0;
    }
    if (kbkdf_index == 0 || kbkdf_index >= KBKDF_KEY_MAX_SLOTS ||
        !is_keyslot_valid(ssc_state, kbkdf_index)) {
        DBGLOG("%s: invalid kbkdf_index: %u\n", __func__, kbkdf_index);
        do_response_prefix(request, response,
                           SSC_RESPONSE_FLAG_KEYSLOT_INVALID);
        return 0;
    }
    int blk_offset = (kbkdf_index * CMD_METADATA_DATA_PAYLOAD_LENGTH *
                      SSC_REQUEST_MAX_COPIES) +
                     (copy * CMD_METADATA_DATA_PAYLOAD_LENGTH);
    DBGLOG("cmd_0x04_req: blk_offset: 0x%x\n", blk_offset);
    HEXDUMP("cmd_0x04_req: ssc_state->kbkdf_keys[kbkdf_index]",
            ssc_state->kbkdf_keys[kbkdf_index], KBKDF_CMAC_OUTPUT_LEN);

    uint8_t req_nop_out[1] = { 0 };
    int err0 =
        aes_ccm_crypt(ssc_state, kbkdf_index, &request[0x00], 0x0,
                      &request[MSG_PREFIX_LENGTH], req_nop_out, false, false);
    if (err0 == 0) {
        DBGLOG("%s: invalid CMAC\n", __func__);
        do_response_prefix(request, response, SSC_RESPONSE_FLAG_CMAC_INVALID);
        return 0;
    }
    do_response_prefix(request, response, SSC_RESPONSE_FLAG_OK);
    HEXDUMP("cmd_0x04_req: req_nop_out", req_nop_out, 1);

    uint8_t resp_dec_out[CMD_METADATA_DATA_PAYLOAD_LENGTH] = { 0 };
    blk_pread(ssc_state->blk, blk_offset, CMD_METADATA_DATA_PAYLOAD_LENGTH,
              resp_dec_out, 0);

    HEXDUMP("cmd_0x04_resp: resp_dec_out", resp_dec_out,
            CMD_METADATA_DATA_PAYLOAD_LENGTH);
    int err1 = aes_ccm_crypt(ssc_state, kbkdf_index, &response[0x00],
                             CMD_METADATA_DATA_PAYLOAD_LENGTH, resp_dec_out,
                             &response[MSG_PREFIX_LENGTH], true, true);
    HEXDUMP("cmd_0x04_resp", response, SSC_RESPONSE_SIZE_CMD_0x4);

    return 0;
}

static int answer_cmd_0x5_metadata_data_write(struct AppleSSCState *ssc_state,
                                              uint8_t *request,
                                              uint8_t *response)
{
    DBGLOG("%s: entered function\n", __func__);
    HEXDUMP("cmd_0x05_req", request, SSC_REQUEST_SIZE_CMD_0x5);
    uint8_t kbkdf_index = request[1];
    uint8_t copy = request[3];
    DBGLOG("cmd_0x05_req: kbkdf_index: %u\n", kbkdf_index);
    DBGLOG("cmd_0x05_req: copy: %u\n", copy);
    if (copy >= SSC_REQUEST_MAX_COPIES) {
        DBGLOG("%s: invalid copy: %u\n", __func__, copy);
        do_response_prefix(request, response,
                           SSC_RESPONSE_FLAG_COMMAND_OR_FIELD_INVALID);
        return 0;
    }
    if (kbkdf_index == 0 || kbkdf_index >= KBKDF_KEY_MAX_SLOTS ||
        !is_keyslot_valid(ssc_state, kbkdf_index)) {
        DBGLOG("%s: invalid kbkdf_index: %u\n", __func__, kbkdf_index);
        do_response_prefix(request, response,
                           SSC_RESPONSE_FLAG_KEYSLOT_INVALID);
        return 0;
    }
    int blk_offset = (kbkdf_index * CMD_METADATA_DATA_PAYLOAD_LENGTH *
                      SSC_REQUEST_MAX_COPIES) +
                     (copy * CMD_METADATA_DATA_PAYLOAD_LENGTH);
    DBGLOG("cmd_0x05_req: blk_offset: 0x%x\n", blk_offset);
    HEXDUMP("cmd_0x05_req: ssc_state->kbkdf_keys[kbkdf_index]",
            ssc_state->kbkdf_keys[kbkdf_index], KBKDF_CMAC_OUTPUT_LEN);

    uint8_t req_dec_out[CMD_METADATA_DATA_PAYLOAD_LENGTH] = { 0 };
    int err0 =
        aes_ccm_crypt(ssc_state, kbkdf_index, &request[0x00],
                      CMD_METADATA_DATA_PAYLOAD_LENGTH,
                      &request[MSG_PREFIX_LENGTH], req_dec_out, false, false);
    if (err0 == 0) {
        DBGLOG("%s: invalid CMAC\n", __func__);
        do_response_prefix(request, response, SSC_RESPONSE_FLAG_CMAC_INVALID);
        return 0;
    }
    do_response_prefix(request, response, SSC_RESPONSE_FLAG_OK);
    HEXDUMP("cmd_0x05_req: req_dec_out", req_dec_out,
            CMD_METADATA_DATA_PAYLOAD_LENGTH);

    blk_pwrite(ssc_state->blk, blk_offset, CMD_METADATA_DATA_PAYLOAD_LENGTH,
               req_dec_out, 0);

    uint8_t resp_nop_out[1] = { 0x00 };
    HEXDUMP("cmd_0x05_resp: resp_nop_out", resp_nop_out, 1);
    int err1 =
        aes_ccm_crypt(ssc_state, kbkdf_index, &response[0x00], 0x0,
                      resp_nop_out, &response[MSG_PREFIX_LENGTH], true, true);
    HEXDUMP("cmd_0x05_resp", response, SSC_RESPONSE_SIZE_CMD_0x5);

    return 0;
}

static int answer_cmd_0x6_metadata_read(struct AppleSSCState *ssc_state,
                                        uint8_t *request, uint8_t *response)
{
    DBGLOG("%s: entered function\n", __func__);
    HEXDUMP("cmd_0x06_req", request, SSC_REQUEST_SIZE_CMD_0x6);

    uint8_t kbkdf_index_key = request[1];
    uint8_t kbkdf_index_dataslot = request[2];
    uint8_t copy = request[3];
    DBGLOG("cmd_0x06_req: kbkdf_index_key: %u\n", kbkdf_index_key);
    DBGLOG("cmd_0x06_req: kbkdf_index_dataslot: %u\n", kbkdf_index_dataslot);
    DBGLOG("cmd_0x06_req: copy: %u\n", copy);
    if (copy >= SSC_REQUEST_MAX_COPIES) {
        DBGLOG("%s: invalid copy: %u\n", __func__, copy);
        do_response_prefix(request, response,
                           SSC_RESPONSE_FLAG_COMMAND_OR_FIELD_INVALID);
        return 0;
    }
    if (kbkdf_index_key >= KBKDF_KEY_MAX_SLOTS ||
        !is_keyslot_valid(ssc_state, kbkdf_index_key)) {
        DBGLOG("%s: invalid kbkdf_index_key: %u\n", __func__, kbkdf_index_key);
        do_response_prefix(request, response,
                           SSC_RESPONSE_FLAG_KEYSLOT_INVALID);
        return 0;
    }
    if (kbkdf_index_dataslot == 0 ||
        kbkdf_index_dataslot >= KBKDF_KEY_MAX_SLOTS) {
        DBGLOG("%s: invalid kbkdf_index_dataslot: %u\n", __func__,
               kbkdf_index_dataslot);
        do_response_prefix(request, response,
                           SSC_RESPONSE_FLAG_KEYSLOT_INVALID);
        return 0;
    }
    int blk_offset =
        (kbkdf_index_dataslot * CMD_METADATA_DATA_PAYLOAD_LENGTH *
         SSC_REQUEST_MAX_COPIES) +
        (copy * CMD_METADATA_DATA_PAYLOAD_LENGTH);
    int key_offset =
        (KBKDF_KEY_KEY_FILE_OFFSET * CMD_METADATA_DATA_PAYLOAD_LENGTH *
         SSC_REQUEST_MAX_COPIES) +
        (kbkdf_index_dataslot * KBKDF_KEY_KEY_LENGTH);
    DBGLOG("cmd_0x06_req: blk_offset: 0x%x\n", blk_offset);
    HEXDUMP("cmd_0x06_req: ssc_state->kbkdf_keys[kbkdf_index_key]",
            ssc_state->kbkdf_keys[kbkdf_index_key], KBKDF_CMAC_OUTPUT_LEN);

    uint8_t req_nop_out[1] = { 0 };
    int err0 =
        aes_ccm_crypt(ssc_state, kbkdf_index_key, &request[0x00], 0x0,
                      &request[MSG_PREFIX_LENGTH], req_nop_out, false, false);
    if (err0 == 0) {
        DBGLOG("%s: invalid CMAC\n", __func__);
        do_response_prefix(request, response, SSC_RESPONSE_FLAG_CMAC_INVALID);
        return 0;
    }
    do_response_prefix(request, response, SSC_RESPONSE_FLAG_OK);
    HEXDUMP("cmd_0x06_req: req_nop_out", req_nop_out, 1);

    uint8_t resp_dec_out[CMD_METADATA_PAYLOAD_LENGTH] = { 0 };
    blk_pread(ssc_state->blk, blk_offset, CMD_METADATA_PAYLOAD_LENGTH,
              resp_dec_out, 0);
    blk_pread(ssc_state->blk, key_offset, CMD_METADATA_PAYLOAD_LENGTH,
              ssc_state->slot_hmac_key[kbkdf_index_dataslot], 0);

    HEXDUMP("cmd_0x06_resp: resp_dec_out", resp_dec_out,
            CMD_METADATA_PAYLOAD_LENGTH);
    int err1 = aes_ccm_crypt(ssc_state, kbkdf_index_key, &response[0x00],
                             CMD_METADATA_PAYLOAD_LENGTH, resp_dec_out,
                             &response[MSG_PREFIX_LENGTH], true, true);
    HEXDUMP("cmd_0x06_resp", response, SSC_RESPONSE_SIZE_CMD_0x6);

    return 0;
}

static int answer_cmd_0x7_init0(struct AppleSSCState *ssc_state,
                                uint8_t *request, uint8_t *response)
{
    struct ecc_point ecc_pub;
    DBGLOG("%s: entered function\n", __func__);

    const char *priv_str = "cccccccccccccccccccccccccccccccccccccccccccccccc"
                     "cccccccccccccccccccccccccccccccccccccccccccccccc";
    int err = generate_ec_priv(priv_str, &ssc_state->ecc_key_main, &ecc_pub);
    do_response_prefix(request, response, SSC_RESPONSE_FLAG_OK);
    uint8_t unknown0[0x06] = { 0x12, 0x34, 0x56, 0x78, 0x90, 0xab };
    uint8_t cpsn[0x07] = { 0xde, 0xad, 0xbe, 0xef, 0xca, 0xfe, 0xcc };
    uint8_t unknown1[0x07] = { 0xcd, 0xef, 0x01, 0x02, 0x03, 0x04, 0x05 };
    memcpy(ssc_state->cpsn, cpsn, sizeof(ssc_state->cpsn));
    memcpy(&response[MSG_PREFIX_LENGTH], unknown0, sizeof(unknown0));
    memcpy(&response[MSG_PREFIX_LENGTH + sizeof(unknown0)], ssc_state->cpsn,
           sizeof(ssc_state->cpsn));
    memcpy(&response[MSG_PREFIX_LENGTH + sizeof(unknown0) +
                     sizeof(ssc_state->cpsn)],
           unknown1, sizeof(unknown1));
    output_ec_pub(&ecc_pub,
                  &response[MSG_PREFIX_LENGTH + sizeof(unknown0) +
                            sizeof(ssc_state->cpsn) + sizeof(unknown1)]);
    ecc_point_clear(&ecc_pub);

    HEXDUMP("cmd_0x07_resp", response, SSC_RESPONSE_SIZE_CMD_0x7);
    return 0;
}

static int answer_cmd_0x8_sleep(struct AppleSSCState *ssc_state,
                                uint8_t *request, uint8_t *response)
{
    DBGLOG("%s: entered function\n", __func__);
    do_response_prefix(request, response, SSC_RESPONSE_FLAG_OK);
    HEXDUMP("cmd_0x08_resp", response, SSC_RESPONSE_SIZE_CMD_0x8);
    return 0;
}

static int answer_cmd_0x9_panic(struct AppleSSCState *ssc_state,
                                uint8_t *request, uint8_t *response)
{
    DBGLOG("%s: entered function\n", __func__);
    ////apple_ssc_reset(DEVICE(ssc_state));
    do_response_prefix(request, response, SSC_RESPONSE_FLAG_OK);
    // uint8_t panic_data[0x24] = {...};
    // memcpy(&response[MSG_PREFIX_LENGTH], panic_data, 0x24);
    memset(&response[MSG_PREFIX_LENGTH], 0xcc, 0x24);
    memcpy(&response[MSG_PREFIX_LENGTH + 0x24], ssc_state->cpsn,
           sizeof(ssc_state->cpsn));
    HEXDUMP("cmd_0x09_resp", response, SSC_RESPONSE_SIZE_CMD_0x9);
    return 0;
}

static uint8_t apple_ssc_rx(I2CSlave *i2c)
{
    AppleSSCState *ssc = APPLE_SSC(i2c);
    uint8_t ret = 0;

    // ssc->req_cur = 0;

    if (ssc->resp_cur >= sizeof(ssc->resp_cmd)) {
        qemu_log_mask(LOG_UNIMP, "%s: ssc->resp_cur too high 0x%02x\n",
                      __func__, ssc->resp_cur);
        return 0;
    }

    if (ssc->resp_cur == 0) {
        // ssc->req_cur = 0;
        memset(ssc->resp_cmd, 0, sizeof(ssc->resp_cmd));
        ssc->resp_cmd[0] = ssc->req_cmd[0];
    }
    // This tries to prevent a spurious call during a dummy read.
    if (ssc->resp_cur == 1) {
        uint8_t cmd = ssc->req_cmd[0];
        if (cmd > 0x09) {
            qemu_log_mask(LOG_UNIMP,
                          "%s: cmd %u: invalid command > 0x09", __func__, cmd);
            do_response_prefix(ssc->req_cmd, ssc->resp_cmd,
                               SSC_RESPONSE_FLAG_COMMAND_OR_FIELD_INVALID);
        } else if (ssc->req_cur != ssc_request_sizes[cmd]) {
            qemu_log_mask(LOG_UNIMP,
                          "%s: cmd %u: invalid cmdsize mismatch req_cur "
                          "is 0x%02x != should 0x%02x\n",
                          __func__, cmd, ssc->req_cur, ssc_request_sizes[cmd]);
            do_response_prefix(ssc->req_cmd, ssc->resp_cmd,
                               SSC_RESPONSE_FLAG_COMMAND_SIZE_MISMATCH);
        } else if (cmd == 0x00) { // req 0x84 bytes, resp 0xc4 bytes
            answer_cmd_0x0_init1(ssc, ssc->req_cmd, ssc->resp_cmd);
        } else if (cmd == 0x01) { // req 0x74 bytes, resp 0x74 bytes
            answer_cmd_0x1_connect_sp(ssc, ssc->req_cmd, ssc->resp_cmd);
        } else if (cmd == 0x02) { // req 0x04 bytes, resp 0x04 bytes
            answer_cmd_0x2_disconnect_sp(ssc, ssc->req_cmd, ssc->resp_cmd);
        } else if (cmd == 0x03) { // req 0x34 bytes, resp 0x14 bytes
            answer_cmd_0x3_metadata_write(ssc, ssc->req_cmd, ssc->resp_cmd);
        } else if (cmd == 0x04) { // req 0x14 bytes, resp 0x54 bytes
            answer_cmd_0x4_metadata_data_read(ssc, ssc->req_cmd, ssc->resp_cmd);
        } else if (cmd == 0x05) { // req 0x54 bytes, resp 0x14 bytes
            answer_cmd_0x5_metadata_data_write(ssc, ssc->req_cmd,
                                               ssc->resp_cmd);
        } else if (cmd == 0x06) { // req 0x14 bytes, resp 0x34 bytes
            answer_cmd_0x6_metadata_read(ssc, ssc->req_cmd, ssc->resp_cmd);
        } else if (cmd == 0x07) { // req 0x04 bytes, resp 0x78 bytes
            answer_cmd_0x7_init0(ssc, ssc->req_cmd, ssc->resp_cmd);
        } else if (cmd == 0x08) { // req 0x04 bytes, resp 0x04 bytes
            answer_cmd_0x8_sleep(ssc, ssc->req_cmd, ssc->resp_cmd);
        } else if (cmd == 0x09) { // req 0x04 bytes, resp 0x2f bytes
            answer_cmd_0x9_panic(ssc, ssc->req_cmd, ssc->resp_cmd);
        }
        ssc->req_cur = 0;
        memset(ssc->req_cmd, 0, sizeof(ssc->req_cmd));
        if (ssc->resp_cmd[3] != SSC_RESPONSE_FLAG_OK) {
            memset(&ssc->resp_cmd[MSG_PREFIX_LENGTH], 0xff,
                   sizeof(ssc->resp_cmd) - MSG_PREFIX_LENGTH);
        }
    }

    ret = ssc->resp_cmd[ssc->resp_cur++];
    qemu_log_mask(LOG_UNIMP, "apple_ssc_rx: resp_cur=0x%02x ret=0x%02x\n",
                  ssc->resp_cur - 1, ret);
#if 0
    MachineState *machine = MACHINE(qdev_get_machine());
    AppleSEPState *sep;
    sep = APPLE_SEP(object_property_get_link(OBJECT(machine), "sep", &error_fatal));
    apple_a7iop_interrupt_status_push(a7iop->iop_mailbox, 0x10002); // I2C
#endif
    return ret;
}

static int apple_ssc_tx(I2CSlave *i2c, uint8_t data)
{
    AppleSSCState *ssc = APPLE_SSC(i2c);

    if (ssc->req_cur == 0) {
        ssc->resp_cur = 0;
        memset(ssc->resp_cmd, 0, sizeof(ssc->resp_cmd));
    }

    if (ssc->req_cur >= sizeof(ssc->req_cmd)) {
        qemu_log_mask(LOG_UNIMP, "apple_ssc_tx: ssc->req_cur too high 0x%02x\n",
                      ssc->req_cur);
        return 0;
    }

    qemu_log_mask(LOG_UNIMP, "apple_ssc_tx: req_cur=0x%02x data=0x%02x\n",
                  ssc->req_cur, data);
    ssc->req_cmd[ssc->req_cur++] = data;
    return 0;
}

static void apple_ssc_reset(DeviceState *state)
{
    AppleSSCState *ssc = APPLE_SSC(state);
    qemu_log_mask(LOG_UNIMP, "%s: called\n", __func__);

    ssc->req_cur = 0;
    ssc->resp_cur = 0;
    memset(ssc->req_cmd, 0, sizeof(ssc->req_cmd));
    memset(ssc->resp_cmd, 0, sizeof(ssc->resp_cmd));

    clear_ecc_scalar(&ssc->ecc_key_main);
    for (int i = 0; i < KBKDF_KEY_MAX_SLOTS; i++) {
        clear_ecc_scalar(&ssc->ecc_keys[i]);
    }
    memset(ssc->random_hmac_key, 0, sizeof(ssc->random_hmac_key));
    memset(ssc->slot_hmac_key, 0, sizeof(ssc->slot_hmac_key));
    memset(ssc->kbkdf_keys, 0, sizeof(ssc->kbkdf_keys));
    memset(ssc->kbkdf_counter, 0, sizeof(ssc->kbkdf_counter));
    uint8_t cpsn[0x07] = { 0xde, 0xad, 0xbe, 0xef, 0xca, 0xfe, 0xfe };
    memcpy(ssc->cpsn, cpsn, sizeof(cpsn));
    blk_set_perm(ssc->blk, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                 BLK_PERM_ALL, &error_fatal);
}

AppleSSCState *apple_ssc_create(MachineState *machine, uint8_t addr)
{
    AppleSSCState *ssc;
    AppleI2CState *i2c = APPLE_I2C(
        object_property_get_link(OBJECT(machine), "sep_i2c", &error_fatal));
    ssc = APPLE_SSC(i2c_slave_create_simple(i2c->bus, TYPE_APPLE_SSC, addr));
    return ssc;
}

static Property apple_ssc_props[] = {
    DEFINE_PROP_DRIVE("drive", AppleSSCState, blk),
    DEFINE_PROP_END_OF_LIST(),
};

static void apple_ssc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *c = I2C_SLAVE_CLASS(klass);

    dc->desc = "Apple SSC";
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);

    c->event = apple_ssc_event;
    c->recv = apple_ssc_rx;
    c->send = apple_ssc_tx;
    device_class_set_legacy_reset(dc, apple_ssc_reset);

    device_class_set_props(dc, apple_ssc_props);
}

static const TypeInfo apple_ssc_type_info = {
    .name = TYPE_APPLE_SSC,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(AppleSSCState),
    .class_init = apple_ssc_class_init,
};

static void apple_ssc_register_types(void)
{
    type_register_static(&apple_ssc_type_info);
}

type_init(apple_ssc_register_types);
