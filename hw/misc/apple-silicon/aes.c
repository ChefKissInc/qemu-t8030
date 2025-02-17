#include "qemu/osdep.h"
#include "crypto/cipher.h"
#include "hw/arm/apple-silicon/dtb.h"
#include "hw/irq.h"
#include "hw/misc/apple-silicon/aes.h"
#include "hw/misc/apple-silicon/aes_reg.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/bitops.h"
#include "qemu/lockable.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/rcu.h"
#include "sysemu/dma.h"
#include "trace.h"

OBJECT_DECLARE_SIMPLE_TYPE(AppleAESState, APPLE_AES)

typedef struct AESCommand {
    uint32_t command;
    uint32_t *data;
    uint32_t data_len;
    QTAILQ_ENTRY(AESCommand) entry;
} AESCommand;

typedef struct AESKey {
    QCryptoCipher *cipher;
    key_select_t select;
    QCryptoCipherAlgo algo;
    uint8_t key[32];
    uint32_t len;
    key_func_t func;
    block_mode_t mode;
    bool wrapped;
    bool encrypt;
    bool disabled;
    uint8_t id;
} AESKey;

struct AppleAESState {
    SysBusDevice parent_obj;
    MemoryRegion iomems[2];
    MemoryRegion *dma_mr;
    AddressSpace dma_as;
    qemu_irq irq;
    int last_level;
    aes_reg_t reg;
    QemuMutex mutex;
    QemuThread thread;
    QemuCond thread_cond;
    QemuMutex queue_mutex;
    QTAILQ_HEAD(, AESCommand) queue;
    uint32_t command;
    uint32_t *data;
    uint32_t data_len;
    uint32_t data_read;
    AESKey keys[2];
    uint8_t iv[4][16];
    bool stopped;
    uint32_t board_id;
};

static uint32_t key_size(uint8_t len)
{
    switch (len) {
    case KEY_LEN_128:
        return 128;
    case KEY_LEN_192:
        return 192;
    case KEY_LEN_256:
        return 256;
    default:
        return 0;
    }
}

static QCryptoCipherAlgo key_algo(uint8_t mode)
{
    switch (mode) {
    case KEY_LEN_128:
        return QCRYPTO_CIPHER_ALGO_AES_128;
    case KEY_LEN_192:
        return QCRYPTO_CIPHER_ALGO_AES_192;
    case KEY_LEN_256:
        return QCRYPTO_CIPHER_ALGO_AES_256;
    default:
        return QCRYPTO_CIPHER_ALGO__MAX;
    }
}

static QCryptoCipherMode key_mode(block_mode_t mode)
{
    switch (mode) {
    case BLOCK_MODE_ECB:
        return QCRYPTO_CIPHER_MODE_ECB;
    case BLOCK_MODE_CBC:
        return QCRYPTO_CIPHER_MODE_CBC;
    case BLOCK_MODE_CTR:
        return QCRYPTO_CIPHER_MODE_CTR;
    default:
        return QCRYPTO_CIPHER_MODE__MAX;
    }
}

static void apple_aes_reset(DeviceState *s);
static void *aes_thread(void *opaque);

static void aes_update_irq(AppleAESState *s)
{
    if (s->reg.int_enable.raw & qatomic_read(&s->reg.int_status.raw)) {
        if (!s->last_level) {
            s->last_level = 1;
            qemu_irq_raise(s->irq);
            trace_apple_aes_update_irq(1);
        }
    } else if (s->last_level) {
        s->last_level = 0;
        qemu_irq_lower(s->irq);
        trace_apple_aes_update_irq(0);
    }
}

static void aes_update_command_fifo_status(AppleAESState *s)
{
    /* TODO: implement read/write_pointer */
    s->reg.command_fifo_status.empty = s->reg.command_fifo_status.level == 0;
    s->reg.command_fifo_status.full =
        s->reg.command_fifo_status.level >= COMMAND_FIFO_SIZE;
    s->reg.command_fifo_status.overflow =
        s->reg.command_fifo_status.level > COMMAND_FIFO_SIZE;
    s->reg.command_fifo_status.low =
        s->reg.command_fifo_status.level < s->reg.watermarks.command_fifo_low;

    if (s->reg.command_fifo_status.low) {
        qatomic_or(&s->reg.int_status.raw, AES_BLK_INT_COMMAND_FIFO_LOW);
    } else {
        qatomic_and(&s->reg.int_status.raw, ~AES_BLK_INT_COMMAND_FIFO_LOW);
    }
    aes_update_irq(s);
}

