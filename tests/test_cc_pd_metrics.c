/*
 * test_cc_pd_metrics.c — host tests for the two CC-PD relay metric fixes.
 *
 *   agentos-681  agent_pool_occupancy() reports live busy/idle/faulted so
 *                MSG_CC_LIST_POLECATS is non-zero under agent load.
 *   agentos-vsi  cc_pd log-stream slot allocation: the boot guest owns slot 0
 *                and each vibe guest gets its own addressable slot.
 *
 * The agent_pool half links the REAL services/legacy-pds/agent_pool.c
 * (host-compiled under AGENTOS_TEST_HOST) and drives it through its public
 * spawn/done API, asserting the occupancy snapshot tracks state transitions.
 *
 * The log-slot half exercises a faithful copy of cc_pd's slot-allocation logic
 * (cc_pd.c itself is a PD that cannot host-link the VirtIO/seL4 layer) and
 * asserts allocation, idempotency, boot-slot reservation, and exhaustion.
 *
 * Build:  cc -o /tmp/test_cc_pd_metrics \
 *             tests/test_cc_pd_metrics.c \
 *             services/legacy-pds/agent_pool.c \
 *             -DAGENTOS_TEST_HOST -include tests/microkit.h \
 *             -I tests -I kernel/agentos-root-task/include
 * Run:    /tmp/test_cc_pd_metrics
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "contracts/agent_pool_contract.h"

/* agent_pool.c uses log_drain_write(), which references this setvar extern.
 * Zero means "rings not mapped" so log_drain_write() is a safe no-op on host. */
uintptr_t log_drain_rings_vaddr = 0;

/* TAP output: each test function emits exactly one `ok`/`not ok` line. */
static int g_tap = 0;
#define PASS(name)  do { printf("ok %d - %s\n", ++g_tap, name); return 0; } while(0)
#define FAIL(msg)   do { printf("not ok %d - %s:%d: %s\n", ++g_tap, __FILE__, __LINE__, msg); return 1; } while(0)
#define CHECK(cond) do { if (!(cond)) FAIL(#cond); } while(0)

/* ── Real agent_pool.c public surface (compiled into this binary) ── */
void agent_pool_init(void);
int  agent_pool_spawn(const char *agent_name, uint64_t task_id,
                      const uint8_t *payload, uint32_t payload_len,
                      uint32_t priority);
void agent_pool_worker_done(int slot, int status);
/* agent_pool_occupancy() is declared in agent_pool_contract.h */

/* ══════════════════════════════════════════════════════════════════════════
 * agentos-681: live polecat (agent worker) occupancy
 * ══════════════════════════════════════════════════════════════════════════ */

static int test_pool_starts_idle(void)
{
    agent_pool_init();
    uint32_t total = 0, busy = 99, idle = 0, faulted = 99;
    agent_pool_occupancy(&total, &busy, &idle, &faulted);

    CHECK(total == 8u);          /* WORKER_POOL_SIZE */
    CHECK(busy == 0u);
    CHECK(idle == total);        /* all idle at init */
    CHECK(faulted == 0u);
    CHECK(busy + idle + faulted == total);  /* invariant */
    PASS("pool_starts_idle");
}

static int test_pool_busy_under_load(void)
{
    agent_pool_init();

    /* Spawn three agents — busy must become non-zero (the agentos-681 bug). */
    int s0 = agent_pool_spawn("alpha", 0, NULL, 0u, 0u);
    int s1 = agent_pool_spawn("beta",  0, NULL, 0u, 0u);
    int s2 = agent_pool_spawn("gamma", 0, NULL, 0u, 0u);
    CHECK(s0 >= 0 && s1 >= 0 && s2 >= 0);

    uint32_t total = 0, busy = 0, idle = 0, faulted = 0;
    agent_pool_occupancy(&total, &busy, &idle, &faulted);
    CHECK(busy == 3u);                       /* non-zero under load */
    CHECK(idle == total - 3u);
    CHECK(busy + idle + faulted == total);

    /* Release one worker — busy must drop, idle must rise. */
    agent_pool_worker_done(s1, 0);
    agent_pool_occupancy(&total, &busy, &idle, &faulted);
    CHECK(busy == 2u);
    CHECK(idle == total - 2u);
    CHECK(busy + idle + faulted == total);
    PASS("pool_busy_under_load");
}

static int test_pool_occupancy_null_safe(void)
{
    agent_pool_init();
    (void)agent_pool_spawn("solo", 0, NULL, 0u, 0u);

    /* Each out pointer is independently optional. */
    uint32_t busy = 0;
    agent_pool_occupancy(NULL, &busy, NULL, NULL);
    CHECK(busy == 1u);
    agent_pool_occupancy(NULL, NULL, NULL, NULL);  /* must not crash */
    PASS("pool_occupancy_null_safe");
}

