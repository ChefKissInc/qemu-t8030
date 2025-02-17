#include "qemu/osdep.h"
#include "hw/nvram/apple_nvram.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "sysemu/block-backend.h"
#include <zlib.h>

static inline uint8_t chrp_checksum(ChrpNvramPartHdr *header)
{
    unsigned int i, sum;
    uint8_t *tmpptr;

    /* Checksum */
    tmpptr = (uint8_t *)header;
    sum = *tmpptr;
    for (i = 0; i < 14; i++) {
        sum += tmpptr[2 + i];
        sum = (sum + ((sum & 0xff00) >> 8)) & 0xff;
    }
    return sum & 0xff;
}

static env_var *find_env(AppleNvramState *s, const char *name)
{
    env_var *v;
    QTAILQ_FOREACH (v, &s->env, entry) {
        if (!strcmp(v->name, name)) {
            return v;
        }
    }
    return NULL;
}

const char *env_get(AppleNvramState *s, const char *name)
{
    env_var *v;

    v = find_env(s, name);

    if (v) {
        return v->str;
    }
    return NULL;
}

size_t env_get_uint(AppleNvramState *s, const char *name, size_t default_val)
{
    env_var *v;

    v = find_env(s, name);

    if (v) {
        return v->u;
    }
    return default_val;
}

bool env_get_bool(AppleNvramState *s, const char *name, bool default_val)
{
    env_var *v;

    v = find_env(s, name);

    if (!v) {
        return default_val;
    }

    if (!strcmp(v->str, "true")) {
        return true;
    }
    if (v->u) {
        return true;
    }
    return false;
}

int env_unset(AppleNvramState *s, const char *name)
{
    env_var *v;

    v = find_env(s, name);

    if (!v) {
        return 0;
    }

    QTAILQ_REMOVE(&s->env, v, entry);

    g_free(v->str);
    g_free(v);

    return 1;
}

int env_set(AppleNvramState *s, const char *name, const char *val,
            uint32_t flags)
{
    g_autofree env_var *v;

    v = find_env(s, name);

    if (v) {
        env_unset(s, name);
        v = NULL;
    }

    v = g_malloc0(sizeof(env_var));

    if (!v) {
        return -1;
    }

    g_strlcpy(v->name, name, sizeof(v->name));
    v->str = g_strdup(val);

    if (v->str == NULL) {
        return -1;
    }

    v->u = strtoul(v->str, NULL, 0);
    v->flags = flags;

    QTAILQ_INSERT_TAIL(&s->env, v, entry);

    g_steal_pointer(&v);
    return 0;
}

int env_set_uint(AppleNvramState *s, const char *name, size_t val,
                 uint32_t flags)
{
    g_autofree char *buf = NULL;

    if (asprintf(&buf, "0x%lx", val) < 0) {
        return -1;
    }
    return env_set(s, name, buf, flags);
}

int env_set_bool(AppleNvramState *s, const char *name, bool val, uint32_t flags)
{
    return env_set(s, name, val ? "true" : "false", flags);
}

static ssize_t env_serialize(AppleNvramState *s, uint8_t *buffer, size_t len)
{
    env_var *v;
    size_t pos = 0;
    g_autofree char *buf = g_malloc0(len);

    if (buf == NULL) {
        return -1;
    }

    QTAILQ_FOREACH (v, &s->env, entry) {
        snprintf(buf + pos, len - pos, "%s=%s", v->name, v->str);
        pos += strlen(buf + pos) + 1;

        if (pos >= len) {
            return -1;
        }
    }
    memcpy(buffer, buf, len);
    return pos;
}

NvramPartition *nvram_find_part(NvramBank *bank, const char *name)
{
    NvramPartition *part;

    QTAILQ_FOREACH (part, &bank->parts, entry) {
        if (!strcmp(part->name, name)) {
            return part;
        }
    }
    return NULL;
}

static void nvram_parse_partitions(NvramBank *bank, void *buf)
{
    ChrpNvramPartHdr *hdr = NULL;
    NvramPartition *part = NULL;
    off_t offset;

    offset = 0x20;

    while (offset + sizeof(ChrpNvramPartHdr) <= bank->len) {
        hdr = (ChrpNvramPartHdr *)(buf + offset);

        if (hdr->checksum != chrp_checksum(hdr)) {
            error_report("bank partition checksum failed");
            return;
        }

        if (hdr->signature == CHRP_NVPART_FREE) {
            break;
        }

        if (((hdr->len * 0x10) + offset > bank->len) || (hdr->len < 1)) {
            error_report("bank partition len out of range");
            return;
        }

        if (!memcmp(hdr->name, APPLE_NVRAM_PANIC_NAME_TRUNCATED, 12)) {
            /* Skip this partition */
            offset += hdr->len * 0x10;
            continue;
        }

        part = g_malloc0(sizeof(NvramPartition));
        part->sig = hdr->signature;
        part->len = (hdr->len * 0x10) - 0x10;
        strncpy(part->name, hdr->name, sizeof(part->name));
        part->data = g_malloc0(part->len);
        memcpy(part->data, buf + offset + 0x10, part->len);

        QTAILQ_INSERT_TAIL(&bank->parts, part, entry);

        offset += hdr->len * 0x10;
    }
}

