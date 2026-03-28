/*
 * Bare-metal stdlib.h stub for agentOS/seL4 freestanding build.
 * Provides only what wasm3 and agentfs actually use.
 */
#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

/* wasm3 uses these via its memory allocator hooks (overridden in m3_config) */
void *malloc(size_t size);
void  free(void *ptr);
void *realloc(void *ptr, size_t size);
void *calloc(size_t nmemb, size_t size);

/* Abort/exit (wasm3 trap paths) */
void abort(void) __attribute__((noreturn));
void exit(int status) __attribute__((noreturn));

/* Misc used by wasm3 */
int                abs(int x);
long               labs(long x);
unsigned long long strtoull(const char *str, char **endptr, int base);
long long          strtoll(const char *str, char **endptr, int base);
unsigned long      strtoul(const char *str, char **endptr, int base);
long               strtol(const char *str, char **endptr, int base);
double             strtod(const char *str, char **endptr);
float              strtof(const char *str, char **endptr);

#endif /* _STDLIB_H */
