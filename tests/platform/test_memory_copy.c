#define _GNU_SOURCE
#include "libs/pd-support/memory_copy.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

static uint8_t pattern(size_t i) { return (uint8_t)(i * 37u + 19u); }
static void check_small(void)
{
    _Alignas(16) uint8_t src[320], dst[320];
    for (size_t i = 0; i < sizeof src; ++i) src[i] = pattern(i);
    for (size_t a = 0; a < 16; ++a)
        for (size_t b = 0; b < 16; ++b)
            for (size_t n = 0; n <= 256; ++n) {
                for (size_t i = 0; i < sizeof dst; ++i) dst[i] = 0xa5;
                assert(aos_copy_nonoverlap(dst+a, src+b, n) == dst+a);
                for (size_t i = 0; i < sizeof dst; ++i) {
                    assert(dst[i] == (i >= a && i < a+n ? pattern(b+i-a) : 0xa5));
                    assert(src[i] == pattern(i));
                }
            }
}
static void check_guards(void)
{
    long page = sysconf(_SC_PAGESIZE);
    assert(page >= 1024);
    size_t p = (size_t)page;
    uint8_t *a = mmap(NULL, 3*p, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    uint8_t *b = mmap(NULL, 3*p, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    assert(a != MAP_FAILED && b != MAP_FAILED);
    assert(mprotect(a+p, p, PROT_READ|PROT_WRITE) == 0);
    assert(mprotect(b+p, p, PROT_READ|PROT_WRITE) == 0);
    for (size_t i = 0; i < p; ++i) a[p+i] = pattern(i);
    assert(mprotect(a+p, p, PROT_READ) == 0);
    for (size_t n = 0; n <= 512; ++n)
        for (size_t extra = 0; extra < 16; ++extra) {
            size_t start = p-n-extra;
            for (size_t i = 0; i < p; ++i) b[p+i] = 0xa5;
            uint8_t *dst = b+p+start;
            /* At n=extra=0 both pointers address inaccessible guard pages. */
            assert(aos_copy_nonoverlap(dst, a+2*p-n, n) == dst);
            for (size_t i = 0; i < p; ++i)
                assert(b[p+i] == (i >= start && i < start+n ?
                    pattern(p-n+i-start) : 0xa5));
        }
    assert(munmap(a, 3*p) == 0 && munmap(b, 3*p) == 0);
}
int main(void)
{
    check_small();
    check_guards();
    const size_t n = 1024u*768u*4u;
    uint8_t *src = malloc(n), *dst = malloc(n);
    assert(src && dst);
    for (size_t i = 0; i < n; ++i) src[i] = pattern(i);
    assert(aos_copy_nonoverlap(dst, src, n) == dst);
    for (size_t i = 0; i < n; ++i) assert(dst[i] == pattern(i));
    free(dst); free(src);
    puts("PASS: memory copy alignments, exact bounds, guard pages and full-frame pixels");
    return 0;
}
