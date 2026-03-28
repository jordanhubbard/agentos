/*
 * Bare-metal inttypes.h stub for agentOS/seL4 freestanding build.
 */
#ifndef _INTTYPES_H
#define _INTTYPES_H

#include <stdint.h>

/* printf format macros (wasm3 uses PRIi32 etc. for debug output) */
#define PRId8   "d"
#define PRId16  "d"
#define PRId32  "d"
#define PRId64  "ld"
#define PRIi8   "i"
#define PRIi16  "i"
#define PRIi32  "i"
#define PRIi64  "li"
#define PRIu8   "u"
#define PRIu16  "u"
#define PRIu32  "u"
#define PRIu64  "lu"
#define PRIx8   "x"
#define PRIx16  "x"
#define PRIx32  "x"
#define PRIx64  "lx"
#define PRIX8   "X"
#define PRIX16  "X"
#define PRIX32  "X"
#define PRIX64  "lX"
#define PRIp    "p"

/* Size-specific */
#define PRIdPTR "ld"
#define PRIiPTR "li"
#define PRIuPTR "lu"
#define PRIxPTR "lx"

#endif /* _INTTYPES_H */
