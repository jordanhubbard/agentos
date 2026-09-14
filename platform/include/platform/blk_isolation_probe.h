/* Test-only fault probe contract. No production image enables this macro. */
#ifndef AOS_BLK_ISOLATION_PROBE_H
#define AOS_BLK_ISOLATION_PROBE_H
#include <platform/blk_layout.h>
#ifdef AGENTOS_BLK_ISOLATION_PROBE
#if AGENTOS_BLK_ISOLATION_PROBE < 1 || AGENTOS_BLK_ISOLATION_PROBE > 8
#error "block isolation probe must be 1..8"
#endif
#define AOS_BLK_PROBE_BADGE 0xa05u
#define AOS_BLK_PROBE_CLIENT (AGENTOS_BLK_ISOLATION_PROBE > 4 ? 1u : 0u)
#define AOS_BLK_PROBE_OPERATION ((AGENTOS_BLK_ISOLATION_PROBE - 1) % 4)
#define AOS_BLK_PROBE_WRITE ((AGENTOS_BLK_ISOLATION_PROBE % 2) == 0)
#define AOS_BLK_PROBE_ADDRESS \
    (AOS_BLK_SHMEM_VA + (AOS_BLK_PROBE_OPERATION < 2 \
        ? AOS_BLK_CLIENT_BASE + (1u - AOS_BLK_PROBE_CLIENT) * AOS_BLK_CLIENT_STRIDE \
        : AOS_BLK_DISK_OFF))
#endif
#endif
