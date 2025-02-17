#include "hw/arm/apple-silicon/boot.h"
#include "hw/arm/apple-silicon/mem.h"
#include "hw/arm/apple-silicon/xnu_pf.h"
#include "qemu/error-report.h"

ApplePfRange *xnu_pf_range_from_va(uint64_t va, uint64_t size)
{
    ApplePfRange *range = g_malloc0(sizeof(ApplePfRange));
    range->va = va;
    range->size = size;
    range->cacheable_base = (uint8_t *)(va - g_virt_base + g_phys_base);
    return range;
}

ApplePfRange *xnu_pf_segment(MachoHeader64 *header, const char *segment_name)
{
    MachoSegmentCommand64 *seg = macho_get_segment(header, segment_name);
    if (seg == NULL) {
        return NULL;
    }

    return xnu_pf_range_from_va(
        xnu_slide_hdr_va(header, 0xFFFF000000000000 | seg->vmaddr),
        seg->filesize);
}

ApplePfRange *xnu_pf_section(MachoHeader64 *header, const char *segment_name,
                             const char *section_name)
{
    MachoSection64 *sec;
    MachoSegmentCommand64 *seg = macho_get_segment(header, segment_name);
    if (seg == NULL) {
        return NULL;
    }

    sec = macho_get_section(seg, section_name);
    if (sec == NULL) {
        return NULL;
    }

    return xnu_pf_range_from_va(
        xnu_slide_hdr_va(header, 0xFFFF000000000000 | sec->addr), sec->size);
}

static MachoHeader64 *xnu_pf_get_first_kext(MachoHeader64 *kheader)
{
    uint64_t *start, kextb;
    ApplePfRange *kmod_start_range;
    MachoHeader64 *rv;

    kmod_start_range =
        xnu_pf_section(kheader, "__PRELINK_INFO", "__kmod_start");

    if (kmod_start_range == NULL) {
        kmod_start_range = xnu_pf_section(kheader, "__PRELINK_TEXT", "__text");
        if (kmod_start_range == NULL) {
            error_report("Unsupported XNU.");
        }
        rv = (MachoHeader64 *)kmod_start_range->cacheable_base;
        g_free(kmod_start_range);

        return rv;
    }

    start = (uint64_t *)(kmod_start_range->cacheable_base);
    kextb = g_virt_slide + (0xffff000000000000 | start[0]);

    g_free(kmod_start_range);

    return (MachoHeader64 *)xnu_va_to_ptr(kextb);
}

