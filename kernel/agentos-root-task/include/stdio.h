/*
 * Bare-metal stdio.h stub for agentOS/seL4 freestanding build.
 * wasm3 uses printf/snprintf for debug output (overridden in m3_config).
 *
 * Under AGENTOS_TEST_HOST (host-compiled unit / integration tests) we
 * delegate to the real system <stdio.h> so that printf / snprintf link
 * against the host libc rather than these bare-metal stubs.
 */
#ifdef AGENTOS_TEST_HOST
/* Pull in the real system stdio.h via the compiler's built-in search path.
 * We undefine _STDIO_H first so the real header's guard does not collide. */
#undef  _STDIO_H
#include_next <stdio.h>
#else /* !AGENTOS_TEST_HOST — bare-metal freestanding build */

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
#ifndef NULL
#define NULL ((void *)0)
#endif

typedef struct _FILE FILE;
extern FILE *stderr;
extern FILE *stdout;
extern FILE *stdin;

#endif /* _STDIO_H */
#endif /* AGENTOS_TEST_HOST */
