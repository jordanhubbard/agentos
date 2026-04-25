/*
 * Bare-metal assert.h stub for agentOS/seL4 freestanding build.
 *
 * When AGENTOS_TEST_HOST is defined (host-side unit tests), provide an
 * inline implementation that calls abort() so tests link on both macOS
 * (no __assert_fail) and Linux.  Bare-metal seL4 builds keep the
 * forward declaration only; the implementation lives in the root task.
 */
#ifndef _ASSERT_H
#define _ASSERT_H

#ifdef AGENTOS_TEST_HOST
#include <stdio.h>
#include <stdlib.h>
static inline __attribute__((noreturn)) void
__assert_fail(const char *expr, const char *file, int line,
              const char *function)
{
    fprintf(stderr, "%s:%d: %s: Assertion `%s' failed.\n",
            file, line, function, expr);
    abort();
}
#else
void __assert_fail(const char *expr, const char *file, int line,
                   const char *function);
#endif

#ifdef NDEBUG
#  define assert(expr) ((void)0)
#else
#  define assert(expr) \
    ((expr) ? ((void)0) : __assert_fail(#expr, __FILE__, __LINE__, __func__))
#endif

#endif /* _ASSERT_H */
