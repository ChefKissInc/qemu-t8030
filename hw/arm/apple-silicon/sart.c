#include "qemu/osdep.h"
#include "exec/address-spaces.h"
#include "hw/arm/apple-silicon/dtb.h"
#include "hw/arm/apple-silicon/sart.h"
#include "qemu/module.h"

// #define DEBUG_SART

#ifdef DEBUG_SART
#define DPRINTF(fmt, ...)                    \
    do {                                     \
        printf("sart: " fmt, ##__VA_ARGS__); \
    } while (0)
#else
#define DPRINTF(fmt, ...) \
    do {                  \
    } while (0)
#endif

#define SART_MAX_VA_BITS 42
#define SART_NUM_REGIONS 16

typedef struct AppleSARTIOMMUMemoryRegion {
    IOMMUMemoryRegion parent_obj;
    AppleSARTState *s;
} AppleSARTIOMMUMemoryRegion;

typedef struct AppleSARTRegion {
    uint64_t addr;
    uint64_t size;
    uint32_t flags;
} AppleSARTRegion;

struct AppleSARTState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    AppleSARTIOMMUMemoryRegion iommu;
    AppleSARTRegion regions[SART_NUM_REGIONS];
    uint32_t version;
    uint32_t reg[0x8000 / sizeof(uint32_t)];
};

static inline uint32_t sart_get_reg(AppleSARTState *s, uint32_t offset)
{
    g_assert_cmphex(offset + sizeof(uint32_t), <=, sizeof(s->reg));
    g_assert_cmphex(offset % sizeof(uint32_t), ==, 0);
    return s->reg[offset / sizeof(uint32_t)];
}

static inline hwaddr sart_get_region_addr(AppleSARTState *s, int region)
{
    g_assert_cmpuint(region, <, SART_NUM_REGIONS);

    switch (s->version) {
    case 1:
    case 2:
        return (sart_get_reg(s, 0x40 + region * 4) >> 0) & 0xFFFFFF;
    case 3:
        return (sart_get_reg(s, 0x40 + region * 4) >> 0) & 0x3FFFFFFF;
    default:
        g_assert_not_reached();
        break;
    }
}

static inline uint64_t sart_get_region_size(AppleSARTState *s, int region)
{
    g_assert_cmpuint(region, <, SART_NUM_REGIONS);

    switch (s->version) {
    case 1:
        return (sart_get_reg(s, 0x0 + region * 4) >> 0) & 0x7FFFF;
    case 2:
        return (sart_get_reg(s, 0x0 + region * 4) >> 0) & 0xFFFFFF;
    case 3:
        return (sart_get_reg(s, 0x80 + region * 4) >> 0) & 0x3FFFFFFF;
    default:
        g_assert_not_reached();
        break;
    }
}

static inline uint32_t sart_get_region_flags(AppleSARTState *s, int region)
{
    g_assert_cmpuint(region, <, SART_NUM_REGIONS);

    switch (s->version) {
    case 1:
        return sart_get_reg(s, 0x0 + region * 4) & ~(0x7FFFFULL << 12);
    case 2:
        return sart_get_reg(s, 0x0 + region * 4) & ~(0xFFFFFFULL << 12);
    case 3:
        return sart_get_reg(s, 0x0 + region * 4);
    default:
        g_assert_not_reached();
        break;
    }
}

static void base_reg_write(void *opaque, hwaddr addr, uint64_t data,
                           unsigned size)
{
    AppleSARTState *s = APPLE_SART(opaque);
    uint32_t orig;
    uint32_t val = data;
    DPRINTF("%s: %s @ 0x" HWADDR_FMT_plx " value: 0x" HWADDR_FMT_plx "\n",
            DEVICE(s)->id, __func__, addr, data);

    orig = s->reg[addr >> 2];

    s->reg[addr >> 2] = val;

    for (int i = 0; i < SART_NUM_REGIONS; i++) {
        if ((sart_get_region_addr(s, i) != s->regions[i].addr) ||
            (sart_get_region_size(s, i) != s->regions[i].size) ||
            (sart_get_region_flags(s, i) != s->regions[i].flags)) {
            hwaddr curr = s->regions[i].addr;
            for (curr = s->regions[i].addr;
                 curr < s->regions[i].addr + s->regions[i].size; curr++) {
                IOMMUTLBEvent event;
                event.type = IOMMU_NOTIFIER_UNMAP;
                event.entry.target_as = &address_space_memory;
                event.entry.iova = curr << 12;
                event.entry.perm = IOMMU_NONE;
                event.entry.addr_mask = 0xFFF;
                memory_region_notify_iommu(IOMMU_MEMORY_REGION(&s->iommu), 0,
                                           event);
            }
            s->regions[i].addr = sart_get_region_addr(s, i);
            s->regions[i].size = sart_get_region_size(s, i);
            s->regions[i].flags = sart_get_region_flags(s, i);
        }
    }
}