static void aes_empty_fifo(AppleAESState *s)
{
    WITH_QEMU_LOCK_GUARD(&s->queue_mutex)
    {
        while (!QTAILQ_EMPTY(&s->queue)) {
            AESCommand *cmd = QTAILQ_FIRST(&s->queue);
            QTAILQ_REMOVE(&s->queue, cmd, entry);
            g_free(cmd);
        }
        s->reg.command_fifo_status.level = 0;
        aes_update_command_fifo_status(s);
    }
}

static void aes_start(AppleAESState *s)
{
    if (s->stopped) {
        s->stopped = false;
        qemu_thread_create(&s->thread, TYPE_APPLE_AES, aes_thread, s,
                           QEMU_THREAD_JOINABLE);
    }
}

static void aes_stop(AppleAESState *s)
{
    if (!s->stopped) {
        s->stopped = true;
        qemu_cond_signal(&s->thread_cond);
        qemu_thread_join(&s->thread);
    }
}

static bool aes_process_command(AppleAESState *s, AESCommand *cmd)
{
    trace_apple_aes_process_command(COMMAND_OPCODE(cmd->command));
    bool locked = false;
#define lock_reg()     \
    do {               \
        bql_lock();    \
        locked = true; \
    } while (0)
    switch (COMMAND_OPCODE(cmd->command)) {
    case OPCODE_KEY: {
        uint32_t ctx = COMMAND_KEY_COMMAND_KEY_CONTEXT(cmd->command);
        s->keys[ctx].select = COMMAND_KEY_COMMAND_KEY_SELECT(cmd->command);
        s->keys[ctx].algo =
            key_algo(COMMAND_KEY_COMMAND_KEY_LENGTH(cmd->command));
        s->keys[ctx].len =
            key_size(COMMAND_KEY_COMMAND_KEY_LENGTH(cmd->command)) / 8;
        s->keys[ctx].wrapped =
            (cmd->command & COMMAND_KEY_COMMAND_WRAPPED) != 0;
        s->keys[ctx].encrypt =
            (cmd->command & COMMAND_KEY_COMMAND_ENCRYPT) != 0;
        s->keys[ctx].func = COMMAND_KEY_COMMAND_KEY_FUNC(cmd->command);
        s->keys[ctx].mode = COMMAND_KEY_COMMAND_BLOCK_MODE(cmd->command);
        s->keys[ctx].id = COMMAND_KEY_COMMAND_COMMAND_ID(cmd->command);
        memcpy(s->keys[ctx].key, &cmd->data[1], s->keys[ctx].len);
        if (ctx) {
            s->reg.key_id.context_1 = s->keys[ctx].id;
        } else {
            s->reg.key_id.context_0 = s->keys[ctx].id;
        }
        if (s->keys[ctx].cipher) {
            qcrypto_cipher_free(s->keys[ctx].cipher);
            s->keys[ctx].cipher = NULL;
        }
        lock_reg();
        if (s->keys[ctx].select != KEY_SELECT_SOFTWARE) {
            s->keys[ctx].disabled = true;
            if (ctx) {
                qatomic_or(&s->reg.int_status.raw, AES_BLK_INT_KEY_1_DISABLED);
            } else {
                qatomic_or(&s->reg.int_status.raw, AES_BLK_INT_KEY_0_DISABLED);
            }
            qemu_log_mask(
                LOG_GUEST_ERROR,
                "%s: Attempting to select unsupported hardware key: 0x%x\n",
                __func__, s->keys[ctx].select);
        } else {
            if (s->keys[ctx].wrapped) {
                qemu_log_mask(LOG_GUEST_ERROR, "%s: What is wrapped key?\n",
                              __func__);
            }
            s->keys[ctx].disabled = false;
            if (ctx) {
                qatomic_and(&s->reg.int_status.raw,
                            ~AES_BLK_INT_KEY_1_DISABLED);
            } else {
                qatomic_and(&s->reg.int_status.raw,
                            ~AES_BLK_INT_KEY_0_DISABLED);
            }
            s->keys[ctx].cipher = qcrypto_cipher_new(
                s->keys[ctx].algo, key_mode(s->keys[ctx].mode),
                s->keys[ctx].key, s->keys[ctx].len, &error_abort);
        }
        break;
    }
    case OPCODE_IV: {
        uint32_t ctx = COMMAND_IV_COMMAND_IV_CONTEXT(cmd->command);
        memcpy(s->iv[ctx], &cmd->data[1], 16);
        break;
    }
    case OPCODE_DSB: {
        //memcpy(, &cmd->data[1], 16);
        //memcpy(, &cmd->data[5], 16);
        break;
    }
    case OPCODE_DATA: {
        command_data_t *c = (command_data_t *)cmd->data;
        uint32_t key_ctx = COMMAND_DATA_COMMAND_KEY_CONTEXT(c->command);
        uint32_t iv_ctx = COMMAND_DATA_COMMAND_IV_CONTEXT(c->command);
        uint32_t len = COMMAND_DATA_COMMAND_LENGTH(c->command);
        dma_addr_t source_addr = c->source_addr;
        dma_addr_t dest_addr = c->dest_addr;
        g_autofree uint8_t *buffer = NULL;
        g_autofree Error *errp = NULL;

        source_addr |=
            ((dma_addr_t)COMMAND_DATA_UPPER_ADDR_SOURCE(c->upper_addr)) << 32;
        dest_addr |= ((dma_addr_t)COMMAND_DATA_UPPER_ADDR_DEST(c->upper_addr))
                     << 32;
        if (len & 0xf) {
            lock_reg();
            qatomic_or(&s->reg.int_status.raw, AES_BLK_INT_INVALID_DATA_LENGTH);
            break;
        }
        if (s->keys[key_ctx].disabled || !s->keys[key_ctx].cipher) {
            lock_reg();
            if (key_ctx) {
                qatomic_or(&s->reg.int_status.raw, AES_BLK_INT_KEY_1_DISABLED);
            } else {
                qatomic_or(&s->reg.int_status.raw, AES_BLK_INT_KEY_0_DISABLED);
            }
            break;
        }

        buffer = g_malloc0(len);

        WITH_RCU_READ_LOCK_GUARD()
        {
            dma_memory_read(&s->dma_as, source_addr, buffer, len,
                            MEMTXATTRS_UNSPECIFIED);
        }
        qcrypto_cipher_setiv(s->keys[key_ctx].cipher, s->iv[iv_ctx], 16, &errp);

        if (s->keys[key_ctx].encrypt) {
            qcrypto_cipher_encrypt(s->keys[key_ctx].cipher, buffer, buffer, len,
                                   &errp);
        } else {
            qcrypto_cipher_decrypt(s->keys[key_ctx].cipher, buffer, buffer, len,
                                   &errp);
        }
        qcrypto_cipher_getiv(s->keys[key_ctx].cipher, s->iv[iv_ctx], 16, &errp);
        dma_memory_write(&s->dma_as, dest_addr, buffer, len,
                         MEMTXATTRS_UNSPECIFIED);
        break;
    }
    case OPCODE_STORE_IV: {
        command_store_iv_t *c = (command_store_iv_t *)cmd->data;
        dma_addr_t dest_addr = 0;
        uint32_t ctx = COMMAND_STORE_IV_COMMAND_CONTEXT(cmd->command);
        dest_addr = c->dest_addr;
        dest_addr |=
            ((dma_addr_t)COMMAND_STORE_IV_COMMAND_UPPER_ADDR_DEST(c->command))
            << 32;
        dma_memory_write(&s->dma_as, dest_addr, s->iv[ctx], 16,
                         MEMTXATTRS_UNSPECIFIED);
        break;
    }
    case OPCODE_FLAG:
        lock_reg();
        qatomic_set(&s->reg.flag_command.code,
                    COMMAND_FLAG_ID_CODE(cmd->command));
        if (cmd->command & COMMAND_FLAG_STOP_COMMANDS) {
            s->stopped = true;
        }
        if (cmd->command & COMMAND_FLAG_SEND_INTERRUPT) {
            qatomic_or(&s->reg.int_status.raw, AES_BLK_INT_FLAG_COMMAND);
        }
        break;
    default:
        lock_reg();
        qatomic_or(&s->reg.int_status.raw, AES_BLK_INT_INVALID_COMMAND);
        break;
    }

    return locked;
#undef lock_reg
}

