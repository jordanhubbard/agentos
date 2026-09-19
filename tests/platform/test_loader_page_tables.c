/* SPDX-License-Identifier: BSD-2-Clause */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "page_tables.h"

static uint64_t high[512] __attribute__((aligned(4096)));
static uint64_t blocks[512] __attribute__((aligned(4096)));

static uint64_t translate(uint64_t va)
{
    uint64_t table = high[(va >> 30) & 511u];
    assert((table & 3u) == 3u);
    const uint64_t *entries = (const uint64_t *)(uintptr_t)(table & ~UINT64_C(4095));
    uint64_t descriptor = entries[(va >> 21) & 511u];
    assert((descriptor & 0x701u) == 0x701u);
    return (descriptor & ~UINT64_C(0x1fffff)) | (va & 0x1fffffu);
}

int main(void)
{
    const uint64_t entries[] = {UINT64_C(0x8060000000), UINT64_C(0xffc0000000)};
    for (unsigned i = 0; i < 2; i++) {
        memset(high, 0, sizeof(high));
        memset(blocks, 0, sizeof(blocks));
        assert(loader_map_kernel_segment(high, blocks, entries[i], 0x60000000, 0x242000));
        assert(translate(entries[i]) == 0x60000000);
        assert(translate(entries[i] + 0x241fff) == 0x60241fff);
        /* A second segment may share a block with the same translation. */
        assert(loader_map_kernel_segment(high, blocks, entries[i] + 0x1000,
                                         0x60001000, 0x1000));
        assert(translate(entries[i] + 0x1234) == 0x60001234);
        assert(blocks[((entries[i] >> 21) + 2) & 511u] == 0);
    }
    puts("PASS: released and upstream ARM kernel layouts translate entry, tail and shared blocks");
    return 0;
}
