/*
 * Bare-metal libc shim implementation for wasm3 on agentOS
 *
 * Provides minimal libc functions needed by wasm3 in a freestanding
 * seL4 Microkit protection domain. No OS, no libc, just us.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <microkit.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>

/* Forward declarations (matching m3_bare_metal.h) */

/* ---- Memory operations ---- */

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else if (d > s) {
        d += n; s += n;
        while (n--) *--d = *--s;
    }
    return dst;
}

void *memset(void *s, int c, size_t n) {
    uint8_t *p = (uint8_t *)s;
    while (n--) *p++ = (uint8_t)c;
    return s;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *a = (const uint8_t *)s1;
    const uint8_t *b = (const uint8_t *)s2;
    while (n--) {
        if (*a != *b) return (*a < *b) ? -1 : 1;
        a++; b++;
    }
    return 0;
}

/* ---- String operations ---- */

size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && *s1 == *s2) { s1++; s2++; }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && *s1 == *s2) { s1++; s2++; n--; }
    if (n == 0) return 0;
    return (unsigned char)*s1 - (unsigned char)*s2;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++));
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    char *d = dst;
    while (n && (*d++ = *src++)) n--;
    while (n--) *d++ = '\0';
    return dst;
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    size_t nlen = strlen(needle);
    while (*haystack) {
        if (strncmp(haystack, needle, nlen) == 0)
            return (char *)haystack;
        haystack++;
    }
    return NULL;
}

/* ---- Formatted output (simplified) ---- */
/* wasm3 uses printf/snprintf for debug logging and error messages.
 console_log(15, 15, "");
 * Format parsing is minimal — just enough for wasm3's actual usage. */

/* Simple number-to-string for snprintf */
static int int_to_str(char *buf, size_t bufsz, int64_t val, int base, bool is_unsigned) {
    char tmp[24];
    int i = 0;
    bool neg = false;
    uint64_t uval;

    if (!is_unsigned && val < 0) {
        neg = true;
        uval = (uint64_t)(-val);
    } else {
        uval = (uint64_t)val;
    }

    if (uval == 0) {
        tmp[i++] = '0';
    } else {
        while (uval > 0 && i < 22) {
            int digit = (int)(uval % base);
            tmp[i++] = (digit < 10) ? ('0' + digit) : ('a' + digit - 10);
            uval /= base;
        }
    }

    int written = 0;
    if (neg && written < (int)bufsz - 1) buf[written++] = '-';
    for (int j = i - 1; j >= 0 && written < (int)bufsz - 1; j--) {
        buf[written++] = tmp[j];
    }
    if (written < (int)bufsz) buf[written] = '\0';
    return written;
}

int vsnprintf(char *str, size_t size, const char *fmt, va_list ap) {
    if (!str || size == 0) return 0;

    size_t pos = 0;
    #define PUT(c) do { if (pos < size - 1) str[pos] = (c); pos++; } while(0)

    while (*fmt) {
        if (*fmt != '%') {
            PUT(*fmt++);
            continue;
        }
        fmt++; /* skip '%' */

        /* Parse flags/width (minimal) */
        bool long_flag = false;
        bool longlong_flag = false;
        bool size_t_flag = false;

        /* Skip flags */
        while (*fmt == '-' || *fmt == '+' || *fmt == ' ' || *fmt == '0' || *fmt == '#') fmt++;
        /* Skip width */
        while (*fmt >= '0' && *fmt <= '9') fmt++;
        /* Skip precision */
        if (*fmt == '.') {
            fmt++;
            while (*fmt >= '0' && *fmt <= '9') fmt++;
        }

        /* Length modifiers */
        if (*fmt == 'l') {
            fmt++;
            long_flag = true;
            if (*fmt == 'l') { fmt++; longlong_flag = true; }
        } else if (*fmt == 'z') {
            fmt++;
            size_t_flag = true;
        } else if (*fmt == 'h') {
            fmt++;
            if (*fmt == 'h') fmt++;
        }

        char tmpbuf[24];
        int len;

        switch (*fmt) {
            case 'd':
            case 'i': {
                int64_t val;
                if (longlong_flag) val = va_arg(ap, int64_t);
                else if (long_flag || size_t_flag) val = va_arg(ap, long);
                else val = va_arg(ap, int);
                len = int_to_str(tmpbuf, sizeof(tmpbuf), val, 10, false);
                for (int i = 0; i < len; i++) PUT(tmpbuf[i]);
                break;
            }
            case 'u': {
                uint64_t val;
                if (longlong_flag) val = va_arg(ap, uint64_t);
                else if (long_flag || size_t_flag) val = va_arg(ap, unsigned long);
                else val = va_arg(ap, unsigned int);
                len = int_to_str(tmpbuf, sizeof(tmpbuf), (int64_t)val, 10, true);
                for (int i = 0; i < len; i++) PUT(tmpbuf[i]);
                break;
            }
            case 'x':
            case 'X': {
                uint64_t val;
                if (longlong_flag) val = va_arg(ap, uint64_t);
                else if (long_flag || size_t_flag) val = va_arg(ap, unsigned long);
                else val = va_arg(ap, unsigned int);
                len = int_to_str(tmpbuf, sizeof(tmpbuf), (int64_t)val, 16, true);
                for (int i = 0; i < len; i++) PUT(tmpbuf[i]);
                break;
            }
            case 'p': {
                uintptr_t val = (uintptr_t)va_arg(ap, void *);
                PUT('0'); PUT('x');
                len = int_to_str(tmpbuf, sizeof(tmpbuf), (int64_t)val, 16, true);
                for (int i = 0; i < len; i++) PUT(tmpbuf[i]);
                break;
            }
            case 's': {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                while (*s) PUT(*s++);
                break;
            }
            case 'c': {
                int c = va_arg(ap, int);
                PUT((char)c);
                break;
            }
            case 'f':
            case 'g':
            case 'e': {
                /* Minimal float printing — just output "?" for now.
                 * wasm3's core doesn't actually printf floats in hot paths. */
                (void)va_arg(ap, double);
                PUT('?');
                break;
            }
            case '%':
                PUT('%');
                break;
            case '\0':
                goto done;
            default:
                PUT('%');
                PUT(*fmt);
                break;
        }
        fmt++;
    }

done:
    if (pos < size) str[pos] = '\0';
    else if (size > 0) str[size - 1] = '\0';

    #undef PUT
    return (int)pos;
}