static void *aes_thread(void *opaque)
{
    AppleAESState *s = APPLE_AES(opaque);
    rcu_register_thread();
    while (!s->stopped) {
        AESCommand *cmd = NULL;
        WITH_QEMU_LOCK_GUARD(&s->queue_mutex)
        {
            if (!QTAILQ_EMPTY(&s->queue)) {
                cmd = QTAILQ_FIRST(&s->queue);
                QTAILQ_REMOVE(&s->queue, cmd, entry);
            }
        }
        if (cmd) {
            if (!aes_process_command(s, cmd)) {
                bql_lock();
            }
            s->reg.command_fifo_status.level -= cmd->data_len;
            aes_update_command_fifo_status(s);
            bql_unlock();

            if (cmd->data) {
                g_free(cmd->data);
            }
            g_free(cmd);
        }
        WITH_QEMU_LOCK_GUARD(&s->queue_mutex)
        {
            while (QTAILQ_EMPTY(&s->queue) && !s->stopped) {
                qemu_cond_wait(&s->thread_cond, &s->queue_mutex);
            }
        }
    }
    rcu_unregister_thread();
    return NULL;
}

static void aes_security_reg_write(void *opaque, hwaddr addr, uint64_t data,
                                   unsigned size)
{
}

static uint64_t aes_security_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    AppleAESState *s;

    s = APPLE_AES(opaque);

