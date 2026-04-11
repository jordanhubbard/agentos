/*
 * Bare-metal inttypes.h stub for agentOS/seL4 freestanding build.
 */
#ifndef _INTTYPES_H
#define _INTTYPES_H

#include <stdint.h>

/* printf format macros (wasm3 uses PRIi32 etc. for debug output) */
#ifndef PRId8
#define PRId8   "d"
#endif
#ifndef PRId16
#define PRId16  "d"
#endif
#ifndef PRId32
#define PRId32  "d"
#endif
#ifndef PRId64
#define PRId64  "ld"
#endif
#ifndef PRIi8
#define PRIi8   "i"
#endif
#ifndef PRIi16
#define PRIi16  "i"
#endif
#ifndef PRIi32
#define PRIi32  "i"
#endif
#ifndef PRIi64
#define PRIi64  "li"
#endif
#ifndef PRIu8
#define PRIu8   "u"
#endif
#ifndef PRIu16
#define PRIu16  "u"
#endif
#ifndef PRIu32
#define PRIu32  "u"
#endif
#ifndef PRIu64
#define PRIu64  "lu"
#endif
#ifndef PRIx8
#define PRIx8   "x"
#endif
#ifndef PRIx16
#define PRIx16  "x"
#endif
#ifndef PRIx32
#define PRIx32  "x"
#endif
#ifndef PRIx64
#define PRIx64  "lx"
#endif
#ifndef PRIX8
#define PRIX8   "X"
#endif
#ifndef PRIX16
#define PRIX16  "X"
#endif
#ifndef PRIX32
#define PRIX32  "X"
#endif
#ifndef PRIX64
#define PRIX64  "lX"
#endif
#ifndef PRIp
#define PRIp    "p"
#endif

/* Size-specific */
#ifndef PRIdPTR
#define PRIdPTR "ld"
#endif
#ifndef PRIiPTR
#define PRIiPTR "li"
#endif
#ifndef PRIuPTR
#define PRIuPTR "lu"
#endif
#ifndef PRIxPTR
#define PRIxPTR "lx"
#endif

#endif /* _INTTYPES_H */
