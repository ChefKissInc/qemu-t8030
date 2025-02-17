/*
 * Copyright (c) 2019 Jonathan Afek <jonyafek@me.com>
 * Copyright (c) 2025 Visual Ehrmanntraut (VisualEhrmanntraut).
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef HW_ARM_APPLE_SILICON_MEM_H
#define HW_ARM_APPLE_SILICON_MEM_H

#include "qemu/osdep.h"
#include "exec/hwaddr.h"
#include "hw/arm/apple-silicon/dtb.h"

extern hwaddr g_virt_base;
extern hwaddr g_phys_base;
extern hwaddr g_virt_slide;
extern hwaddr g_phys_slide;

#define ROUND_UP_16K(v) ROUND_UP(v, 0x4000)

hwaddr vtop_static(hwaddr va);
hwaddr ptov_static(hwaddr pa);
hwaddr vtop_slid(hwaddr va);
hwaddr vtop_mmu(hwaddr va, CPUState *cs);

hwaddr vtop_bases(hwaddr va, hwaddr phys_base, hwaddr virt_base);
hwaddr ptov_bases(hwaddr pa, hwaddr phys_base, hwaddr virt_base);

MemoryRegion *allocate_ram(MemoryRegion *top, const char *name, hwaddr addr,
                           hwaddr size, int priority);

typedef struct CarveoutAllocator CarveoutAllocator;

/// Creates a new carveout allocator
CarveoutAllocator *carveout_alloc_new(DTBNode *carveout_mmap, hwaddr dram_base,
                                      hwaddr dram_size, hwaddr alignment);
/// Returns the address of the allocated region.
hwaddr carveout_alloc_mem(CarveoutAllocator *ca, hwaddr size);
/// Returns the kernel region size.
hwaddr carveout_alloc_finalise(CarveoutAllocator *ca);

#endif /* HW_ARM_APPLE_SILICON_MEM_H */
