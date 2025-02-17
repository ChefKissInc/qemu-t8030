#include "qemu/osdep.h"
#include "hw/arm/apple-silicon/dart.h"
#include "hw/arm/apple-silicon/dtb.h"
#include "hw/irq.h"
#include "hw/qdev-core.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "monitor/hmp-target.h"
#include "monitor/monitor.h"
#include "qapi/qmp/qdict.h"
#include "qemu/bitops.h"
#include "qemu/module.h"
#include "sysemu/dma.h"

// #define DEBUG_DART

#ifdef DEBUG_DART
#define DPRINTF(fmt, ...)                    \
    do {                                     \
        printf("dart: " fmt, ##__VA_ARGS__); \
    } while (0)
#else
#define DPRINTF(fmt, ...) \
    do {                  \
    } while (0)
#endif

#define DART_MAX_INSTANCE 2
#define DART_MAX_STREAMS 16
#define DART_MAX_TTBR 4
#define DART_MAX_VA_BITS 38

#define DART_PARAMS1 (0x0)
#define DART_PARAMS1_PAGE_SHIFT(_x) (((_x) & 0xf) << 24)

#define DART_PARAMS2 (0x4)
#define DART_PARAMS2_BYPASS_SUPPORT (0x1)

#define DART_TLB_OP (0x20)
#define DART_TLB_OP_BUSY (1 << 2)
#define DART_TLB_OP_INVALIDATE (1 << 20)

#define DART_SID_MASK_LOW (0x34)
#define DART_SID_MASK_HIGH (0x38)
#define DART_ERROR_STATUS (0x40)
#define DART_ERROR_STREAM_SHIFT (24)
#define DART_ERROR_STREAM_LENGTH (4)
#define DART_ERROR_STREAM(_x) (((_x) & 0xf) << 24)
#define DART_ERROR_CODE(_x) (((_x) & 0xf) << 0)
#define DART_ERROR_FLAG (1 << 31)
#define DART_ERROR_APF_REJECT (1 << 11)
#define DART_ERROR_UNKNOWN (1 << 9)
#define DART_ERROR_CTRR_WRITE_PROT (1 << 8)
#define DART_ERROR_REGION_PROT (1 << 7)
#define DART_ERROR_AXI_SLV_ERR (1 << 6)
#define DART_ERROR_AXI_SLV_DECODE (1 << 5)
#define DART_ERROR_READ_PROT (1 << 4)
#define DART_ERROR_WRITE_PROT (1 << 3)
#define DART_ERROR_PTE_INVLD (1 << 2)
#define DART_ERROR_L2E_INVLD (1 << 1)
#define DART_ERROR_TTBR_INVLD (1 << 0)
#define DART_ERROR_ADDRESS_LO (0x50)
#define DART_ERROR_ADDRESS_HI (0x54)
#define DART_CONFIG (0x60)
#define DART_CONFIG_LOCK (1 << 15)

#define DART_SID_REMAP(sid4) (0x80 + 4 * (sid4))

#define DART_TCR(sid) (0x100 + 4 * (sid))
#define DART_TCR_TXEN (1 << 7)
#define DART_TCR_BYPASS_DART (1 << 8)
#define DART_TCR_BYPASS_DAPF (1 << 12)

#define DART_TTBR(sid, idx) (0x200 + 16 * (sid) + 4 * (idx))
#define DART_TTBR_VALID (1 << 31)
#define DART_TTBR_SHIFT 12
#define DART_TTBR_MASK 0xFFFFFFF

#define DART_TTE_NO_WRITE (1 << 7)
#define DART_TTE_NO_READ (1 << 8)
#define DART_TTE_AP_MASK (3 << 7)
#define DART_TTE_VALID (1 << 0)
#define DART_TTE_TYPE_TABLE (1 << 0)
#define DART_TTE_TYPE_BLOCK (3 << 0)
#define DART_TTE_TYPE_MASK (0x3)
#define DART_TTE_ADDR_MASK (0xFFFFFFFFFFull)

#define DART_IOTLB_SID_SHIFT (53)
#define DART_IOTLB_SID(_x) (((_x) & 0xfull) << 53)
#define DART_IOTLB_SID_EX(_x) (((_x) >> 53) & 0xf)

typedef enum {
    DART_UNKNOWN = 0,
    DART_DART,
    DART_SMMU,
    DART_DAPF,
} dart_instance_t;

static const char *dart_instance_name[] = {
    [DART_UNKNOWN] = "Unknown",
    [DART_DART] = "DART",
    [DART_SMMU] = "SMMU",
    [DART_DAPF] = "DAPF",
};

typedef struct AppleDARTTLBEntry {
    hwaddr block_addr;
    IOMMUAccessFlags perm;
} AppleDARTTLBEntry;

typedef struct AppleDARTInstance AppleDARTInstance;

typedef struct AppleDARTIOMMUMemoryRegion {
    IOMMUMemoryRegion parent_obj;
    AppleDARTInstance *o;
    uint32_t sid;
} AppleDARTIOMMUMemoryRegion;

struct AppleDARTInstance {
    MemoryRegion iomem;
    AppleDARTIOMMUMemoryRegion *iommus[DART_MAX_STREAMS];
    AppleDARTState *s;
    uint32_t id;
    dart_instance_t type;
#pragma pack(push, 1)
    union {
        uint32_t base_reg[0x4000 / sizeof(uint32_t)];
        struct {
            uint32_t params1;
            uint32_t params2;
            uint32_t unk8[6];
            uint32_t tlb_op;
            uint32_t unk24[4];
            uint64_t sid_mask;
            uint32_t unk3c;
            uint32_t error_status;
            uint32_t unk44[3];
            uint64_t error_address;
            uint32_t unk58[2];
            uint32_t config;
            uint32_t unk64[7];
            uint8_t remap[16];
            uint32_t unk90[28];
            uint32_t tcr[DART_MAX_STREAMS];
            uint32_t unk140[48];
            uint32_t ttbr[DART_MAX_STREAMS][DART_MAX_TTBR];
        };
    };
#pragma pack(pop)

    GHashTable *tlb;
    QemuMutex mutex;
};

struct AppleDARTState {
    SysBusDevice parent_obj;
    char name[0x20];
    qemu_irq irq;
    AppleDARTInstance instances[DART_MAX_INSTANCE];
    uint32_t num_instances;
    uint32_t page_size;
    uint32_t page_shift;
    uint64_t page_mask;
    uint64_t page_bits;
    uint32_t l_mask[3];
    uint32_t l_shift[3];
    uint32_t sids;
    uint32_t bypass;
    uint64_t bypass_address;
};

static int apple_dart_device_list(Object *obj, void *opaque)
{
    GSList **list = opaque;

    if (object_dynamic_cast(obj, TYPE_APPLE_DART)) {
        *list = g_slist_append(*list, DEVICE(obj));
    }

    object_child_foreach(obj, apple_dart_device_list, opaque);
    return 0;
}

static GSList *apple_dart_get_device_list(void)
{
    GSList *list = NULL;

    object_child_foreach(qdev_get_machine(), apple_dart_device_list, &list);
    return list;
}

static gboolean apple_dart_tlb_remove_by_sid_mask(gpointer key, gpointer value,
                                                  gpointer user_data)
{
    hwaddr va = (hwaddr)key;
    uint64_t sid_mask = *(uint64_t *)user_data;

    if ((1ULL << DART_IOTLB_SID_EX(va)) & sid_mask) {
        return true;
    }
    return false;
}

static void apple_dart_update_irq(AppleDARTState *s)
{
    int level = 0;
    for (int i = 0; i < DART_MAX_INSTANCE && !level; i++) {
        AppleDARTInstance *o = &s->instances[i];
        if (o->type == DART_DART) {
            level |= (o->error_status != 0);
        }
    }
#ifndef DEBUG_DART
    qemu_set_irq(s->irq, level);
#endif /* DEBUG_DART */
}

static void base_reg_write(void *opaque, hwaddr addr, uint64_t data,
                           unsigned size)
{
    AppleDARTInstance *o = (AppleDARTInstance *)opaque;
    AppleDARTState *s = o->s;
    uint32_t orig;
    uint32_t val = data;
    bool iflg = 0;
    DPRINTF("%s[%d]: (%s) %s @ 0x" HWADDR_FMT_plx " value: 0x" HWADDR_FMT_plx
            "\n",
            s->name, o->id, dart_instance_name[o->type], __func__, addr, data);

    orig = o->base_reg[addr >> 2];

    if (o->type == DART_DART) {
        switch (addr) {
        case DART_TLB_OP:
            if (val & DART_TLB_OP_INVALIDATE) {
                IOMMUTLBEvent event;
                uint64_t sid_mask = o->sid_mask;
                int i;

                if (qatomic_read(&o->tlb_op) & DART_TLB_OP_BUSY) {
                    return;
                }
                qatomic_or(&o->tlb_op, DART_TLB_OP_BUSY);
                qemu_mutex_lock(&o->mutex);

                for (i = 0; i < DART_MAX_STREAMS; i++) {
                    if ((sid_mask & (1ULL << i)) && o->iommus[i]) {
                        event.type = IOMMU_NOTIFIER_UNMAP;
                        event.entry.target_as = &address_space_memory;
                        event.entry.iova = 0;
                        event.entry.perm = IOMMU_NONE;
                        event.entry.addr_mask = ~(hwaddr)0;

                        memory_region_notify_iommu(
                            IOMMU_MEMORY_REGION(o->iommus[i]), 0, event);
                    }
                }

                g_hash_table_foreach_remove(
                    o->tlb, apple_dart_tlb_remove_by_sid_mask, &sid_mask);
                val &= ~(DART_TLB_OP_INVALIDATE | DART_TLB_OP_BUSY);
                qatomic_and(&o->tlb_op,
                            ~(DART_TLB_OP_INVALIDATE | DART_TLB_OP_BUSY));
                qemu_mutex_unlock(&o->mutex);
                return;
            }
            break;
        case DART_ERROR_STATUS:
            val = orig & (~val);
            iflg = 1;
            break;
        }
    }
    o->base_reg[addr >> 2] = val;
    if (iflg) {
        apple_dart_update_irq(s);
    }
}

static uint64_t base_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    AppleDARTInstance *o = (AppleDARTInstance *)opaque;
    DPRINTF("%s[%d]: (%s) %s @ 0x" HWADDR_FMT_plx "\n", o->s->name, o->id,
            dart_instance_name[o->type], __func__, addr);

    if (o->type == DART_DART) {
        switch (addr) {
        case DART_TLB_OP:
            return qatomic_read(&o->tlb_op);
        default:
            return o->base_reg[addr >> 2];
            break;
        }
    }
    return 0;
}

