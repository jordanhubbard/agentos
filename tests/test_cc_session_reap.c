/* Host test: a live session must never be evicted to serve a new connect.
 *
 * Models the session table and the reap rule. The rule under test is the
 * predicate reap_oldest_session uses to choose a victim. */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "contracts/cc_contract.h"

typedef struct {
    bool     active;
    uint32_t state;
    uint32_t ticks_since_active;
} model_session_t;

static model_session_t g[CC_MAX_SESSIONS];

/* Mirrors the corrected rule: only a session already marked EXPIRED is a
 * candidate. Age alone is not sufficient. */
static int reap_candidate(void)
{
    int victim = -1;
    uint32_t oldest = 0u;
    for (int i = 0; i < (int)CC_MAX_SESSIONS; i++) {
        if (!g[i].active) continue;
        if (g[i].state != CC_SESSION_STATE_EXPIRED) continue;
        if (g[i].ticks_since_active >= oldest) {
            oldest = g[i].ticks_since_active;
            victim = i;
        }
    }
    return victim;
}

int main(void)
{
    /* Table full of LIVE sessions, all heavily aged. No victim may be
     * chosen — a new connect must be refused, not served by eviction. */
    for (int i = 0; i < (int)CC_MAX_SESSIONS; i++) {
        g[i].active = true;
        g[i].state = CC_SESSION_STATE_CONNECTED;
        g[i].ticks_since_active = 1000u + (uint32_t)i;
    }
    assert(reap_candidate() == -1);

    /* One session expires: it becomes the victim even though it is not the
     * oldest. */
    g[2].state = CC_SESSION_STATE_EXPIRED;
    g[2].ticks_since_active = 1u;
    assert(reap_candidate() == 2);

    /* Two expired: the older one is chosen. */
    g[5].state = CC_SESSION_STATE_EXPIRED;
    g[5].ticks_since_active = 9u;
    assert(reap_candidate() == 5);

    printf("test_cc_session_reap: PASS\n");
    return 0;
}
