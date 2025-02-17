#ifndef HW_ARM_XNU_PF_H
#define HW_ARM_XNU_PF_H
#include "hw/arm/apple-silicon/boot.h"

typedef struct {
    uint64_t va;
    uint64_t size;
    uint8_t *cacheable_base;
} ApplePfRange;

typedef struct ApplePfPatch ApplePfPatch;

struct ApplePfPatch {
    bool (*pf_callback)(ApplePfPatch *patch, void *cacheable_stream);
    bool is_required;
    bool has_fired;
    bool should_match;
    void (*pf_match)(ApplePfPatch *patch, uint8_t access_type, void *preread,
                     void *cacheable_stream);
    ApplePfPatch *next_patch;
    uint8_t pf_data[0];
    const char *name;
};

typedef bool (*xnu_pf_patch_callback)(ApplePfPatch *patch,
                                      void *cacheable_stream);

typedef struct {
    ApplePfPatch *patch_head;
    uint64_t p0;
    uint8_t accesstype;
    bool is_required;
} ApplePfPatchset;

#define XNU_PF_ACCESS_8BIT (0x8)
#define XNU_PF_ACCESS_16BIT (0x10)
#define XNU_PF_ACCESS_32BIT (0x20)
#define XNU_PF_ACCESS_64BIT (0x40)

ApplePfRange *xnu_pf_range_from_va(uint64_t va, uint64_t size);

ApplePfRange *xnu_pf_segment(MachoHeader64 *header, const char *segment_name);

ApplePfRange *xnu_pf_section(MachoHeader64 *header, const char *segment,
                             const char *section_name);

void xnu_pf_disable_patch(ApplePfPatch *patch);

void xnu_pf_enable_patch(ApplePfPatch *patch);

ApplePfRange *xnu_pf_get_actual_text_exec(MachoHeader64 *header);

ApplePfPatch *xnu_pf_ptr_to_data(ApplePfPatchset *patchset, uint64_t slide,
                                 ApplePfRange *range, void *data, size_t datasz,
                                 bool required, xnu_pf_patch_callback callback);

ApplePfPatch *xnu_pf_maskmatch(ApplePfPatchset *patchset, const char *name,
                               uint64_t *matches, uint64_t *masks,
                               uint32_t entryc, bool required,
                               xnu_pf_patch_callback callback);

void xnu_pf_apply(ApplePfRange *range, ApplePfPatchset *patchset);

ApplePfPatchset *xnu_pf_patchset_create(uint8_t pf_accesstype);

void xnu_pf_patchset_destroy(ApplePfPatchset *patchset);

MachoHeader64 *xnu_pf_get_kext_header(MachoHeader64 *kheader,
                                      const char *kext_bundle_id);

void xnu_pf_apply_each_kext(MachoHeader64 *kheader, ApplePfPatchset *patchset);

void xnu_kpf(MachoHeader64 *hdr);
#endif
