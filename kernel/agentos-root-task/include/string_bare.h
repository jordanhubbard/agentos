/*
 * Minimal string ops for bare-metal seL4 PDs (no libc)
 */
#pragma once
#include <stddef.h>

static inline void *memset(void *s, int c, size_t n) {
    unsigned char *p = s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

static inline void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

static inline int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *p = a, *q = b;
    while (n--) {
        if (*p != *q) return *p - *q;
        p++; q++;
    }
    return 0;
}

static inline size_t strlen(const char *s) {
    size_t n = 0;
    while (*s++) n++;
    return n;
}

static inline char *strncpy(char *dst, const char *src, size_t n) {
    char *d = dst;
    while (n-- && (*d++ = *src++));
    while (n--) *d++ = '\0';
    return dst;
}