#if 0
    qemu_log_mask(LOG_GUEST_ERROR, "%s: Read 0x" HWADDR_FMT_plx "\n", __func__,
                  addr);
#endif

    switch (addr) {
    case 0x00: // unknown0
        // return 0x00; // panic panic(cpu 0 caller 0xfffffff0090dcdb8):
        // "AppleS8000AESAccelerator::AES Error: IntStatus 0x0x2021\n" return
        // 0x0F; // no panic return 0xF0; // panic return 0x07; // no panic
        // return 0x03; // panic "AppleS8000AESAccelerator::AES Error: IntStatus
        // 0x0x2021\n" return 0x04; // panic "AppleS8000AESAccelerator::AES
        // Error: IntStatus 0x0xa1\n" return (1 << 2) | (1 << 1) | (0 << 0); //
        // panic "AppleS8000AESAccelerator::AES Error: IntStatus 0x0xa1\n"
        // return (1 << 2) | (0 << 1) | (1 << 0); // no panic
        // return (0 << 2) | (0 << 1) | (1 << 0); // panic
        // "AppleS8000AESAccelerator::AES Error: IntStatus 0x0x2021\n" return (0
        // << 2) | (1 << 1) | (0 << 0); // panic "AppleS8000AESAccelerator::AES
        // Error: IntStatus 0x0x2021\n" return (1 << 2) | (1 << 1) | (1 << 0);
        // // no panic
        return (1 << 2) | (1 << 0);
        //return 0xFF; // (val & 0xf)
    case 0x20: // board-id
        return s->board_id & 0x1f; // (val & 0x1f)
    case 0x30: // unknown1
        return 0x00;
        //return 0xFF; // many various flags // might cause sep skgs errors "panic(cpu 0 caller 0xfffffff008b69bb0): SEP Panic: :skg /skgs: 0x00017190 0x000169f8 0x000169d8 0x00011c2c 0x00011998 0x00013198 0x0000b98c 0x000160e8 [hnhpg]"
    case 0x34: // bit 24 = is fresh boot?
        return (1 << 24) | (1 << 25);
    default: // We don't know the rest
        return 0xFF;
    }
}