MachoHeader64 *xnu_pf_get_kext_header(MachoHeader64 *kheader,
                                      const char *kext_bundle_id)
{
    uint64_t *info, *start;
    uint32_t count;
    uint32_t i;

    if (kheader->file_type == MH_FILESET) {
        return macho_get_fileset_header(kheader, kext_bundle_id);
    }

    ApplePfRange *kmod_info_range =
        xnu_pf_section(kheader, "__PRELINK_INFO", "__kmod_info");
    if (!kmod_info_range) {
        char kname[256];
        const char *prelinkinfo, *last_dict;
        ApplePfRange *kext_info_range =
            xnu_pf_section(kheader, "__PRELINK_INFO", "__info");
        if (!kext_info_range) {
            error_report("unsupported xnu");
        }

        prelinkinfo = strstr((const char *)kext_info_range->cacheable_base,
                             "PrelinkInfoDictionary");
        last_dict = strstr(prelinkinfo, "<array>") + 7;
        while (last_dict) {
            const char *nested_dict, *ident;
            const char *end_dict = strstr(last_dict, "</dict>");
            if (!end_dict) {
                break;
            }

            nested_dict = strstr(last_dict + 1, "<dict>");
            while (nested_dict) {
                if (nested_dict > end_dict) {
                    break;
                }

                nested_dict = strstr(nested_dict + 1, "<dict>");
                end_dict = strstr(end_dict + 1, "</dict>");
            }

            ident = g_strstr_len(last_dict, end_dict - last_dict,
                                 "CFBundleIdentifier");
            if (ident) {
                const char *value = strstr(ident, "<string>");
                if (value) {
                    const char *value_end;

                    value += strlen("<string>");
                    value_end = strstr(value, "</string>");
                    if (value_end) {
                        memcpy(kname, value, value_end - value);
                        kname[value_end - value] = 0;
                        if (strcmp(kname, kext_bundle_id) == 0) {
                            const char *addr =
                                g_strstr_len(last_dict, end_dict - last_dict,
                                             "_PrelinkExecutableLoadAddr");
                            if (addr) {
                                const char *avalue = strstr(addr, "<integer");
                                if (avalue) {
                                    avalue = strstr(avalue, ">");
                                    if (avalue) {
                                        avalue++;
                                        g_free(kext_info_range);
                                        return xnu_va_to_ptr(
                                            g_virt_slide +
                                            strtoull(avalue, 0, 0));
                                    }
                                }
                            }
                        }
                    }
                }
            }

            last_dict = strstr(end_dict, "<dict>");
        }

        g_free(kext_info_range);
        return NULL;
    }
    ApplePfRange *kmod_start_range =
        xnu_pf_section(kheader, "__PRELINK_INFO", "__kmod_start");
    if (!kmod_start_range) {
        return NULL;
    }

    info = (uint64_t *)(kmod_info_range->cacheable_base);
    start = (uint64_t *)(kmod_start_range->cacheable_base);
    count = kmod_info_range->size / 8;
    for (i = 0; i < count; i++) {
        const char *kext_name =
            (const char *)xnu_va_to_ptr(g_virt_slide +
                                        (0xffff000000000000 | info[i])) +
            0x10;
        if (strcmp(kext_name, kext_bundle_id) == 0) {
            g_free(kmod_info_range);
            g_free(kmod_start_range);
            return (MachoHeader64 *)xnu_va_to_ptr(
                g_virt_slide + (0xffff000000000000 | start[i]));
        }
    }

    g_free(kmod_info_range);
    g_free(kmod_start_range);

    return NULL;
}

ApplePfRange *xnu_pf_get_actual_text_exec(MachoHeader64 *header)
{
    ApplePfRange *text_exec = xnu_pf_section(header, "__TEXT_EXEC", "__text");
    if (header->file_type == MH_FILESET) {
        return text_exec;
    }

    MachoHeader64 *first_kext = xnu_pf_get_first_kext(header);
    if (first_kext) {
        ApplePfRange *first_kext_text_exec =
            xnu_pf_section(first_kext, "__TEXT_EXEC", "__text");

        if (first_kext_text_exec) {
            uint64_t text_exec_end_real;
            uint64_t text_exec_end = text_exec_end_real =
                ((uint64_t)(text_exec->va)) + text_exec->size;
            uint64_t first_kext_p = ((uint64_t)(first_kext_text_exec->va));

            if (text_exec_end > first_kext_p &&
                first_kext_text_exec->va > text_exec->va) {
                text_exec_end = first_kext_p;
            }

            text_exec->size -= text_exec_end_real - text_exec_end;
        }

        g_free(first_kext_text_exec);
    }
    return text_exec;
}

void xnu_pf_apply_each_kext(MachoHeader64 *kheader, ApplePfPatchset *patchset)
{
    bool is_required;
    uint64_t *start;
    uint32_t count;
    uint32_t i;

    ApplePfRange *kmod_start_range =
        xnu_pf_section(kheader, "__PRELINK_INFO", "__kmod_start");
    if (!kmod_start_range) {
        ApplePfRange *kext_text_exec_range =
            xnu_pf_section(kheader, "__PLK_TEXT_EXEC", "__text");
        if (!kext_text_exec_range) {
            error_report("unsupported xnu");
        }
        xnu_pf_apply(kext_text_exec_range, patchset);
        g_free(kext_text_exec_range);
        return;
    }

    is_required = patchset->is_required;
    patchset->is_required = false;

    start = (uint64_t *)(kmod_start_range->cacheable_base);
    count = kmod_start_range->size / 8;
    for (i = 0; i < count; i++) {
        MachoHeader64 *kexth = (MachoHeader64 *)xnu_va_to_ptr(
            g_virt_slide + (0xffff000000000000 | start[i]));
        ApplePfRange *apply_range =
            xnu_pf_section(kexth, "__TEXT_EXEC", "__text");
        if (apply_range) {
            xnu_pf_apply(apply_range, patchset);
            g_free(apply_range);
        }
    }
    g_free(kmod_start_range);

    patchset->is_required = is_required;
    if (is_required) {
        for (ApplePfPatch *patch = patchset->patch_head; patch;
             patch = patch->next_patch) {
            if (patch->is_required && !patch->has_fired) {
                error_report("Missing patch: %s", patch->name);
            }
        }
    }
}

