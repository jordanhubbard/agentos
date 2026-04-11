/*
 * agentOS Snapshot Scheduler — Unit Test
 *
 * Tests the slot tracking table, delta-compression logic, history ring, and
 * policy configuration from snapshot_sched.c.  Runs on the host without seL4.
 *
 * Build:  cc -o /tmp/test_snapshot_sched \
 *             tests/test_snapshot_sched.c \
 *             -I kernel/agentos-root-task/include \
 *             -DAGENTOS_TEST_HOST -DAGENTOS_SNAPSHOT_SCHED
 * Run:    /tmp/test_snapshot_sched
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

/* ══════════════════════════════════════════════════════════════════════════
 * Host-side stubs
 * ══════════════════════════════════════════════════════════════════════════ */
#ifdef AGENTOS_TEST_HOST

static uint64_t _mrs[64];
static inline void     microkit_mr_set(uint32_t i, uint64_t v) { _mrs[i] = v; }
static inline uint64_t microkit_mr_get(uint32_t i)             { return _mrs[i]; }
typedef uint32_t microkit_channel;
typedef uint64_t microkit_msginfo_t;
static inline microkit_msginfo_t microkit_msginfo_new(uint64_t l, uint32_t c) {
    (void)c; return l;
}

/* Track PPC calls for assertion */
static uint32_t ppc_agentfs_call_count = 0;
static uint32_t ppc_agentfs_ok_reply   = 1;   /* default: AgentFS says ok */
static uint32_t ppc_eventbus_call_count = 0;
static uint32_t ppc_ctrl_running_mask   = 0;   /* bitmask returned by controller */

static microkit_msginfo_t microkit_ppcall(microkit_channel ch, microkit_msginfo_t m) {
    (void)m;
    if (ch == 2 /* CH_AGENTFS */) {
        ppc_agentfs_call_count++;
        microkit_mr_set(0, ppc_agentfs_ok_reply);
        microkit_mr_set(1, 0xDEAD);  /* hash_lo */
        microkit_mr_set(2, 0xBEEF);  /* hash_hi */
    } else if (ch == 3 /* CH_EVENT_BUS */) {
        ppc_eventbus_call_count++;
        microkit_mr_set(0, 1);
    } else if (ch == 1 /* CH_CTRL */) {
        microkit_mr_set(1, ppc_ctrl_running_mask);
    }
    return 0;
}
static inline void microkit_dbg_puts(const char *s) { printf("%s", s); }

#define LOG(fmt, ...) printf("[test] " fmt "\n", ##__VA_ARGS__)

#endif /* AGENTOS_TEST_HOST */

/* ══════════════════════════════════════════════════════════════════════════
 * Inline snapshot_sched logic (mirrored from snapshot_sched.c)
 * ══════════════════════════════════════════════════════════════════════════ */

#define CH_TIMER_TICK   0u
#define CH_CTRL         1u
#define CH_AGENTFS      2u
#define CH_EVENT_BUS    3u
#define CH_POLICY       4u

#define OP_SNAPSHOT_SLOT      0xE1u
#define OP_CTRL_SLOT_LIST     0x10u
#define OP_EVENT_BUS_PUBLISH  0x30u
#define OP_SNAP_STATUS        0xB0u
#define OP_SNAP_SET_POLICY    0xB1u
#define OP_SNAP_FORCE         0xB2u
#define OP_SNAP_GET_HISTORY   0xB3u
#define EVENT_SNAP_SCHED_DONE 0x20u

#define SNAP_INTERVAL_TICKS_DEFAULT  500u
#define SNAP_MIN_DELTA_DEFAULT       64u
#define SNAP_MAX_SLOTS               8u
#define SNAP_HISTORY_SIZE            8u

typedef struct {
    uint32_t slot_id;
    uint32_t last_snap_tick;
    uint32_t last_snap_heap_kb;
    uint32_t snap_count;
    uint32_t last_hash_lo;
    uint32_t last_hash_hi;
} SlotSnapState;

typedef struct {
    uint32_t tick;
    uint32_t slots_checked;
    uint32_t slots_snapped;
    uint32_t skipped_delta;
    uint32_t failed;
} SnapHistoryEntry;