static void aes_reg_write(void *opaque, hwaddr addr, uint64_t data,
                          unsigned size)
{
    AppleAESState *s = APPLE_AES(opaque);
    uint32_t orig = data;
    uint32_t index = addr >> 2;
    uint32_t *mmio;
    uint32_t old;
    uint32_t val = data;
    int iflg = 0;
    bool nowrite = false;

    if (addr >= AES_BLK_REG_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x" HWADDR_FMT_plx "\n",
                      __func__, addr);
        return;
    }

    mmio = &s->reg.raw[index];
    old = *mmio;

    switch (addr) {
    case REG_AES_VERSION:
        QEMU_FALLTHROUGH;
    case REG_AES_STATUS:
        QEMU_FALLTHROUGH;
    case REG_AES_KEY_ID:
        QEMU_FALLTHROUGH;
    case REG_AES_AXI_STATUS:
        QEMU_FALLTHROUGH;
    case REG_AES_COMMAND_FIFO_STATUS:
        QEMU_FALLTHROUGH;
    case REG_AES_COMMAND_FIFO_COUNT:
        QEMU_FALLTHROUGH;
    case REG_AES_FLAG_COMMAND:
        QEMU_FALLTHROUGH;
    case REG_AES_SKG_KEY:
        nowrite = true;
        val = old;
        break;
    case REG_AES_INT_STATUS:
        nowrite = true;
        val = qatomic_and_fetch(&s->reg.int_status.raw, ~val);
        QEMU_FALLTHROUGH;
    case REG_AES_INT_ENABLE:
        iflg = 1;
        break;
    case REG_AES_WATERMARKS:
        aes_update_command_fifo_status(s);
        break;
    case REG_AES_CONTROL:
        switch (val) {
        case AES_BLK_CONTROL_START:
            aes_start(s);
            break;
        case AES_BLK_CONTROL_STOP:
            aes_stop(s);
            break;
        case AES_BLK_CONTROL_RESET:
            aes_empty_fifo(s);
            break;
        case AES_BLK_CONTROL_RESET_AES:
            apple_aes_reset(DEVICE(s));
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "REG_AES_CONTROL: Invalid write: 0x%x\n", val);
            break;
        }
        nowrite = true;
        val = old;
        break;
    case REG_AES_COMMAND_FIFO_S8000:
        QEMU_FALLTHROUGH;
    case REG_AES_COMMAND_FIFO:
        if (s->data_len > s->data_read) {
            s->data[s->data_read] = val;
            s->data_read++;
        } else {
            s->command = val;
            switch (COMMAND_OPCODE(val)) {
            case OPCODE_KEY:
                if (COMMAND_KEY_COMMAND_KEY_SELECT(val) ==
                    KEY_SELECT_SOFTWARE) {
                    uint32_t key_len =
                        key_size(COMMAND_KEY_COMMAND_KEY_LENGTH(val)) / 8;

                    s->data_len = key_len / 4 + 1;
                    s->data = g_new0(uint32_t, s->data_len);
                    s->data[0] = val;
                    s->data_read = 1;
                } else {
                    s->data_len = 1;
                    s->data = g_new0(uint32_t, s->data_len);
                    s->data[0] = val;
                    s->data_read = 1;
                }
                break;
            case OPCODE_IV:
                s->data_len = sizeof(command_iv_t) / 4;
                s->data = g_new0(uint32_t, s->data_len);
                s->data[0] = val;
                s->data_read = 1;
                break;
            case OPCODE_DSB:
                s->data_len = sizeof(command_dsb_t) / 4;
                s->data = g_new0(uint32_t, s->data_len);
                s->data[0] = val;
                s->data_read = 1;
                break;
            case OPCODE_DATA:
                s->data_len = sizeof(command_data_t) / 4;
                s->data = g_new0(uint32_t, s->data_len);
                s->data[0] = val;
                s->data_read = 1;
                break;
            case OPCODE_STORE_IV:
                s->data_len = sizeof(command_store_iv_t) / 4;
                s->data = g_new0(uint32_t, s->data_len);
                s->data[0] = val;
                s->data_read = 1;
                break;
            case OPCODE_FLAG:
                s->data_len = 1;
                s->data = g_new0(uint32_t, s->data_len);
                s->data[0] = val;
                s->data_read = 1;
                break;
            default:
                qatomic_or(&s->reg.int_status.raw, AES_BLK_INT_INVALID_COMMAND);
                iflg = 1;
                qemu_log_mask(LOG_GUEST_ERROR,
                              "REG_AES_COMMAND_FIFO: Unknown opcode: 0x%x\n",
                              COMMAND_OPCODE(val));
                break;
            }
        }

        if (s->data && s->data_len <= s->data_read) {
            AESCommand *cmd = g_malloc0(sizeof(AESCommand));
            cmd->command = s->command;
            cmd->data = s->data;
            cmd->data_len = s->data_len;

            s->command = 0;
            s->data = NULL;
            s->data_len = s->data_read = 0;

            WITH_QEMU_LOCK_GUARD(&s->queue_mutex)
            {
                QTAILQ_INSERT_TAIL(&s->queue, cmd, entry);
            }
            qemu_cond_signal(&s->thread_cond);
        }

        nowrite = true;
        val = 0;
        s->reg.command_fifo_status.level++;
        aes_update_command_fifo_status(s);
        break;
    case REG_AES_CONFIG:
        break;
    case REG_AES_CLEAR_FIFO:
        if (val == REG_AES_CLEAR_FIFO_RESET) {
            aes_empty_fifo(s);
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: write to unknown reg: 0x" HWADDR_FMT_plx "\n",
                      __func__, addr);
        break;
    }

    if (!nowrite) {
        *mmio = val;
    }

    if (iflg) {
        aes_update_irq(s);
    }
    trace_apple_aes_reg_write(addr, orig, old, val);
}

