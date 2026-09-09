#include "cc_retry_cache.h"

static void copy_frame(uint8_t *dst, const uint8_t *src)
{
    for (size_t i = 0u; i < CC_RETRY_FRAME_SIZE; i++) dst[i] = src[i];
}

void cc_retry_cache_init(cc_retry_cache_t *cache)
{
    if (cache != NULL) cache->pending = false;
}

void cc_retry_cache_record(cc_retry_cache_t *cache,
                           const void *request, const void *reply)
{
    if (cache == NULL || request == NULL || reply == NULL) return;
    copy_frame(cache->request, request);
    copy_frame(cache->reply, reply);
    cache->pending = true;
}

bool cc_retry_cache_replay(cc_retry_cache_t *cache,
                           const void *request, void *reply)
{
    bool equal = true;
    const uint8_t *candidate = request;

    if (cache == NULL || request == NULL || reply == NULL || !cache->pending) {
        return false;
    }
    for (size_t i = 0u; i < CC_RETRY_FRAME_SIZE; i++) {
        if (cache->request[i] != candidate[i]) {
            equal = false;
            break;
        }
    }
    if (equal) copy_frame(reply, cache->reply);
    cache->pending = false;
    return equal;
}
