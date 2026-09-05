#include <stdio.h>
#include <string.h>

#include "cc_retry_cache.h"

static int check(int condition, const char *name)
{
    printf("%s - %s\n", condition ? "ok" : "not ok", name);
    return condition ? 0 : 1;
}

int main(void)
{
    int failed = 0;
    cc_retry_cache_t cache;
    unsigned char request[CC_RETRY_FRAME_SIZE] = {0};
    unsigned char other[CC_RETRY_FRAME_SIZE] = {0};
    unsigned char reply[CC_RETRY_FRAME_SIZE] = {0};
    unsigned char replay[CC_RETRY_FRAME_SIZE] = {0};

    request[0] = 0x2au;
    reply[0] = 0x7bu;
    other[0] = 0x2bu;

    cc_retry_cache_init(&cache);
    failed += check(!cc_retry_cache_replay(&cache, request, replay),
                    "empty cache never replays");

    cc_retry_cache_record(&cache, request, reply);
    failed += check(cc_retry_cache_replay(&cache, request, replay) &&
                    memcmp(reply, replay, sizeof(reply)) == 0,
                    "exact reconnect retry receives saved reply");
    failed += check(!cc_retry_cache_replay(&cache, request, replay),
                    "saved reply is consumed once");

    cc_retry_cache_record(&cache, request, reply);
    failed += check(!cc_retry_cache_replay(&cache, other, replay) &&
                    !cc_retry_cache_replay(&cache, request, replay),
                    "unrelated next request invalidates saved reply");

    printf("1..4\n");
    return failed == 0 ? 0 : 1;
}