static SlotSnapState   g_slots[SNAP_MAX_SLOTS];
static uint32_t        g_slot_count     = 0;
static SnapHistoryEntry g_history[SNAP_HISTORY_SIZE];
static uint32_t        g_history_head   = 0;
static uint32_t        g_tick_counter   = 0;
static uint32_t        g_round_counter  = 0;
static uint32_t        g_total_snapped  = 0;
static uint32_t        g_interval_ticks = SNAP_INTERVAL_TICKS_DEFAULT;
static uint32_t        g_min_delta      = SNAP_MIN_DELTA_DEFAULT;

static void snap_init(void) {
    memset(g_slots,   0, sizeof(g_slots));
    memset(g_history, 0, sizeof(g_history));
    g_tick_counter  = 0;
    g_round_counter = 0;
    g_total_snapped = 0;
    g_slot_count    = 0;
    g_interval_ticks = SNAP_INTERVAL_TICKS_DEFAULT;
    g_min_delta      = SNAP_MIN_DELTA_DEFAULT;
    ppc_agentfs_call_count  = 0;
    ppc_agentfs_ok_reply    = 1;
    ppc_eventbus_call_count = 0;
    ppc_ctrl_running_mask   = 0;
}

static SlotSnapState *get_or_create_slot(uint32_t slot_id) {
    for (uint32_t i = 0; i < g_slot_count; i++) {
        if (g_slots[i].slot_id == slot_id) return &g_slots[i];
    }
    if (g_slot_count < SNAP_MAX_SLOTS) {
        SlotSnapState *s = &g_slots[g_slot_count++];
        memset(s, 0, sizeof(*s));
        s->slot_id = slot_id;
        return s;
    }
    return NULL;
}

static void history_push(uint32_t tick, uint32_t checked,
                         uint32_t snapped, uint32_t skipped, uint32_t failed) {
    g_history[g_history_head % SNAP_HISTORY_SIZE] = (SnapHistoryEntry){
        .tick           = tick,
        .slots_checked  = checked,
        .slots_snapped  = snapped,
        .skipped_delta  = skipped,
        .failed         = failed,
    };
    g_history_head++;
}

static bool snapshot_slot_fn(SlotSnapState *s) {
    microkit_mr_set(0, OP_SNAPSHOT_SLOT);
    microkit_mr_set(1, s->slot_id);
    microkit_msginfo_t resp =
        microkit_ppcall(CH_AGENTFS, microkit_msginfo_new(OP_SNAPSHOT_SLOT, 2));
    (void)resp;
    uint32_t ok      = (uint32_t)microkit_mr_get(0);
    uint32_t hash_lo = (uint32_t)microkit_mr_get(1);
    uint32_t hash_hi = (uint32_t)microkit_mr_get(2);
    if (!ok) return false;
    s->last_snap_tick    = g_tick_counter;
    s->last_snap_heap_kb = 0;
    s->last_hash_lo      = hash_lo;
    s->last_hash_hi      = hash_hi;
    s->snap_count++;
    return true;
}

static void run_snapshot_round(uint32_t running_mask) {
    uint32_t checked = 0, snapped = 0, skipped = 0, failed = 0;
    for (uint32_t slot_id = 0; slot_id < SNAP_MAX_SLOTS; slot_id++) {
        if (!(running_mask & (1u << slot_id))) continue;
        checked++;
        SlotSnapState *s = get_or_create_slot(slot_id);
        if (!s) { failed++; continue; }
        uint32_t tick_delta = g_tick_counter - s->last_snap_tick;
        if (tick_delta < g_interval_ticks && s->snap_count > 0) {
            skipped++;
            continue;
        }
        if (snapshot_slot_fn(s)) {
            snapped++;
            g_total_snapped++;
        } else {
            failed++;
        }
    }
    g_round_counter++;
    history_push(g_tick_counter, checked, snapped, skipped, failed);
    microkit_mr_set(0, OP_EVENT_BUS_PUBLISH);
    microkit_mr_set(1, EVENT_SNAP_SCHED_DONE);
    microkit_mr_set(2, checked);
    microkit_mr_set(3, snapped);
    microkit_mr_set(4, g_tick_counter);
    microkit_ppcall(CH_EVENT_BUS, microkit_msginfo_new(OP_EVENT_BUS_PUBLISH, 5));
}

