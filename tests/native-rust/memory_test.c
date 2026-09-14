#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

void *aos_rust_memcpy(void *, const void *, size_t);
void *aos_rust_memmove(void *, const void *, size_t);
void *aos_rust_memset(void *, int, size_t);
int aos_rust_memcmp(const void *, const void *, size_t);
size_t aos_rust_strlen(const char *);

int main(void)
{
    unsigned char source[32], actual[32], expected[32];
    for (unsigned i = 0; i < 32; i++) source[i] = (unsigned char)(i * 13);
    assert(aos_rust_memcpy(actual, source, 32) == actual);
    assert(memcmp(actual, source, 32) == 0);
    memcpy(expected, source, 32);
    memmove(expected + 3, expected, 20);
    assert(aos_rust_memmove(actual + 3, actual, 20) == actual + 3);
    assert(memcmp(actual, expected, 32) == 0);
    memmove(expected, expected + 5, 23);
    aos_rust_memmove(actual, actual + 5, 23);
    assert(memcmp(actual, expected, 32) == 0);
    aos_rust_memmove(actual, actual, 32);
    aos_rust_memmove(actual, source, 0);
    assert(memcmp(actual, expected, 32) == 0);
    assert(aos_rust_memset(actual + 2, 0x1a5, 17) == actual + 2);
    memset(expected + 2, 0xa5, 17);
    assert(memcmp(actual, expected, 32) == 0);
    assert(aos_rust_memcmp(actual, expected, 32) == 0);
    assert(aos_rust_memcmp("\xff", "\x7f", 1) > 0);
    assert(aos_rust_memcmp("a", "b", 1) < 0);
    assert(aos_rust_memcmp("a", "b", 0) == 0);
    assert(aos_rust_strlen("") == 0);
    assert(aos_rust_strlen("hello\0hidden") == 5);
    puts("PASS: native Rust freestanding memory primitives");
    return 0;
}
