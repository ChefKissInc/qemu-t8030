#ifndef APPLE_NVRAM_H
#define APPLE_NVRAM_H

#include "qemu/osdep.h"
#include "block/block.h"
#include "block/nvme.h"
#include "hw/block/block.h"
#include "hw/nvme/nvme.h"
#include "hw/nvram/chrp_nvram.h"
#include "qemu/queue.h"
#include "qom/object.h"

#define TYPE_APPLE_NVRAM "apple-nvram"
OBJECT_DECLARE_TYPE(AppleNvramState, AppleNvramClass, APPLE_NVRAM)

#pragma pack(push, 1)
typedef struct {
    ChrpNvramPartHdr chrp;
    uint32_t adler;
    uint32_t generation;
    uint8_t padding[8];
} AppleNvramPartHdr;
#pragma pack(pop)

#define APPLE_NVRAM_PANIC_NAME "APL,OSXPanic"
#define APPLE_NVRAM_PANIC_NAME_TRUNCATED "APL,OSXPani"

typedef struct env_var {
    QTAILQ_ENTRY(env_var) entry;

    char name[64];
    char *str;
    size_t u;
    uint32_t flags;
} env_var;

typedef struct NvramPartition {
    QTAILQ_ENTRY(NvramPartition) entry;

    uint8_t sig;
    size_t len;
    uint8_t *data;
    char name[16];
} NvramPartition;

typedef struct NvramBank {
    QTAILQ_HEAD(, NvramPartition) parts;

    size_t len;
} NvramBank;
void nvram_free(NvramBank *bank);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(NvramBank, nvram_free);

typedef struct AppleNvramState {
    NvmeNamespace parent_obj;

    NvramBank *bank;
    QTAILQ_HEAD(, env_var) env;
    uint32_t len;
} AppleNvramState;

struct AppleNvramClass {
    /*< private >*/
    DeviceClass base_class;

    /*< public >*/
    DeviceRealize parent_realize;
    DeviceUnrealize parent_unrealize;
    DeviceReset parent_reset;
};

NvramPartition *nvram_find_part(NvramBank *bank, const char *name);
NvramBank *nvram_parse(void *buf, size_t len);
void apple_nvram_load(AppleNvramState *s);
void apple_nvram_save(AppleNvramState *s);
ssize_t apple_nvram_serialize(AppleNvramState *s, void *buffer, size_t size);

const char *env_get(AppleNvramState *s, const char *name);
size_t env_get_uint(AppleNvramState *s, const char *name, size_t default_val);
bool env_get_bool(AppleNvramState *s, const char *name, bool default_val);
int env_unset(AppleNvramState *s, const char *name);
int env_set(AppleNvramState *s, const char *name, const char *val,
            uint32_t flags);
int env_set_uint(AppleNvramState *s, const char *name, size_t val,
                 uint32_t flags);
int env_set_bool(AppleNvramState *s, const char *name, bool val,
                 uint32_t flags);
#endif /* APPLE_NVRAM_H */