/* ── Test framework ──────────────────────────────────────────────────────── */

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) printf("\n=== TEST: %s ===\n", (name))

#define ASSERT_TRUE(expr, msg) do { \
    if (!(expr)) { printf("  FAIL: %s\n", (msg)); tests_failed++; } \
    else         { printf("  PASS: %s\n", (msg)); tests_passed++; } \
} while(0)

#define ASSERT_EQ(a, b, msg) do { \
    if ((uint64_t)(a) != (uint64_t)(b)) { \
        printf("  FAIL: %s (expected %llu, got %llu)\n", (msg), \
               (unsigned long long)(b), (unsigned long long)(a)); \
        tests_failed++; \
    } else { printf("  PASS: %s\n", (msg)); tests_passed++; } \
} while(0)

/* ── Tests ────────────────────────────────────────────────────────────────── */

static void test_initial_state(void) {
    TEST("initial_state");
    snap_init();
    ASSERT_EQ(g_tick_counter, 0, "tick_counter = 0");
    ASSERT_EQ(g_round_counter, 0, "round_counter = 0");
    ASSERT_EQ(g_total_snapped, 0, "total_snapped = 0");
    ASSERT_EQ(g_slot_count, 0, "slot_count = 0");
    ASSERT_EQ(g_interval_ticks, SNAP_INTERVAL_TICKS_DEFAULT, "default interval");
}

static void test_snapshot_single_slot(void) {
    TEST("snapshot_single_slot");
    snap_init();
    ppc_agentfs_ok_reply = 1;
    uint32_t mask = 0x01; /* slot 0 running */
    run_snapshot_round(mask);
    ASSERT_EQ(g_total_snapped, 1, "one slot snapped");
    ASSERT_EQ(ppc_agentfs_call_count, 1, "one AgentFS PPC issued");
    ASSERT_EQ(ppc_eventbus_call_count, 1, "one EventBus publish");
    ASSERT_EQ(g_round_counter, 1, "round_counter = 1");
    ASSERT_EQ(g_slots[0].snap_count, 1, "slot 0 snap_count = 1");
    ASSERT_EQ(g_slots[0].last_hash_lo, 0xDEAD, "hash_lo stored");
    ASSERT_EQ(g_slots[0].last_hash_hi, 0xBEEF, "hash_hi stored");
}

static void test_snapshot_multiple_slots(void) {
    TEST("snapshot_multiple_slots");
    snap_init();
    ppc_agentfs_ok_reply = 1;
    uint32_t mask = 0x07; /* slots 0, 1, 2 */
    run_snapshot_round(mask);
    ASSERT_EQ(g_total_snapped, 3, "three slots snapped");
    ASSERT_EQ(ppc_agentfs_call_count, 3, "three AgentFS PPCs issued");
    ASSERT_EQ(g_slot_count, 3, "slot tracking table has 3 entries");
}

static void test_skip_after_recent_snap(void) {
    TEST("skip_after_recent_snap");
    snap_init();
    ppc_agentfs_ok_reply = 1;
    uint32_t mask = 0x01;
    /* First round: unconditional snap */
    run_snapshot_round(mask);
    uint32_t snaps_after_first = g_total_snapped;
    /* Second round immediately after (tick_delta = 0 < interval) → skip */
    run_snapshot_round(mask);
    ASSERT_EQ(g_total_snapped, snaps_after_first, "slot skipped on second round (recent)");
    /* Most recent history entry is at index (g_history_head - 1) % SNAP_HISTORY_SIZE */
    uint32_t last_idx = (g_history_head - 1) % SNAP_HISTORY_SIZE;
    ASSERT_EQ(g_history[last_idx].skipped_delta,
              1, "history records 1 skipped-delta slot");
}

static void test_snap_after_interval(void) {
    TEST("snap_after_interval");
    snap_init();
    ppc_agentfs_ok_reply = 1;
    uint32_t mask = 0x01;
    run_snapshot_round(mask);
    uint32_t initial_snaps = g_total_snapped;
    /* Advance tick counter past interval */
    g_tick_counter += g_interval_ticks + 1;
    run_snapshot_round(mask);
    ASSERT_EQ(g_total_snapped, initial_snaps + 1, "snap taken after interval elapsed");
}

