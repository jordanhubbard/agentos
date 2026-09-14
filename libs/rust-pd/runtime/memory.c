/* Freestanding memory primitives used by Rust core/alloc. No logging,
 * allocator, device access or OS calls belong in this runtime dependency. */
#include <stddef.h>
#include <stdint.h>

#ifdef AGENTOS_TEST_HOST
#define memcpy aos_rust_memcpy
#define memmove aos_rust_memmove
#define memset aos_rust_memset
#define memcmp aos_rust_memcmp
#define strlen aos_rust_strlen
#endif

void *memcpy(void *destination, const void *source, size_t count)
{
    unsigned char *out = destination;
    const unsigned char *in = source;
    for (size_t i = 0; i < count; i++) out[i] = in[i];
    return destination;
}

void *memmove(void *destination, const void *source, size_t count)
{
    unsigned char *out = destination;
    const unsigned char *in = source;
    if ((uintptr_t)out < (uintptr_t)in) {
        for (size_t i = 0; i < count; i++) out[i] = in[i];
    } else if (out != in) {
        for (size_t i = count; i > 0; i--) out[i - 1] = in[i - 1];
    }
    return destination;
}

void *memset(void *destination, int value, size_t count)
{
    unsigned char *out = destination;
    for (size_t i = 0; i < count; i++) out[i] = (unsigned char)value;
    return destination;
}

int memcmp(const void *left, const void *right, size_t count)
{
    const unsigned char *a = left, *b = right;
    for (size_t i = 0; i < count; i++) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

size_t strlen(const char *text)
{
    size_t count = 0;
    while (text[count]) count++;
    return count;
}
