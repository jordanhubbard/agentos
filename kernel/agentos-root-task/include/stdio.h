/*
 * Bare-metal stdio.h stub for agentOS/seL4 freestanding build.
 * wasm3 uses printf/snprintf for debug output (overridden in m3_config).
 */
#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>
#include <stdarg.h>

/* Standard I/O (wasm3 debug paths — output goes to /dev/null on bare metal) */
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int fprintf(void *stream, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* EOF / NULL stream */
#define EOF  (-1)
#define NULL ((void *)0)

typedef struct _FILE FILE;
extern FILE *stderr;
extern FILE *stdout;
extern FILE *stdin;

#endif /* _STDIO_H */