ApplePfPatchset *xnu_pf_patchset_create(uint8_t pf_accesstype)
{
    ApplePfPatchset *r = g_new0(ApplePfPatchset, 1);
    r->accesstype = pf_accesstype;
    r->is_required = true;
    return r;
}

typedef struct {
    ApplePfPatch patch;
    uint32_t pair_count;
    uint64_t pairs[][2];
} ApplePfMaskMatch;

static inline bool xnu_pf_maskmatch_match_8(ApplePfMaskMatch *patch,
                                            uint8_t access_type,
                                            uint8_t *preread,
                                            uint8_t *cacheable_stream)
{
    uint32_t i, count = patch->pair_count;

    for (i = 0; i < count; i++) {
        if (count < 8) {
            if ((preread[i] & patch->pairs[i][1]) != patch->pairs[i][0]) {
                return false;
            }
        } else {
            if ((cacheable_stream[i] & patch->pairs[i][1]) !=
                patch->pairs[i][0]) {
                return false;
            }
        }
    }

    return true;
}

static inline bool xnu_pf_maskmatch_match_16(ApplePfMaskMatch *patch,
                                             uint8_t access_type,
                                             uint16_t *preread,
                                             uint16_t *cacheable_stream)
{
    uint32_t i, count = patch->pair_count;

    for (i = 0; i < count; i++) {
        if (count < 8) {
            if ((preread[i] & patch->pairs[i][1]) != patch->pairs[i][0]) {
                return false;
            }
        } else {
            if ((cacheable_stream[i] & patch->pairs[i][1]) !=
                patch->pairs[i][0]) {
                return false;
            }
        }
    }

    return true;
}

static inline bool xnu_pf_maskmatch_match_32(ApplePfMaskMatch *patch,
                                             uint8_t access_type,
                                             uint32_t *preread,
                                             uint32_t *cacheable_stream)
{
    uint32_t i, count = patch->pair_count;
    for (i = 0; i < count; i++) {
        if (count < 8) {
            if ((preread[i] & patch->pairs[i][1]) != patch->pairs[i][0]) {
                return false;
            }
        } else {
            if ((cacheable_stream[i] & patch->pairs[i][1]) !=
                patch->pairs[i][0]) {
                return false;
            }
        }
    }

    return true;
}

static inline bool xnu_pf_maskmatch_match_64(ApplePfMaskMatch *patch,
                                             uint8_t access_type,
                                             uint64_t *preread,
                                             uint64_t *cacheable_stream)
{
    uint32_t i, count = patch->pair_count;

    for (i = 0; i < count; i++) {
        if (count < 8) {
            if ((preread[i] & patch->pairs[i][1]) != patch->pairs[i][0]) {
                return false;
            }
        } else {
            if ((cacheable_stream[i] & patch->pairs[i][1]) !=
                patch->pairs[i][0]) {
                return false;
            }
        }
    }

    return true;
}

static void xnu_pf_maskmatch_match(ApplePfMaskMatch *patch, uint8_t access_type,
                                   void *preread, void *cacheable_stream)
{
    bool val = false;

    switch (access_type) {
    case XNU_PF_ACCESS_8BIT:
        val = xnu_pf_maskmatch_match_8(patch, access_type, preread,
                                       cacheable_stream);
        break;
    case XNU_PF_ACCESS_16BIT:
        val = xnu_pf_maskmatch_match_16(patch, access_type, preread,
                                        cacheable_stream);
        break;
    case XNU_PF_ACCESS_32BIT:
        val = xnu_pf_maskmatch_match_32(patch, access_type, preread,
                                        cacheable_stream);
        break;
    case XNU_PF_ACCESS_64BIT:
        val = xnu_pf_maskmatch_match_64(patch, access_type, preread,
                                        cacheable_stream);
        break;
    default:
        break;
    }

    if (val) {
        if (patch->patch.pf_callback((ApplePfPatch *)patch, cacheable_stream)) {
            patch->patch.has_fired = true;
        }
    }
}