/* ══════════════════════════════════════════════════════════════════════════
 * agentos-vsi: log-stream slot allocation
 *
 * Mirror of cc_pd.c's g_log_slots[] / cc_log_slot_for_handle().  Kept in sync
 * with the implementation; this test exists to lock the slot semantics.
 * ══════════════════════════════════════════════════════════════════════════ */

#define CC_LOG_SLOTS          8u
#define CC_LOG_SLOT_BOOT      0u
#define CC_LOG_SLOT_INVALID   0xFFFFFFFFu

typedef struct {
    bool     in_use;
    uint32_t guest_handle;
    uint32_t pd_id;
} cc_log_slot_t;

static cc_log_slot_t g_log_slots[CC_LOG_SLOTS];

static void cc_log_slots_reset(void)
{
    memset(g_log_slots, 0, sizeof(g_log_slots));
}

static uint32_t cc_log_slot_for_handle(uint32_t guest_handle, uint32_t pd_id)
{
    for (uint32_t i = 1u; i < CC_LOG_SLOTS; i++) {
        if (g_log_slots[i].in_use &&
            g_log_slots[i].guest_handle == guest_handle) {
            return i;
        }
    }
    for (uint32_t i = 1u; i < CC_LOG_SLOTS; i++) {
        if (!g_log_slots[i].in_use) {
            g_log_slots[i].in_use       = true;
            g_log_slots[i].guest_handle = guest_handle;
            g_log_slots[i].pd_id        = pd_id;
            return i;
        }
    }
    return CC_LOG_SLOT_INVALID;
}

static int test_log_slot_boot_reserved(void)
{
    cc_log_slots_reset();
    /* Allocation never hands out slot 0; it is the boot guest's. */
    uint32_t a = cc_log_slot_for_handle(100u, 0u);
    uint32_t b = cc_log_slot_for_handle(101u, 0u);
    CHECK(a != CC_LOG_SLOT_BOOT);
    CHECK(b != CC_LOG_SLOT_BOOT);
    CHECK(a >= 1u && a < CC_LOG_SLOTS);
    CHECK(b >= 1u && b < CC_LOG_SLOTS);
    CHECK(a != b);                           /* distinct guests, distinct slots */
    PASS("log_slot_boot_reserved");
}

static int test_log_slot_idempotent(void)
{
    cc_log_slots_reset();
    uint32_t first  = cc_log_slot_for_handle(42u, 0u);
    uint32_t second = cc_log_slot_for_handle(42u, 0u);
    CHECK(first != CC_LOG_SLOT_INVALID);
    CHECK(first == second);                  /* same handle → same slot */
    CHECK(g_log_slots[first].guest_handle == 42u);
    PASS("log_slot_idempotent");
}

static int test_log_slot_exhaustion(void)
{
    cc_log_slots_reset();
    /* Slots 1..7 are allocatable (slot 0 reserved) → exactly 7 guests fit. */
    uint32_t assigned = 0;
    for (uint32_t h = 0; h < CC_LOG_SLOTS - 1u; h++) {
        uint32_t s = cc_log_slot_for_handle(1000u + h, 0u);
        CHECK(s != CC_LOG_SLOT_INVALID);
        assigned++;
    }
    CHECK(assigned == CC_LOG_SLOTS - 1u);
    /* One more distinct guest must fail (table full). */
    CHECK(cc_log_slot_for_handle(9999u, 0u) == CC_LOG_SLOT_INVALID);
    /* But a known handle still resolves even when full. */
    CHECK(cc_log_slot_for_handle(1000u, 0u) != CC_LOG_SLOT_INVALID);
    PASS("log_slot_exhaustion");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Main
 * ══════════════════════════════════════════════════════════════════════════ */

typedef int (*test_fn)(void);

static const test_fn tests[] = {
    test_pool_starts_idle,
    test_pool_busy_under_load,
    test_pool_occupancy_null_safe,
    test_log_slot_boot_reserved,
    test_log_slot_idempotent,
    test_log_slot_exhaustion,
};

int main(void)
{
    printf("# cc_pd metrics tests (agentos-681 / agentos-vsi)\n");
    int failed = 0;
    size_t n = sizeof(tests) / sizeof(tests[0]);
    printf("1..%zu\n", n);
    for (size_t i = 0; i < n; i++)
        failed += tests[i]();
    printf("# %s (%zu/%zu passed)\n",
           failed == 0 ? "ALL PASS" : "FAILURES DETECTED",
           n - (size_t)failed, n);
    return failed == 0 ? 0 : 1;
}