NvramBank *nvram_parse(void *buf, size_t len)
{
    AppleNvramPartHdr *hdr = buf;
    NvramBank *bank = NULL;

    bank = g_malloc0(sizeof(NvramBank));
    QTAILQ_INIT(&bank->parts);
    bank->len = len;

    if (hdr->chrp.checksum != chrp_checksum(&hdr->chrp)) {
        error_report(
            "nvram partition failed checksum: expected: 0x%x, got 0x%x",
            hdr->chrp.checksum, chrp_checksum(&hdr->chrp));
        return bank;
    }

    uint32_t adler = adler32(1, buf + 0x14, len - 0x14);

    if (adler != hdr->adler) {
        error_report("nvram bank fails adler32: expected: 0x%x, got 0x%x",
                     hdr->adler, adler);
        return bank;
    }
    nvram_parse_partitions(bank, buf);

    return bank;
}

static int nvram_prepare_bank(NvramBank *bank, void **buffer, size_t *len)
{
    g_autofree void *buf = NULL;
    off_t offset = 0;
    AppleNvramPartHdr *apple_hdr = NULL;
    ChrpNvramPartHdr *hdr = NULL;
    NvramPartition *part = NULL;

    buf = g_malloc0(bank->len);

    apple_hdr = (AppleNvramPartHdr *)buf;
    apple_hdr->chrp.signature = 0x5a;
    apple_hdr->chrp.len = 0x2;
    memcpy(apple_hdr->chrp.name, "nvram", sizeof("nvram"));
    apple_hdr->chrp.checksum = chrp_checksum(&apple_hdr->chrp);
    apple_hdr->generation = 0;

    offset = 0x20;
    QTAILQ_FOREACH (part, &bank->parts, entry) {
        if (part->len == 0) {
            error_report("%s: empty partition", __func__);
            continue;
        }

        if (offset + sizeof(ChrpNvramPartHdr) > bank->len) {
            error_report("%s: not enough space", __func__);
            return -1;
        }

        hdr = (ChrpNvramPartHdr *)(buf + offset);
        hdr->signature = part->sig;
        hdr->len = ROUND_UP(part->len + 0x10, 0x10) / 0x10;
        memcpy(hdr->name, part->name, sizeof(hdr->name));
        hdr->checksum = chrp_checksum(hdr);

        if (offset + sizeof(ChrpNvramPartHdr) + part->len > bank->len) {
            error_report("%s: not enough space", __func__);
            return -1;
        }
        memcpy(hdr + 1, part->data, part->len);
        offset += hdr->len * 0x10;
    }

    if (offset + sizeof(ChrpNvramPartHdr) <= bank->len) {
        hdr = (ChrpNvramPartHdr *)(buf + offset);
        hdr->signature = CHRP_NVPART_FREE;
        hdr->len = (bank->len - offset) / 0x10;
        hdr->checksum = chrp_checksum(hdr);
    }

    apple_hdr->adler = adler32(1, buf + 0x14, bank->len - 0x14);

    *buffer = g_steal_pointer(&buf);
    *len = bank->len;
    return 0;
}

void nvram_free(NvramBank *bank)
{
    NvramPartition *p1 = QTAILQ_FIRST(&bank->parts);

    while (p1) {
        NvramPartition *p2 = QTAILQ_NEXT(p1, entry);
        g_free(p1->data);
        g_free(p1);
        p1 = p2;
    }

    g_free(bank);
}

static void apple_nvram_load_env(AppleNvramState *s)
{
    NvramPartition *part = nvram_find_part(s->bank, "common");
    uint32_t cnt = 0;
    char *name = NULL;
    uint32_t name_len = 0;
    char *data = NULL;
    uint32_t data_len = 0;

    while (cnt < part->len) {
        if (part->data[cnt] == '\0') {
            break;
        }

        name = (char *)part->data + cnt;
        for (name_len = 0; (cnt + name_len) < part->len; name_len++) {
            if (name[name_len] == '=') {
                break;
            }
        }

        if (cnt + name_len >= part->len) {
            break;
        }

        cnt += name_len + 1;

        data = (char *)part->data + cnt;
        for (data_len = 0; (cnt + data_len) < part->len; data_len++) {
            if (data[data_len] == '\0') {
                break;
            }
        }

        if (cnt + data_len >= part->len) {
            break;
        }
        cnt += data_len + 1;

        name = g_strndup(name, name_len);
        data = g_strndup(data, data_len);

        env_set(s, name, data, 0);
        g_free(name);
        g_free(data);
    }
}