int snprintf(char *str, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(str, size, fmt, ap);
    va_end(ap);
    return ret;
}

int printf(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    console_log(15, 15, buf);
    return ret;
}

/* Fake FILE pointers */
static int _stderr_dummy;
static int _stdout_dummy;
void *stderr = &_stderr_dummy;
void *stdout = &_stdout_dummy;

int fprintf(void *stream, const char *fmt, ...) {
    (void)stream;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    console_log(15, 15, buf);
    return ret;
}

int puts(const char *s) {
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = s; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = "\n"; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        console_log(15, 15, _cl_buf);
    }
    return 0;
}

int fputs(const char *s, void *stream) {
    (void)stream;
    console_log(15, 15, s);
    return 0;
}

/* ---- stdlib functions ---- */
/* With d_m3FixedHeap enabled, wasm3 uses its internal bump allocator.
 * These are fallbacks in case any code path still references them. */

void *malloc(size_t size) {
    (void)size;
    console_log(15, 15, "[bare_metal] WARNING: malloc called (should use fixed heap)\n");
    return NULL;
}

void *calloc(size_t nmemb, size_t size) {
    (void)nmemb; (void)size;
    console_log(15, 15, "[bare_metal] WARNING: calloc called\n");
    return NULL;
}

void *realloc(void *ptr, size_t size) {
    (void)ptr; (void)size;
    console_log(15, 15, "[bare_metal] WARNING: realloc called\n");
    return NULL;
}

void free(void *ptr) {
    (void)ptr;
    /* no-op */
}

void abort(void) {
    console_log(15, 15, "[bare_metal] ABORT\n");
    for (;;) {} /* spin forever */
}

void exit(int status) {
    (void)status;
    console_log(15, 15, "[bare_metal] EXIT\n");
    for (;;) {}
}

long strtol(const char *nptr, char **endptr, int base) {
    long result = 0;
    bool neg = false;
    
    while (*nptr == ' ' || *nptr == '\t') nptr++;
    if (*nptr == '-') { neg = true; nptr++; }
    else if (*nptr == '+') nptr++;

    if (base == 0) {
        if (*nptr == '0' && (nptr[1] == 'x' || nptr[1] == 'X')) { base = 16; nptr += 2; }
        else if (*nptr == '0') { base = 8; nptr++; }
        else base = 10;
    } else if (base == 16 && *nptr == '0' && (nptr[1] == 'x' || nptr[1] == 'X')) {
        nptr += 2;
    }

    while (*nptr) {
        int digit;
        if (*nptr >= '0' && *nptr <= '9') digit = *nptr - '0';
        else if (*nptr >= 'a' && *nptr <= 'f') digit = *nptr - 'a' + 10;
        else if (*nptr >= 'A' && *nptr <= 'F') digit = *nptr - 'A' + 10;
        else break;
        if (digit >= base) break;
        result = result * base + digit;
        nptr++;
    }

    if (endptr) *endptr = (char *)nptr;
    return neg ? -result : result;
}

unsigned long strtoul(const char *nptr, char **endptr, int base) {
    return (unsigned long)strtol(nptr, endptr, base);
}

