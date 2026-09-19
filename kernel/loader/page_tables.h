/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef AGENTOS_LOADER_PAGE_TABLES_H
#define AGENTOS_LOADER_PAGE_TABLES_H
#include <stdbool.h>
#include <stdint.h>

/* The bootstrap identity map occupies L0[0]. Kernel segments share one
 * high 1 GiB window in L0[1], backed by 2 MiB blocks. This accommodates
 * upstream kernels whose virtual/physical offsets differ below bit 30. */
static inline bool loader_map_kernel_segment(uint64_t high[512],
                                             uint64_t blocks[512],
                                             uint64_t va, uint64_t pa,
                                             uint64_t bytes)
{
    const uint64_t block = UINT64_C(1) << 21;
    if (!bytes || va < (UINT64_C(1) << 39) ||
        va >= (UINT64_C(1) << 40) || bytes > (UINT64_C(1) << 40) - va ||
        pa >= (UINT64_C(1) << 40) || bytes > (UINT64_C(1) << 40) - pa ||
        ((va ^ pa) & (block - 1)) ||
        (va >> 30) != ((va + bytes - 1) >> 30)) return false;
    const unsigned slot = (unsigned)((va >> 30) & 511u);
    for (unsigned i = 0; i < 512; i++)
        if (i != slot && high[i]) return false;
    const uint64_t first = va & ~(block - 1);
    const uint64_t last = (va + bytes - 1) & ~(block - 1);
    const uint64_t physical = pa & ~(block - 1);
    for (uint64_t address = first; address <= last; address += block) {
        unsigned index = (unsigned)((address >> 21) & 511u);
        uint64_t descriptor = (physical + address - first) | UINT64_C(0x701);
        if (blocks[index] && blocks[index] != descriptor) return false;
    }
    high[slot] = (uint64_t)(uintptr_t)blocks | UINT64_C(3);
    for (uint64_t address = first; address <= last; address += block)
        blocks[(address >> 21) & 511u] =
            (physical + address - first) | UINT64_C(0x701);
    return true;
}
#endif
