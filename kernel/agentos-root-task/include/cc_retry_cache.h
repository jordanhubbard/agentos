/*
 * One-entry replay cache for the sequential CC host transport.
 *
 * A VirtIO TX timeout can occur after a state-changing request was dispatched
 * but before its reply reached the host. The reconnecting host repeats that
 * exact frame. Replay the saved reply once instead of executing the operation
 * twice. Unrelated next requests invalidate the pending entry.
 */
#ifndef AGENTOS_CC_RETRY_CACHE_H
#define AGENTOS_CC_RETRY_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CC_RETRY_FRAME_SIZE 4112u

typedef struct cc_retry_cache {
    uint8_t request[CC_RETRY_FRAME_SIZE];
    uint8_t reply[CC_RETRY_FRAME_SIZE];
    bool pending;
} cc_retry_cache_t;

void cc_retry_cache_init(cc_retry_cache_t *cache);
void cc_retry_cache_record(cc_retry_cache_t *cache,
                           const void *request, const void *reply);
bool cc_retry_cache_replay(cc_retry_cache_t *cache,
                           const void *request, void *reply);

#endif /* AGENTOS_CC_RETRY_CACHE_H */