ssize_t apple_nvram_serialize(AppleNvramState *s, void *buffer, size_t size)
{
    NvramPartition *p = nvram_find_part(s->bank, "common");
    g_autofree void *buf = NULL;
    size_t len = 0;

    if (!p) {
        p = g_malloc0(sizeof(NvramPartition));
        p->sig = 0x70;
        g_strlcpy(p->name, "common", sizeof(p->name));
        p->len = 0x7f0;
        p->data = g_malloc0(p->len);
        QTAILQ_INSERT_HEAD(&s->bank->parts, p, entry);
    }

    if (env_serialize(s, p->data, p->len) < 0) {
        error_report("%s: failed to serialize env", __func__);
    }

    if (nvram_prepare_bank(s->bank, &buf, &len) < 0) {
        error_report("%s: failed to prepare bank", __func__);
        return -1;
    }

    if (size < len) {
        len = size;
    }
    memcpy(buffer, buf, len);
    return len;
}

static void apple_nvram_cleanup(AppleNvramState *s)
{
    env_var *v = QTAILQ_FIRST(&s->env);
    if (s->bank) {
        nvram_free(s->bank);
        s->bank = NULL;
    }

    while (v != NULL) {
        env_var *next = QTAILQ_NEXT(v, entry);
        g_free(v->str);
        g_free(v);
        v = next;
    }
    QTAILQ_INIT(&s->env);
}

void apple_nvram_save(AppleNvramState *s)
{
    NvmeNamespace *ns = NVME_NS(s);
    g_autofree void *buf = g_malloc0(s->len);
    ssize_t len = apple_nvram_serialize(s, buf, s->len);

    if (len < 0) {
        error_report("%s: Failed to serialize NVRAM", __func__);
        return;
    }

    if (blk_pwrite(ns->blkconf.blk, 0, len, buf, 0) < 0) {
        error_report("%s: Failed to write NVRAM", __func__);
        return;
    }
}

void apple_nvram_load(AppleNvramState *s)
{
    NvmeNamespace *ns = NVME_NS(s);
    g_autofree void *buffer = NULL;
    size_t len = blk_getlength(ns->blkconf.blk);

    if (len > 0x2000) {
        len = 0x2000;
    }

    buffer = g_malloc0(len);

    blk_flush(ns->blkconf.blk);
    blk_drain(ns->blkconf.blk);

    if (blk_pread(ns->blkconf.blk, 0, len, buffer, 0) < 0) {
        error_report("%s: Failed to read NVRAM", __func__);
        return;
    }

    apple_nvram_cleanup(s);
    s->len = len;
    s->bank = nvram_parse(buffer, len);
    QTAILQ_INIT(&s->env);

    if (nvram_find_part(s->bank, "common") == NULL) {
        NvramPartition *part = g_malloc0(sizeof(NvramPartition));
        part->sig = 0x70;
        g_strlcpy(part->name, "common", sizeof(part->name));
        part->len = 0x7f0;
        part->data = g_malloc0(part->len);
        QTAILQ_INSERT_HEAD(&s->bank->parts, part, entry);
    }
    apple_nvram_load_env(s);
}

static void apple_nvram_realize(DeviceState *dev, Error **errp)
{
    AppleNvramState *s = APPLE_NVRAM(dev);
    AppleNvramClass *anc = APPLE_NVRAM_GET_CLASS(dev);
    Error *local_err = NULL;

    anc->parent_realize(dev, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }
    apple_nvram_load(s);
}

static void apple_nvram_unrealize(DeviceState *dev)
{
    AppleNvramState *s = APPLE_NVRAM(dev);
    AppleNvramClass *anc = APPLE_NVRAM_GET_CLASS(dev);

    anc->parent_unrealize(dev);

    apple_nvram_cleanup(s);
}

static void apple_nvram_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    AppleNvramClass *anc = APPLE_NVRAM_CLASS(klass);

    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);

    device_class_set_parent_realize(dc, apple_nvram_realize,
                                    &anc->parent_realize);
    device_class_set_parent_unrealize(dc, apple_nvram_unrealize,
                                      &anc->parent_unrealize);
    dc->desc = "Apple NVRAM";
}

static void apple_nvram_instance_init(Object *obj)
{
}

static const TypeInfo apple_nvram_info = {
    .name = TYPE_APPLE_NVRAM,
    .parent = TYPE_NVME_NS,
    .class_size = sizeof(AppleNvramClass),
    .class_init = apple_nvram_class_init,
    .instance_size = sizeof(AppleNvramState),
    .instance_init = apple_nvram_instance_init,
};

static void apple_nvram_register_types(void)
{
    type_register_static(&apple_nvram_info);
}

type_init(apple_nvram_register_types)
