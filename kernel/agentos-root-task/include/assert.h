/*
 * Bare-metal assert.h stub for agentOS/seL4 freestanding build.
 */
#ifndef _ASSERT_H
#define _ASSERT_H

void __assert_fail(const char *expr, const char *file, int line, const char *function);

#ifdef NDEBUG
#  define assert(expr) ((void)0)
#else
#  define assert(expr) \
    ((expr) ? ((void)0) : __assert_fail(#expr, __FILE__, __LINE__, __func__))
#endif

#endif /* _ASSERT_H */