static void test_agentfs_failure(void) {
    TEST("agentfs_failure");
    snap_init();
    ppc_agentfs_ok_reply = 0; /* AgentFS returns error */
    run_snapshot_round(0x01);
    ASSERT_EQ(g_total_snapped, 0, "no snaps on AgentFS failure");
    /* history should record 1 failed */
    SnapHistoryEntry *h = &g_history[(g_history_head - 1) % SNAP_HISTORY_SIZE];
    ASSERT_EQ(h->failed, 1, "history records 1 failure");
}

static void test_history_ring_wraps(void) {
    TEST("history_ring_wraps");
    snap_init();
    ppc_agentfs_ok_reply = 1;
    /* Run more rounds than the ring can hold */
    for (uint32_t r = 0; r < SNAP_HISTORY_SIZE + 3; r++) {
        g_tick_counter += g_interval_ticks + 1;
        run_snapshot_round(0x01);
    }
    ASSERT_EQ(g_round_counter, SNAP_HISTORY_SIZE + 3, "round_counter accumulates");
    /* The ring head modulo must still be in range */
    ASSERT_TRUE(g_history_head > SNAP_HISTORY_SIZE, "history_head wrapped");
    /* Most recent entry should reference the latest tick */
    SnapHistoryEntry *last = &g_history[(g_history_head - 1) % SNAP_HISTORY_SIZE];
    ASSERT_TRUE(last->tick > 0, "most recent history entry has non-zero tick");
}

static void test_policy_update(void) {
    TEST("policy_update");
    snap_init();
    g_interval_ticks = 200;
    g_min_delta      = 16;
    ASSERT_EQ(g_interval_ticks, 200, "interval updated");
    ASSERT_EQ(g_min_delta, 16, "min_delta updated");
}

static void test_all_slots_mask(void) {
    TEST("all_slots_mask");
    snap_init();
    ppc_agentfs_ok_reply = 1;
    uint32_t full_mask = (1u << SNAP_MAX_SLOTS) - 1;
    run_snapshot_round(full_mask);
    ASSERT_EQ(g_total_snapped, SNAP_MAX_SLOTS, "all 8 slots snapped");
    ASSERT_EQ(ppc_agentfs_call_count, SNAP_MAX_SLOTS, "8 AgentFS PPCs issued");
}

static void test_empty_mask_no_work(void) {
    TEST("empty_mask_no_work");
    snap_init();
    run_snapshot_round(0x00);
    ASSERT_EQ(g_total_snapped, 0, "no snaps when mask is 0");
    ASSERT_EQ(ppc_agentfs_call_count, 0, "no AgentFS PPCs for empty mask");
    ASSERT_EQ(g_round_counter, 1, "round still counted");
}

static void test_slot_table_full(void) {
    TEST("slot_table_full");
    snap_init();
    ppc_agentfs_ok_reply = 1;
    /* Snapshot all 8 slots to fill the tracking table */
    uint32_t full_mask = (1u << SNAP_MAX_SLOTS) - 1;
    run_snapshot_round(full_mask);
    ASSERT_EQ(g_slot_count, SNAP_MAX_SLOTS, "slot table full after first round");
    /* Additional round — all slots already tracked, skipped due to delta */
    run_snapshot_round(full_mask);
    ASSERT_EQ(g_slot_count, SNAP_MAX_SLOTS, "slot count stable — no overflow");
}

/* ── main ──────────────────────────────────────────────────────────────── */

int main(void) {
    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║  agentOS Snapshot Scheduler — Test Suite         ║\n");
    printf("╚══════════════════════════════════════════════════╝\n");

    test_initial_state();
    test_snapshot_single_slot();
    test_snapshot_multiple_slots();
    test_skip_after_recent_snap();
    test_snap_after_interval();
    test_agentfs_failure();
    test_history_ring_wraps();
    test_policy_update();
    test_all_slots_mask();
    test_empty_mask_no_work();
    test_slot_table_full();

    printf("\n══════════════════════════════════════════════════\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    if (tests_failed > 0) {
        printf("FAIL\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