struct xnu_pf_ptr_to_datamatch {
    ApplePfPatch patch;
    void *data;
    size_t datasz;
    uint64_t slide;
    ApplePfRange *range;
};

static void xnu_pf_ptr_to_data_match(struct xnu_pf_ptr_to_datamatch *patch,
                                     uint8_t access_type, void *preread,
                                     void *cacheable_stream)
{
    uint64_t pointer = *(uint64_t *)preread;

    pointer |= 0xffff000000000000;
    pointer += patch->slide;

    if (pointer >= patch->range->va &&
        pointer < (patch->range->va + patch->range->size)) {
        if (memcmp(patch->data,
                   (void *)(pointer - patch->range->va +
                            patch->range->cacheable_base),
                   patch->datasz) == 0) {
            if (patch->patch.pf_callback((ApplePfPatch *)patch,
                                         cacheable_stream)) {
                patch->patch.has_fired = true;
            }
        }
    }
}

ApplePfPatch *xnu_pf_maskmatch(ApplePfPatchset *patchset, const char *name,
                               uint64_t *matches, uint64_t *masks,
                               uint32_t entryc, bool required,
                               xnu_pf_patch_callback callback)
{
    uint32_t i;
    ApplePfMaskMatch *mm;
    uint32_t loadc;

    for (i = 0; i < entryc; i++) {
        if ((matches[i] & masks[i]) != matches[i]) {
            error_report("Bad maskmatch: %s (index %u)", name, i);
        }
    }

    mm = g_malloc0(sizeof(ApplePfMaskMatch) + 16 * entryc);
    memset(mm, 0, sizeof(ApplePfMaskMatch));
    mm->patch.should_match = true;
    mm->patch.pf_callback = (void *)callback;
    mm->patch.pf_match = (void *)xnu_pf_maskmatch_match;
    mm->patch.is_required = required;
    mm->patch.name = name;
    mm->pair_count = entryc;

    loadc = entryc;
    if (loadc > 8) {
        loadc = 8;
    }

    for (i = 0; i < entryc; i++) {
        mm->pairs[i][0] = matches[i];
        mm->pairs[i][1] = masks[i];
    }

    mm->patch.next_patch = patchset->patch_head;
    patchset->patch_head = &mm->patch;

    return &mm->patch;
}

ApplePfPatch *xnu_pf_ptr_to_data(ApplePfPatchset *patchset, uint64_t slide,
                                 ApplePfRange *range, void *data, size_t datasz,
                                 bool required, xnu_pf_patch_callback callback)
{
    struct xnu_pf_ptr_to_datamatch *mm =
        g_malloc0(sizeof(struct xnu_pf_ptr_to_datamatch));

    memset(mm, 0, sizeof(struct xnu_pf_ptr_to_datamatch));
    mm->patch.should_match = true;
    mm->patch.pf_callback = (void *)callback;
    mm->patch.pf_match = (void *)xnu_pf_ptr_to_data_match;
    mm->patch.is_required = required;

    mm->slide = slide;
    mm->range = range;
    mm->data = data;
    mm->datasz = datasz;

    mm->patch.next_patch = patchset->patch_head;
    patchset->patch_head = &mm->patch;

    return &mm->patch;
}

void xnu_pf_disable_patch(ApplePfPatch *patch)
{
    if (!patch->should_match) {
        return;
    }

    patch->should_match = false;
}

void xnu_pf_enable_patch(ApplePfPatch *patch)
{
    if (patch->should_match) {
        return;
    }

    patch->should_match = true;
}