static uint64_t base_reg_read(void *opaque, hwaddr addr, unsigned size)
{
    AppleSARTState *s = APPLE_SART(opaque);
    DPRINTF("%s: %s @ 0x" HWADDR_FMT_plx "\n", DEVICE(s)->id, __func__, addr);

    return s->reg[addr >> 2];
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

static IOMMUTLBEntry apple_sart_translate(IOMMUMemoryRegion *mr, hwaddr addr,
                                          IOMMUAccessFlags flag, int iommu_idx)
{
    AppleSARTIOMMUMemoryRegion *iommu = APPLE_SART_IOMMU_MEMORY_REGION(mr);
    AppleSARTState *s = container_of(iommu, AppleSARTState, iommu);

    IOMMUTLBEntry entry = {
        .target_as = &address_space_memory,
        .iova = addr & ~0xFFF,
        .translated_addr = addr & ~0xFFF,
        .addr_mask = 0xFFF,
        .perm = IOMMU_RW,
    };
    addr >>= 12;

    for (int i = 0; i < SART_NUM_REGIONS; i++) {
        if ((s->regions[i].addr <= addr) &&
            (addr < s->regions[i].addr + s->regions[i].size) &&
            s->regions[i].flags) {
            entry.perm = IOMMU_RW;
            break;
        }
    }
    return entry;
}

static void apple_sart_reset(DeviceState *dev)
{
    AppleSARTState *s = APPLE_SART(dev);
    memset(s->reg, 0, sizeof(s->reg));
    memset(s->regions, 0, sizeof(s->regions));
}

SysBusDevice *apple_sart_create(DTBNode *node)
{
    DeviceState *dev;
    AppleSARTState *s;
    SysBusDevice *sbd;
    DTBProp *prop;
    uint64_t *reg;

    dev = qdev_new(TYPE_APPLE_SART);
    s = APPLE_SART(dev);
    sbd = SYS_BUS_DEVICE(dev);

    prop = dtb_find_prop(node, "name");
    dev->id = g_strdup((const char *)prop->data);

    prop = dtb_find_prop(node, "sart-version");
    if (prop == NULL) {
        // iOS 13?
        s->version = 1;
    } else {
        g_assert_nonnull(prop);
        s->version = *(uint32_t *)prop->data;
    }
    g_assert_cmpuint(s->version, >=, 1);
    g_assert_cmpuint(s->version, <=, 3);

    prop = dtb_find_prop(node, "reg");
    g_assert_nonnull(prop);

    reg = (uint64_t *)prop->data;
    memory_region_init_io(&s->iomem, OBJECT(dev), &base_reg_ops, s,
                          TYPE_APPLE_SART ".reg", reg[1]);
    sysbus_init_mmio(sbd, &s->iomem);
    memory_region_init_iommu(&s->iommu, sizeof(AppleSARTIOMMUMemoryRegion),
                             TYPE_APPLE_SART_IOMMU_MEMORY_REGION, OBJECT(s),
                             dev->id, 1ULL << SART_MAX_VA_BITS);

    sysbus_init_mmio(sbd, MEMORY_REGION(&s->iommu));

    return sbd;
}

static void apple_sart_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, apple_sart_reset);
    dc->desc = "Apple SART IOMMU";
}

static void apple_sart_iommu_memory_region_class_init(ObjectClass *klass,
                                                      void *data)
{
    IOMMUMemoryRegionClass *imrc = IOMMU_MEMORY_REGION_CLASS(klass);

    imrc->translate = apple_sart_translate;
}

static const TypeInfo apple_sart_info = {
    .name = TYPE_APPLE_SART,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AppleSARTState),
    .class_init = apple_sart_class_init,
};

static const TypeInfo apple_sart_iommu_memory_region_info = {
    .parent = TYPE_IOMMU_MEMORY_REGION,
    .name = TYPE_APPLE_SART_IOMMU_MEMORY_REGION,
    .class_init = apple_sart_iommu_memory_region_class_init,
};

static void apple_sart_register_types(void)
{
    type_register_static(&apple_sart_info);
    type_register_static(&apple_sart_iommu_memory_region_info);
}

type_init(apple_sart_register_types);