static uint64_t aes_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    AppleAESState *s = APPLE_AES(opaque);
    uint32_t val = 0;
    uint32_t *mmio = NULL;

    if (addr >= AES_BLK_REG_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: Bad offset 0x" HWADDR_FMT_plx "\n",
                      __func__, addr);
        return 0;
    }

    s->reg.status.text_dpa_random_seeded = 1;
    s->reg.status.key_unwrap_dpa_random_seeded = 1;
    s->reg.status.dpa_seeded_unk0 = 1;
    s->reg.status.dpa_seeded_unk1 = 1;
    s->reg.status.dpa_seeded_unk2 = 1;
    s->reg.status.dpa_seeded_unk3 = 1;
    s->reg.status.dpa_seeded_unk4 = 1;
    s->reg.status.dpa_seeded_unk5 = 1;
    mmio = &s->reg.raw[addr >> 2];

    switch (addr) {
    case REG_AES_INT_STATUS:
        QEMU_FALLTHROUGH;
    case REG_AES_COMMAND_FIFO_STATUS:
        QEMU_FALLTHROUGH;
    case REG_AES_FLAG_COMMAND:
        val = qatomic_read(mmio);
        break;
    default:
        val = s->reg.raw[addr >> 2];
        break;
    }
#if 0
    qemu_log_mask(LOG_UNIMP, "AES reg READ @ 0x%llx value: 0x%llx\n", addr, val);
#endif

    trace_apple_aes_reg_read(addr, val);
    return val;
}

static const MemoryRegionOps aes_reg_ops = {
    .write = aes_reg_write,
    .read = aes_reg_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned = false,
};

static const MemoryRegionOps aes_security_reg_ops = {
    .write = aes_security_reg_write,
    .read = aes_security_reg_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned = false,
};

static void apple_aes_reset(DeviceState *dev)
{
    AppleAESState *s = APPLE_AES(dev);

    memset(s->reg.raw, 0, AES_BLK_REG_SIZE);

    s->reg.status.text_dpa_random_seeded = 1;
    s->reg.status.key_unwrap_dpa_random_seeded = 1;

    s->command = 0;
    if (s->data) {
        g_free(s->data);
        s->data = NULL;
    }
    s->data_read = 0;
    s->data_len = 0;
    s->stopped = true;
    aes_stop(s);
    aes_empty_fifo(s);
}

static void apple_aes_realize(DeviceState *dev, Error **errp)
{
    AppleAESState *s = APPLE_AES(dev);
    Object *obj;

    obj = object_property_get_link(OBJECT(dev), "dma-mr", &error_abort);

    s->dma_mr = MEMORY_REGION(obj);
    address_space_init(&s->dma_as, s->dma_mr, TYPE_APPLE_AES);

    qemu_cond_init(&s->thread_cond);
    qemu_mutex_init(&s->queue_mutex);
    apple_aes_reset(dev);
}

static void apple_aes_unrealize(DeviceState *dev)
{
    AppleAESState *s = APPLE_AES(dev);

    apple_aes_reset(dev);
    qemu_cond_destroy(&s->thread_cond);
    qemu_mutex_destroy(&s->queue_mutex);
}

SysBusDevice *apple_aes_create(DTBNode *node, uint32_t board_id)
{
    DeviceState *dev;
    AppleAESState *s;
    SysBusDevice *sbd;
    DTBProp *prop;
    uint64_t *reg;

    dev = qdev_new(TYPE_APPLE_AES);
    s = APPLE_AES(dev);
    sbd = SYS_BUS_DEVICE(dev);

    s->board_id = board_id;

    prop = dtb_find_prop(node, "reg");
    g_assert_nonnull(prop);

    reg = (uint64_t *)prop->data;

    /*
     * 0: aesMemoryMap
     * 1: aesDisableKeyMap
     */
    memory_region_init_io(&s->iomems[0], OBJECT(dev), &aes_reg_ops, s,
                          TYPE_APPLE_AES ".mmio", reg[1]);

    sysbus_init_mmio(sbd, &s->iomems[0]);

    memory_region_init_io(&s->iomems[1], OBJECT(dev), &aes_security_reg_ops, s,
                          TYPE_APPLE_AES ".disable_key.mmio", reg[3]);
    sysbus_init_mmio(sbd, &s->iomems[1]);

    s->last_level = 0;
    sysbus_init_irq(sbd, &s->irq);

    QTAILQ_INIT(&s->queue);

    return sbd;
}