static inline void xnu_pf_apply_8(ApplePfRange *range,
                                  ApplePfPatchset *patchset)
{
    uint8_t *stream = (uint8_t *)range->cacheable_base;
    uint8_t reads[8];
    uint32_t index, stream_iters = range->size;
    int i;

    for (i = 0; i < 8; i++) {
        reads[i] = stream[i];
    }

    for (index = 0; index < stream_iters; index++) {
        ApplePfPatch *patch = patchset->patch_head;

        while (patch) {
            if (patch->should_match) {
                patch->pf_match(patch, XNU_PF_ACCESS_8BIT, reads,
                                &stream[index]);
            }
            patch = patch->next_patch;
        }

        for (i = 0; i < 7; i++) {
            reads[i] = reads[i + 1];
        }
        reads[7] = stream[index + 8];
    }
}

static inline void xnu_pf_apply_16(ApplePfRange *range,
                                   ApplePfPatchset *patchset)
{
    uint16_t *stream = (uint16_t *)range->cacheable_base;
    uint16_t reads[8];
    uint32_t i, index, stream_iters = range->size >> 1;

    for (i = 0; i < 8; i++) {
        reads[i] = stream[i];
    }

    for (index = 0; index < stream_iters; index++) {
        ApplePfPatch *patch = patchset->patch_head;

        while (patch) {
            if (patch->should_match) {
                patch->pf_match(patch, XNU_PF_ACCESS_16BIT, reads,
                                &stream[index]);
            }
            patch = patch->next_patch;
        }

        for (i = 0; i < 7; i++) {
            reads[i] = reads[i + 1];
        }
        reads[7] = stream[index + 8];
    }
}

static inline void xnu_pf_apply_32(ApplePfRange *range,
                                   ApplePfPatchset *patchset)
{
    uint32_t *stream = (uint32_t *)range->cacheable_base;
    uint32_t reads[8];
    uint32_t i, index, stream_iters = range->size >> 2;

    for (i = 0; i < 8; i++) {
        reads[i] = stream[i];
    }

    for (index = 0; index < stream_iters; index++) {
        ApplePfPatch *patch = patchset->patch_head;

        while (patch) {
            if (patch->should_match) {
                patch->pf_match(patch, XNU_PF_ACCESS_32BIT, reads,
                                &stream[index]);
            }
            patch = patch->next_patch;
        }

        for (i = 0; i < 7; i++) {
            reads[i] = reads[i + 1];
        }
        reads[7] = stream[index + 8];
    }
}

static inline void xnu_pf_apply_64(ApplePfRange *range,
                                   ApplePfPatchset *patchset)
{
    uint64_t *stream = (uint64_t *)range->cacheable_base;
    uint64_t reads[8];
    uint32_t i, index, stream_iters = range->size >> 2;

    for (i = 0; i < 8; i++) {
        reads[i] = stream[i];
    }

    for (index = 0; index < stream_iters; index++) {
        ApplePfPatch *patch = patchset->patch_head;

        while (patch) {
            if (patch->should_match) {
                patch->pf_match(patch, XNU_PF_ACCESS_64BIT, reads,
                                &stream[index]);
            }
            patch = patch->next_patch;
        }

        for (i = 0; i < 7; i++) {
            reads[i] = reads[i + 1];
        }
        reads[7] = stream[index + 8];
    }
}

void xnu_pf_apply(ApplePfRange *range, ApplePfPatchset *patchset)
{
    switch (patchset->accesstype) {
    case XNU_PF_ACCESS_8BIT:
        xnu_pf_apply_8(range, patchset);
        break;
    case XNU_PF_ACCESS_16BIT:
        xnu_pf_apply_16(range, patchset);
        break;
    case XNU_PF_ACCESS_32BIT:
        xnu_pf_apply_32(range, patchset);
        break;
    case XNU_PF_ACCESS_64BIT:
        xnu_pf_apply_64(range, patchset);
        break;
    default:
        break;
    }

    if (patchset->is_required) {
        for (ApplePfPatch *patch = patchset->patch_head; patch;
             patch = patch->next_patch) {
            if (patch->is_required && !patch->has_fired) {
                error_report("Missing patch: %s", patch->name);
            }
        }
    }
}

void xnu_pf_patchset_destroy(ApplePfPatchset *patchset)
{
    ApplePfPatch *o_patch;
    ApplePfPatch *patch = patchset->patch_head;

    while (patch) {
        o_patch = patch;
        patch = patch->next_patch;
        g_free(o_patch);
    }

    g_free(patchset);
}
