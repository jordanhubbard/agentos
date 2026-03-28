/*
 * Bare-metal libc shim for wasm3 on agentOS/seL4 Microkit
 *
 * wasm3 includes <stdio.h>, <stdlib.h>, <string.h>, <assert.h>.
 * On a freestanding seL4 Microkit PD, those don't exist.
 * This header provides minimal stubs for what wasm3 actually uses.
 *
 * Strategy: compile wasm3 sources with -include m3_bare_metal.h
 * which gets included before any other header. We then define
 * guards so the real system headers are skipped.
 */

#ifndef m3_bare_metal_h
#define m3_bare_metal_h

/* Include our agentOS config FIRST (before m3_config.h) */
#include "m3_agentos_config.h"

/* Standard integer types from the compiler */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <limits.h>
#include <float.h>

/*
 * Guard out system headers that wasm3 tries to include.
 * We provide the needed symbols below.
 */
#define _STDIO_H
#define _STDIO_H_
#define _STDLIB_H
#define _STDLIB_H_
#define _STRING_H
#define _STRING_H_
#define _ASSERT_H
#define _ASSERT_H_

/* These are the actual clang/GCC freestanding guards */
#ifndef __STDC_HOSTED__
#define __STDC_HOSTED__ 0
#endif

/* ---- string.h replacements ---- */
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
int   memcmp(const void *s1, const void *s2, size_t n);
size_t strlen(const char *s);
int   strcmp(const char *s1, const char *s2);
int   strncmp(const char *s1, const char *s2, size_t n);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
char *strstr(const char *haystack, const char *needle);

/* ---- stdio.h replacements ---- */
typedef void FILE;
extern FILE *stderr;
extern FILE *stdout;

#define EOF (-1)
#define NULL ((void*)0)

/* These all route to microkit_dbg_puts or are no-ops */
int printf(const char *fmt, ...);
int fprintf(FILE *stream, const char *fmt, ...);
int snprintf(char *str, size_t size, const char *fmt, ...);
int vsnprintf(char *str, size_t size, const char *fmt, va_list ap);
int puts(const char *s);
int fputs(const char *s, FILE *stream);

/* ---- stdlib.h replacements ---- */
/* With d_m3FixedHeap, wasm3 uses its internal bump allocator.
 * But some paths still reference these as fallback. */
void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void  free(void *ptr);
void  abort(void) __attribute__((noreturn));
void  exit(int status) __attribute__((noreturn));
long  strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);

/* ---- assert.h replacement ---- */
#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
#define assert(expr) ((expr) ? ((void)0) : __assert_fail(#expr, __FILE__, __LINE__))
void __assert_fail(const char *expr, const char *file, int line);
#endif

/* ---- math stubs (wasm3 float support) ---- */
double fabs(double x);
float  fabsf(float x);
double sqrt(double x);
float  sqrtf(float x);
double ceil(double x);
double floor(double x);
double trunc(double x);
double rint(double x);
double fmin(double x, double y);
double fmax(double x, double y);
float  fminf(float x, float y);
float  fmaxf(float x, float y);
float  ceilf(float x);
float  floorf(float x);
float  truncf(float x);
float  rintf(float x);
float  sqrtf(float x);

/* Needed by some wasm3 paths */
#define UINT32_MAX  0xFFFFFFFFU
#define INT32_MAX   0x7FFFFFFF
#define INT32_MIN   (-INT32_MAX - 1)
#define UINT64_MAX  0xFFFFFFFFFFFFFFFFULL
#define INT64_MAX   0x7FFFFFFFFFFFFFFFLL
#define INT64_MIN   (-INT64_MAX - 1LL)

/* PRIx format macros that wasm3 uses */
#ifndef PRIi32
#define PRIi32 "d"
#endif
#ifndef PRIi64
#define PRIi64 "lld"
#endif
#ifndef PRIu32
#define PRIu32 "u"
#endif
#ifndef PRIu64
#define PRIu64 "llu"
#endif

#endif /* m3_bare_metal_h */