/* ---- Assert ---- */

__attribute__((weak))
void __assert_fail(const char *expr, const char *file, int line, const char *function) {
    (void)function;
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = "[ASSERT] "; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = file; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = ":"; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        console_log(15, 15, _cl_buf);
    }
    char lbuf[12];
    int_to_str(lbuf, sizeof(lbuf), line, 10, false);
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = lbuf; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = ": "; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = expr; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = "\n"; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        console_log(15, 15, _cl_buf);
    }
    for (;;) {} /* hang */
}

/* ---- Math stubs ---- */
/* These are needed for wasm3 float support.
 * Using compiler builtins where available, software fallback otherwise. */

double fabs(double x) { return x < 0 ? -x : x; }
float  fabsf(float x) { return x < 0 ? -x : x; }

/* Software sqrt (Newton-Raphson, ~8 iterations) */
double sqrt(double x) {
    if (x <= 0.0) return 0.0;
    double guess = x * 0.5;
    for (int i = 0; i < 10; i++) {
        guess = 0.5 * (guess + x / guess);
    }
    return guess;
}

float sqrtf(float x) { return (float)sqrt((double)x); }

double ceil(double x) {
    int64_t i = (int64_t)x;
    if (x > 0 && x != (double)i) return (double)(i + 1);
    return (double)i;
}

double floor(double x) {
    int64_t i = (int64_t)x;
    if (x < 0 && x != (double)i) return (double)(i - 1);
    return (double)i;
}

double trunc(double x) {
    return (double)(int64_t)x;
}

double rint(double x) {
    return (double)(int64_t)(x + (x >= 0 ? 0.5 : -0.5));
}

double fmin(double x, double y) { return x < y ? x : y; }
double fmax(double x, double y) { return x > y ? x : y; }
float  fminf(float x, float y) { return x < y ? x : y; }
float  fmaxf(float x, float y) { return x > y ? x : y; }
float  ceilf(float x)  { return (float)ceil((double)x); }
float  floorf(float x) { return (float)floor((double)x); }
float  truncf(float x) { return (float)trunc((double)x); }
float  rintf(float x)  { return (float)rint((double)x); }

/* ---- Additional math and string functions for wasm3 ---- */

double copysign(double x, double y) {
    uint64_t xi, yi;
    __builtin_memcpy(&xi, &x, 8);
    __builtin_memcpy(&yi, &y, 8);
    xi = (xi & 0x7FFFFFFFFFFFFFFFULL) | (yi & 0x8000000000000000ULL);
    double r; __builtin_memcpy(&r, &xi, 8); return r;
}

float copysignf(float x, float y) {
    uint32_t xi, yi;
    __builtin_memcpy(&xi, &x, 4);
    __builtin_memcpy(&yi, &y, 4);
    xi = (xi & 0x7FFFFFFF) | (yi & 0x80000000);
    float r; __builtin_memcpy(&r, &xi, 4); return r;
}

double strtod(const char *str, char **endptr) {
    while (*str == ' ' || *str == '\t') str++;
    int neg = 0;
    if (*str == '-') { neg = 1; str++; }
    else if (*str == '+') str++;
    double result = 0.0;
    while (*str >= '0' && *str <= '9')
        result = result * 10.0 + (*str++ - '0');
    if (*str == '.') {
        str++;
        double frac = 0.1;
        while (*str >= '0' && *str <= '9') {
            result += (*str++ - '0') * frac;
            frac *= 0.1;
        }
    }
    if (endptr) *endptr = (char *)str;
    return neg ? -result : result;
}

float strtof(const char *str, char **endptr) {
    return (float)strtod(str, endptr);
}

unsigned long long strtoull(const char *str, char **endptr, int base) {
    while (*str == ' ' || *str == '\t') str++;
    if (base == 0) {
        if (str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) { base = 16; str += 2; }
        else if (str[0] == '0') { base = 8; str++; }
        else base = 10;
    } else if (base == 16 && str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
        str += 2;
    }
    unsigned long long result = 0;
    while (1) {
        int digit;
        if (*str >= '0' && *str <= '9') digit = *str - '0';
        else if (*str >= 'a' && *str <= 'f') digit = *str - 'a' + 10;
        else if (*str >= 'A' && *str <= 'F') digit = *str - 'A' + 10;
        else break;
        if (digit >= base) break;
        result = result * (unsigned long long)base + (unsigned long long)digit;
        str++;
    }
    if (endptr) *endptr = (char *)str;
    return result;
}

long long strtoll(const char *str, char **endptr, int base) {
    while (*str == ' ' || *str == '\t') str++;
    int neg = 0;
    if (*str == '-') { neg = 1; str++; }
    else if (*str == '+') str++;
    long long r = (long long)strtoull(str, endptr, base);
    return neg ? -r : r;
}