static const MemoryRegionOps base_reg_ops = {
    .write = base_reg_write,
    .read = base_reg_read,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .valid.unaligned = false,
};

static AppleDARTTLBEntry *apple_dart_ptw(AppleDARTInstance *o, uint32_t sid,
                                         hwaddr iova, uint32_t *error_status)
{
    AppleDARTState *s = o->s;

    uint64_t idx = (iova & (s->l_mask[0])) >> s->l_shift[0];
    uint64_t pte, pa;
    int level;
    AppleDARTTLBEntry *tlb_entry = NULL;
    uint32_t err_status = 0;

    if ((idx >= DART_MAX_TTBR) ||
        ((o->ttbr[sid][idx] & DART_TTBR_VALID) == 0)) {
        err_status = (DART_ERROR_FLAG | DART_ERROR_TTBR_INVLD);
        goto end;
    }

    pte = o->ttbr[sid][idx];
    pa = (pte & DART_TTBR_MASK) << DART_TTBR_SHIFT;

    for (level = 1; level < 3; level++) {
        idx = (iova & (s->l_mask[level])) >> s->l_shift[level];
        pa += 8 * idx;

        if (dma_memory_read(&address_space_memory, pa, &pte, sizeof(pte),
                            MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            err_status = (DART_ERROR_FLAG | DART_ERROR_L2E_INVLD);
            pa = 0;
            pte = 0;
            break;
        }
        DPRINTF("%s: level: %d, pa: 0x" HWADDR_FMT_plx " pte: 0x%llx(0x%llx)\n",
                __func__, level, pa, pte, idx);

        if ((pte & DART_TTE_VALID) == 0) {
            err_status = (DART_ERROR_FLAG | DART_ERROR_PTE_INVLD);
            pa = 0;
            pte = 0;
            break;
        }
        pa = pte & s->page_mask & DART_TTE_ADDR_MASK;
    }

    if ((pte & DART_TTE_VALID)) {
        tlb_entry = g_new0(AppleDARTTLBEntry, 1);
        tlb_entry->block_addr = (pte & s->page_mask & DART_TTE_ADDR_MASK);
        tlb_entry->perm = IOMMU_ACCESS_FLAG(!(pte & DART_TTE_NO_READ),
                                            !(pte & DART_TTE_NO_WRITE));
    } else {
        err_status = (DART_ERROR_FLAG | DART_ERROR_PTE_INVLD);
    }
end:
    if (error_status) {
        *error_status = err_status;
    }
    return tlb_entry;
}

static int apple_dart_attrs_to_index(IOMMUMemoryRegion *iommu, MemTxAttrs attrs)
{
    return 0;
}

static IOMMUTLBEntry apple_dart_translate(IOMMUMemoryRegion *mr, hwaddr addr,
                                          IOMMUAccessFlags flag, int iommu_idx)
{
    AppleDARTIOMMUMemoryRegion *iommu = APPLE_DART_IOMMU_MEMORY_REGION(mr);
    AppleDARTInstance *o = iommu->o;
    AppleDARTState *s = o->s;
    AppleDARTTLBEntry *tlb_entry = NULL;
    uint32_t sid = iommu->sid;
    uint64_t iova, key;

    IOMMUTLBEntry entry = {
        .target_as = &address_space_memory,
        .iova = addr,
        .translated_addr = s->bypass_address + addr,
        .addr_mask = s->page_bits,
        .perm = IOMMU_NONE,
    };

    g_assert_cmpuint(sid, <, DART_MAX_STREAMS);
    qemu_mutex_lock(&o->mutex);
    sid = o->remap[sid] & 0xf;

    if (s->bypass & (1 << sid)) {
        goto end;
    }

    if ((o->tcr[sid] & DART_TCR_TXEN) == 0) {
        /* Disabled translation goto bypass address, not error */
        entry.perm = IOMMU_RW;
        goto end;
    }

    if (o->tcr[sid] & DART_TCR_BYPASS_DART) {
        entry.perm = IOMMU_RW;
        goto end;
    }

    iova = addr >> s->page_shift;
    key = DART_IOTLB_SID(iommu->sid) | iova;

    tlb_entry = g_hash_table_lookup(o->tlb, GUINT_TO_POINTER(key));

    if (tlb_entry == NULL) {
        uint32_t status = 0;
        tlb_entry = apple_dart_ptw(o, sid, iova, &status);
        if (tlb_entry) {
            g_hash_table_insert(o->tlb, GUINT_TO_POINTER(key), tlb_entry);
            DPRINTF("%s[%d]: (%s) SID %u: 0x" HWADDR_FMT_plx
                    " -> 0x" HWADDR_FMT_plx " (%c%c)\n",
                    s->name, o->id, dart_instance_name[o->type], iommu->sid,
                    addr, tlb_entry->block_addr | (addr & s->page_bits),
                    (tlb_entry->perm & IOMMU_RO) ? 'r' : '-',
                    (tlb_entry->perm & IOMMU_WO) ? 'w' : '-');
        } else {
            o->error_status |= status;
            o->error_status =
                deposit32(o->error_status, DART_ERROR_STREAM_SHIFT,
                          DART_ERROR_STREAM_LENGTH, iommu->sid);
            o->error_address = addr;
        }
    }
    if (tlb_entry) {
        entry.translated_addr =
            tlb_entry->block_addr | (addr & entry.addr_mask);
        entry.perm = tlb_entry->perm;
    }

    if ((flag & IOMMU_WO) && !(entry.perm & IOMMU_WO)) {
        o->error_address = addr;
        o->error_status |= (DART_ERROR_FLAG | DART_ERROR_WRITE_PROT);
        o->error_status = deposit32(o->error_status, DART_ERROR_STREAM_SHIFT,
                                    DART_ERROR_STREAM_LENGTH, iommu->sid);
    }

    if ((flag & IOMMU_RO) && !(entry.perm & IOMMU_RO)) {
        o->error_address = addr;
        o->error_status |= (DART_ERROR_FLAG | DART_ERROR_READ_PROT);
        o->error_status = deposit32(o->error_status, DART_ERROR_STREAM_SHIFT,
                                    DART_ERROR_STREAM_LENGTH, iommu->sid);
    }

end:
    DPRINTF("%s[%d]: (%s) SID %u: 0x" HWADDR_FMT_plx " -> 0x" HWADDR_FMT_plx
            " (%c%c)\n",
            s->name, o->id, dart_instance_name[o->type], iommu->sid, entry.iova,
            entry.translated_addr, (entry.perm & IOMMU_RO) ? 'r' : '-',
            (entry.perm & IOMMU_WO) ? 'w' : '-');
    qemu_mutex_unlock(&o->mutex);
    apple_dart_update_irq(s);
    return entry;
}

static void apple_dart_reset(DeviceState *dev)
{
    AppleDARTState *s = APPLE_DART(dev);
    int i, j;

    for (i = 0; i < s->num_instances; i++) {
        memset(s->instances[i].base_reg, 0, sizeof(s->instances[i].base_reg));
        switch (s->instances[i].type) {
        case DART_DART: {
            s->instances[i].params1 = DART_PARAMS1_PAGE_SHIFT(s->page_shift);
            for (j = 0; j < DART_MAX_STREAMS; j++) {
                s->instances[i].remap[j] = j;
            }

            WITH_QEMU_LOCK_GUARD(&s->instances[i].mutex)
            {
                if (s->instances[i].tlb) {
                    g_hash_table_destroy(s->instances[i].tlb);
                    s->instances[i].tlb = NULL;
                }
                s->instances[i].tlb = g_hash_table_new_full(
                    g_direct_hash, g_direct_equal, NULL, g_free);
            }
        }
        default:
            break;
        }
    }
}

IOMMUMemoryRegion *apple_dart_iommu_mr(AppleDARTState *s, uint32_t sid)
{
    int i;

    if (sid >= DART_MAX_STREAMS) {
        return NULL;
    }
    for (i = 0; i < s->num_instances; i++) {
        if (s->instances[i].type == DART_DART) {
            return IOMMU_MEMORY_REGION(s->instances[i].iommus[sid]);
        }
    }
    return NULL;
}

IOMMUMemoryRegion *apple_dart_instance_iommu_mr(AppleDARTState *s,
                                                uint32_t instance, uint32_t sid)
{
    if (sid >= DART_MAX_STREAMS) {
        return NULL;
    }
    if (instance >= s->num_instances) {
        return NULL;
    }
    if (s->instances[instance].type == DART_DART) {
        return IOMMU_MEMORY_REGION(s->instances[instance].iommus[sid]);
    }
    return NULL;
}

AppleDARTState *apple_dart_create(DTBNode *node)
{
    DeviceState *dev;
    AppleDARTState *s;
    SysBusDevice *sbd;
    DTBProp *prop;
    uint64_t *reg;
    uint32_t *instance;
    int i;

    dev = qdev_new(TYPE_APPLE_DART);
    s = APPLE_DART(dev);
    sbd = SYS_BUS_DEVICE(dev);

    prop = dtb_find_prop(node, "name");
    g_strlcpy(s->name, (char *)prop->data, sizeof(s->name));
    dev->id = g_strdup((char *)prop->data);

    prop = dtb_find_prop(node, "page-size");
    if (!prop || prop->length < 4) {
        s->page_shift = 12;
    } else {
        s->page_shift = 31 - clz32(*(uint32_t *)prop->data);
    }
    s->page_size = 1 << s->page_shift;
    s->page_bits = s->page_size - 1;
    s->page_mask = ~s->page_bits;

    switch (s->page_shift) {
    case 12:
        memcpy(s->l_mask, (uint32_t[3]){ 0xc0000, 0x3fe00, 0x1ff }, 12);
        memcpy(s->l_shift, (uint32_t[3]){ 0x12, 9, 0 }, 12);
        break;
    case 14:
        memcpy(s->l_mask, (uint32_t[3]){ 0xc00000, 0x3ff800, 0x7ff }, 12);
        memcpy(s->l_shift, (uint32_t[3]){ 0x16, 11, 0 }, 12);
        break;
    default:
        g_assert_not_reached();
    }

    prop = dtb_find_prop(node, "sids");
    if (prop && prop->length >= 4) {
        s->sids = *(uint32_t *)prop->data;
    }

    prop = dtb_find_prop(node, "bypass");
    if (prop && prop->length >= 4) {
        s->bypass = *(uint32_t *)prop->data;
    }

    prop = dtb_find_prop(node, "bypass-address");
    if (prop && prop->length >= 8) {
        s->bypass_address = *(uint64_t *)prop->data;
    }

    prop = dtb_find_prop(node, "instance");
    g_assert_nonnull(prop);
    g_assert_cmpuint((prop->length / 12) * 12, ==, prop->length);
    instance = (uint32_t *)prop->data;

    prop = dtb_find_prop(node, "reg");
    g_assert_nonnull(prop);

    reg = (uint64_t *)prop->data;


    for (i = 0; i < prop->length / 16; i++) {
        AppleDARTInstance *o = &s->instances[i];
        s->num_instances++;
        o->id = i;
        o->s = s;
        memory_region_init_io(&o->iomem, OBJECT(dev), &base_reg_ops, o,
                              TYPE_APPLE_DART ".reg", reg[i * 2 + 1]);
        sysbus_init_mmio(sbd, &o->iomem);

        switch (*instance) {
        case 'DART': {
            o->type = DART_DART;

            for (int j = 0; j < DART_MAX_STREAMS; j++) {
                if ((1 << j) & s->sids) {
                    g_autofree char *name =
                        g_strdup_printf("%s-%d-%d", s->name, o->id, j);
                    o->iommus[j] = g_new0(AppleDARTIOMMUMemoryRegion, 1);
                    o->iommus[j]->sid = j;
                    o->iommus[j]->o = o;
                    memory_region_init_iommu(
                        o->iommus[j], sizeof(AppleDARTIOMMUMemoryRegion),
                        TYPE_APPLE_DART_IOMMU_MEMORY_REGION, OBJECT(s), name,
                        1ULL << DART_MAX_VA_BITS);

                    qemu_mutex_init(&o->mutex);
                }
            }
            break;
        }
        case 'SMMU':
            o->type = DART_SMMU;
            break;
        case 'DAPF':
            o->type = DART_DAPF;
            break;
        default:
            o->type = DART_UNKNOWN;
            break;
        }
        DPRINTF("%s: DART %s instance %d: %s\n", __func__, s->name, i,
                dart_instance_name[o->type]);
        instance += 3;
    }

    sysbus_init_irq(sbd, &s->irq);

    return s;
}

static void apple_dart_dump_pt(Monitor *mon, AppleDARTInstance *o, hwaddr iova,
                               uint64_t *entries, int level, uint64_t pte)
{
    AppleDARTState *s = o->s;
    if (level == 3) {
        monitor_printf(mon,
                       "\t\t\t0x" HWADDR_FMT_plx " ... 0x" HWADDR_FMT_plx
                       " -> 0x%llx %c%c\n",
                       iova << s->page_shift, (iova + 1) << s->page_shift,
                       pte & s->page_mask & DART_TTE_ADDR_MASK,
                       pte & DART_TTE_NO_READ ? '-' : 'r',
                       pte & DART_TTE_NO_WRITE ? '-' : 'w');
        return;
    }

    for (uint64_t i = 0; i <= (s->l_mask[level] >> s->l_shift[level]); i++) {
        uint64_t pte2 = entries[i];

        if ((pte2 & DART_TTE_VALID) ||
            ((level == 0) && (pte2 & DART_TTBR_VALID))) {
            uint64_t pa = pte2 & s->page_mask & DART_TTE_ADDR_MASK;
            if (level == 0) {
                pa = (pte2 & DART_TTBR_MASK) << DART_TTBR_SHIFT;
            }
            uint64_t next_n_entries = 0;
            if (level < 2) {
                next_n_entries =
                    (s->l_mask[level + 1] >> s->l_shift[level + 1]) + 1;
            }
            g_autofree uint64_t *next = g_malloc0(8 * next_n_entries);
            if (dma_memory_read(&address_space_memory, pa, next,
                                8 * next_n_entries,
                                MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
                continue;
            }

            apple_dart_dump_pt(mon, o, iova | (i << s->l_shift[level]), next,
                               level + 1, pte2);
        }
    }
}

void hmp_info_dart(Monitor *mon, const QDict *qdict)
{
    const char *name = qdict_get_try_str(qdict, "name");
    g_autoptr(GSList) device_list = apple_dart_get_device_list();
    AppleDARTState *dart = NULL;

    if (!name) {
        for (GSList *ele = device_list; ele; ele = ele->next) {
            DeviceState *dev = ele->data;
            dart = APPLE_DART(dev);
            monitor_printf(mon, "%s\tPage size: %d\t%d Instances\n", dev->id,
                           dart->page_size, dart->num_instances);
        }
        return;
    } else {
        for (GSList *ele = device_list; ele; ele = ele->next) {
            DeviceState *dev = ele->data;
            if (!strcmp(dev->id, name)) {
                dart = APPLE_DART(dev);
                break;
            }
        }
    }

    if (!dart) {
        monitor_printf(mon, "Cannot find dart %s\n", name);
        return;
    }

    for (int i = 0; i < dart->num_instances; i++) {
        AppleDARTInstance *o = &dart->instances[i];
        monitor_printf(mon, "\tInstance %d: type: %s\n", i,
                       dart_instance_name[o->type]);
        if (o->type != DART_DART) {
            continue;
        }

        for (int sid = 0; sid < DART_MAX_STREAMS; sid++) {
            if (dart->sids & (1 << sid)) {
                uint32_t remap = o->remap[sid] & 0xf;
                if (sid != remap) {
                    monitor_printf(mon, "\t\tSID %d: Remapped to %d\n", sid,
                                   remap);
                    continue;
                }
                if ((o->tcr[sid] & DART_TCR_TXEN) == 0) {
                    monitor_printf(mon, "\t\tSID %d: Translation disabled\n",
                                   sid);
                    continue;
                }

                if (o->tcr[sid] & DART_TCR_BYPASS_DART) {
                    monitor_printf(mon, "\t\tSID %d: Translation bypassed\n",
                                   sid);
                    continue;
                }
                monitor_printf(mon, "\t\tSID %d:\n", sid);
                uint64_t l0_entries[4] = { o->ttbr[sid][0], o->ttbr[sid][1],
                                           o->ttbr[sid][2], o->ttbr[sid][3] };
                apple_dart_dump_pt(mon, o, 0, l0_entries, 0, 0);
            }
        }
    }
}

static const VMStateDescription vmstate_apple_dart_instance = {
    .name = "apple_dart_instance",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields =
        (VMStateField[]){
            VMSTATE_UINT32_ARRAY(base_reg, AppleDARTInstance,
                                 0x4000 / sizeof(uint32_t)),
            VMSTATE_END_OF_LIST(),
        }
};

static const VMStateDescription vmstate_apple_dart = {
    .name = "apple_dart",
    .version_id = 1,
    .minimum_version_id = 1,
    .priority = MIG_PRI_IOMMU,
    .fields =
        (VMStateField[]){
            VMSTATE_STRUCT_ARRAY(instances, AppleDARTState, DART_MAX_INSTANCE,
                                 1, vmstate_apple_dart_instance,
                                 AppleDARTInstance),

            VMSTATE_END_OF_LIST(),
        }
};

static void apple_dart_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, apple_dart_reset);
    dc->desc = "Apple DART IOMMU";
    dc->vmsd = &vmstate_apple_dart;
}

static void apple_dart_iommu_memory_region_class_init(ObjectClass *klass,
                                                      void *data)
{
    IOMMUMemoryRegionClass *imrc = IOMMU_MEMORY_REGION_CLASS(klass);

    imrc->translate = apple_dart_translate;
    imrc->attrs_to_index = apple_dart_attrs_to_index;
}

static const TypeInfo apple_dart_info = {
    .name = TYPE_APPLE_DART,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AppleDARTState),
    .class_init = apple_dart_class_init,
};

static const TypeInfo apple_dart_iommu_memory_region_info = {
    .parent = TYPE_IOMMU_MEMORY_REGION,
    .name = TYPE_APPLE_DART_IOMMU_MEMORY_REGION,
    .class_init = apple_dart_iommu_memory_region_class_init,
};

static void apple_dart_register_types(void)
{
    type_register_static(&apple_dart_info);
    type_register_static(&apple_dart_iommu_memory_region_info);
}

type_init(apple_dart_register_types);