static int apple_aes_key_post_load(void *opaque, int version_id)
{
    AESKey *k = (AESKey *)opaque;
    if (k->cipher) {
        qcrypto_cipher_free(k->cipher);
        k->cipher = NULL;
    }
    if (k->select != KEY_SELECT_SOFTWARE) {
        k->disabled = true;
    } else {
        k->disabled = false;
        k->cipher = qcrypto_cipher_new(k->algo, key_mode(k->mode), k->key,
                                       k->len, &error_abort);
    }
    return 0;
}

static int apple_aes_pre_save(void *opaque)
{
    AppleAESState *s = APPLE_AES(opaque);
    if (!s->stopped) {
        aes_stop(s);
        s->stopped = false;
    }
    return 0;
}

static int apple_aes_post_load(void *opaque, int version_id)
{
    AppleAESState *s = APPLE_AES(opaque);
    if (!s->stopped) {
        s->stopped = true;
        aes_start(s);
    }
    return 0;
}

static const VMStateDescription vmstate_apple_aes_command = {
    .name = "apple_aes_command",
    .fields =
        (VMStateField[]){
            VMSTATE_UINT32(command, AESCommand),
            VMSTATE_UINT32(data_len, AESCommand),
            VMSTATE_VARRAY_UINT32(data, AESCommand, data_len, 1,
                                  vmstate_info_uint32, uint32_t),
            VMSTATE_END_OF_LIST(),
        }
};

static const VMStateDescription vmstate_apple_aes_key = {
    .name = "apple_aes_key",
    .post_load = apple_aes_key_post_load,
    .fields =
        (VMStateField[]){
            VMSTATE_UINT32(select, AESKey),
            VMSTATE_UINT32(algo, AESKey),
            VMSTATE_UINT32(len, AESKey),
            VMSTATE_BOOL(wrapped, AESKey),
            VMSTATE_BOOL(encrypt, AESKey),
            VMSTATE_UINT32(func, AESKey),
            VMSTATE_UINT32(mode, AESKey),
            VMSTATE_UINT8(id, AESKey),
            VMSTATE_UINT8_ARRAY(key, AESKey, 32),
            VMSTATE_BOOL(disabled, AESKey),
            VMSTATE_END_OF_LIST(),
        }
};

static const VMStateDescription vmstate_apple_aes = {
    .name = "apple_aes",
    .pre_save = apple_aes_pre_save,
    .post_load = apple_aes_post_load,
    .fields =
        (VMStateField[]){
            VMSTATE_INT32(last_level, AppleAESState),
            VMSTATE_UINT32_ARRAY(reg.raw, AppleAESState,
                                 AES_BLK_REG_SIZE / sizeof(uint32_t)),
            VMSTATE_QTAILQ_V(queue, AppleAESState, 0, vmstate_apple_aes_command,
                             AESCommand, entry),
            VMSTATE_UINT32(command, AppleAESState),
            VMSTATE_UINT32(data_len, AppleAESState),
            VMSTATE_UINT32(data_read, AppleAESState),
            VMSTATE_STRUCT_ARRAY(keys, AppleAESState, 2, 1,
                                 vmstate_apple_aes_key, AESKey),
            VMSTATE_UINT8_2DARRAY(iv, AppleAESState, 4, 16),
            VMSTATE_BOOL(stopped, AppleAESState),
            VMSTATE_END_OF_LIST(),
        }
};

static void apple_aes_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = apple_aes_realize;
    dc->unrealize = apple_aes_unrealize;
    device_class_set_legacy_reset(dc, apple_aes_reset);
    dc->desc = "Apple AES Accelerator";
    dc->vmsd = &vmstate_apple_aes;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo apple_aes_info = {
    .name = TYPE_APPLE_AES,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AppleAESState),
    .class_init = apple_aes_class_init,
};

static void apple_aes_register_types(void)
{
    type_register_static(&apple_aes_info);
}

type_init(apple_aes_register_types);
