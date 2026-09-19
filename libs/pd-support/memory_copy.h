/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef AGENTOS_MEMORY_COPY_H
#define AGENTOS_MEMORY_COPY_H
#include <stddef.h>
#include <stdint.h>

/* Ordinary memory only, not MMIO. Like memcpy, the ranges must not overlap.
 * may_alias permits copying arbitrary object representations through words.
 * Never round either range outward: even a one-byte tail can end at an
 * unmapped page. Differently aligned ranges retain the byte-copy path. */
typedef uint64_t aos_copy_word_t __attribute__((__may_alias__));
static inline void *aos_copy_nonoverlap(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if ((((uintptr_t)d ^ (uintptr_t)s) & 7u) == 0) {
        while (n && ((uintptr_t)d & 7u)) {
            *d++ = *s++;
            --n;
        }
        aos_copy_word_t *dw = (aos_copy_word_t *)d;
        const aos_copy_word_t *sw = (const aos_copy_word_t *)s;
        /* Under -mstrict-align, vectorising an arbitrary byte loop expands
         * into byte assembly. These proven-aligned scalar words avoid that
         * expansion without adding SIMD state or unaligned accesses. */
#ifdef __clang__
#pragma clang loop vectorize(disable) interleave(disable)
#endif
        while (n >= sizeof(*dw)) {
            *dw++ = *sw++;
            n -= sizeof(*dw);
        }
        d = (uint8_t *)dw;
        s = (const uint8_t *)sw;
    }
    while (n--) *d++ = *s++;
    return dst;
}
#endif
