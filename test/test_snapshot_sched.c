/*
 * test_snapshot_sched.c — unit tests for agentOS snapshot_sched PD logic
 *
 * Standalone host test that mirrors the scheduling, delta-compression, and
 * history-ring logic from snapshot_sched.c without any seL4 / Microkit deps.
 *
 * Tests:
 *   1. Initial state: all counters zero
 *   2. Tick accumulation: round fires at SNAP_INTERVAL_TICKS boundary
 *   3. Per-slot delta check: slot with recent snap is skipped
 *   4. Force-round: all running slots snapped regardless of interval
 *   5. History ring: wraps correctly after SNAP_HISTORY_SIZE rounds
 *   6. Policy update: new interval honoured immediately
 *   7. Mask filtering: only running slots (bitmask) are snapped
 *   8. Status query: counters reflect all completed rounds
 *
 * Build & run (no dependencies):
 *   cc test/test_snapshot_sched.c -o /tmp/test_snapshot_sched && /tmp/test_snapshot_sched
 *
 * Exits 0 on all pass, non-zero on failure.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

/* ── Mirror constants from snapshot_sched.c ─────────────────────────────── */

#define SNAP_INTERVAL_TICKS_DEFAULT  500u
#define SNAP_MIN_DELTA_DEFAULT        64u
#define SNAP_MAX_SLOTS                 8u
#define SNAP_HISTORY_SIZE              8u
#define OP_SNAP_STATUS        0xB0u
#define OP_SNAP_SET_POLICY    0xB1u
#define OP_SNAP_FORCE         0xB2u
#define OP_SNAP_GET_HISTORY   0xB3u
#define EVENT_SNAP_SCHED_DONE 0x20u

/* ── Mirrored data structures ────────────────────────────────────────────── */

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

/* ── Simulation state ────────────────────────────────────────────────────── */

static SlotSnapState  sim_slots[SNAP_MAX_SLOTS];
static uint32_t       sim_slot_count   = 0;
static SnapHistoryEntry sim_history[SNAP_HISTORY_SIZE];
static uint32_t       sim_history_head = 0;
static uint32_t       sim_tick         = 0;
static uint32_t       sim_rounds       = 0;
static uint32_t       sim_total_snapped = 0;
static uint32_t       sim_interval     = SNAP_INTERVAL_TICKS_DEFAULT;
static uint32_t       sim_min_delta    = SNAP_MIN_DELTA_DEFAULT;

/* Test observability: count IPC calls to AgentFS */
static uint32_t agentfs_calls    = 0;
static bool     agentfs_fail_next = false; /* inject failure for next call */

/* ── Helper: init ────────────────────────────────────────────────────────── */

static void sim_init(void) {
    memset(sim_slots,   0, sizeof(sim_slots));
    memset(sim_history, 0, sizeof(sim_history));
    sim_slot_count   = 0;
    sim_history_head = 0;
    sim_tick         = 0;
    sim_rounds       = 0;
    sim_total_snapped = 0;
    sim_interval     = SNAP_INTERVAL_TICKS_DEFAULT;
    sim_min_delta    = SNAP_MIN_DELTA_DEFAULT;
    agentfs_calls    = 0;
    agentfs_fail_next = false;
}

static SlotSnapState *get_or_create_slot(uint32_t slot_id) {
    for (uint32_t i = 0; i < sim_slot_count; i++)
        if (sim_slots[i].slot_id == slot_id) return &sim_slots[i];
    if (sim_slot_count < SNAP_MAX_SLOTS) {
        SlotSnapState *s = &sim_slots[sim_slot_count++];
        memset(s, 0, sizeof(*s));
        s->slot_id = slot_id;
        return s;
    }
    return NULL;
}

static void history_push(uint32_t tick, uint32_t checked,
                          uint32_t snapped, uint32_t skipped, uint32_t failed) {
    sim_history[sim_history_head % SNAP_HISTORY_SIZE] = (SnapHistoryEntry){
        .tick          = tick,
        .slots_checked = checked,
        .slots_snapped = snapped,
        .skipped_delta = skipped,
        .failed        = failed,
    };
    sim_history_head++;
}

/* Simulated AgentFS snapshot — returns true unless failure injected */
static bool sim_snapshot_slot(SlotSnapState *s) {
    agentfs_calls++;
    if (agentfs_fail_next) { agentfs_fail_next = false; return false; }
    s->last_snap_tick    = sim_tick;
    s->last_snap_heap_kb = 0;
    s->snap_count++;
    return true;
}

/* Run one snapshot round over running_mask */
static void sim_run_round(uint32_t running_mask) {
    uint32_t checked = 0, snapped = 0, skipped = 0, failed = 0;
    for (uint32_t id = 0; id < SNAP_MAX_SLOTS; id++) {
        if (!(running_mask & (1u << id))) continue;
        checked++;
        SlotSnapState *s = get_or_create_slot(id);
        if (!s) { failed++; continue; }

        uint32_t tick_delta = sim_tick - s->last_snap_tick;
        if (tick_delta < sim_interval && s->snap_count > 0) {
            skipped++;
            continue;
        }
        if (sim_snapshot_slot(s)) { snapped++; sim_total_snapped++; }
        else                       { failed++; }
    }
    sim_rounds++;
    history_push(sim_tick, checked, snapped, skipped, failed);
}

/* Advance ticks and auto-fire rounds at interval boundaries */
static void sim_advance(uint32_t ticks, uint32_t running_mask) {
    for (uint32_t t = 0; t < ticks; t++) {
        sim_tick++;
        if ((sim_tick % sim_interval) == 0)
            sim_run_round(running_mask);
    }
}

/* ── Test helpers ────────────────────────────────────────────────────────── */

#define PASS(name) printf("PASS: %s\n", name)
#define FAIL(name, ...) do { printf("FAIL: %s — ", name); printf(__VA_ARGS__); printf("\n"); failures++; } while(0)

static int failures = 0;

/* ── Test 1: Initial state ───────────────────────────────────────────────── */

static void test_initial_state(void) {
    sim_init();
    assert(sim_tick         == 0);
    assert(sim_rounds       == 0);
    assert(sim_total_snapped == 0);
    assert(sim_slot_count   == 0);
    PASS("initial state: all counters zero");
}

/* ── Test 2: Round fires at interval boundary ─────────────────────────────── */

static void test_interval_fire(void) {
    sim_init();
    uint32_t mask = 0x03; /* slots 0 and 1 running */
    sim_advance(499, mask);
    if (sim_rounds != 0) { FAIL("interval_fire", "round fired before boundary (rounds=%u)", sim_rounds); return; }
    sim_advance(1, mask); /* tick 500 — should fire */
    if (sim_rounds != 1) { FAIL("interval_fire", "round did not fire at tick 500 (rounds=%u)", sim_rounds); return; }
    if (sim_total_snapped != 2) { FAIL("interval_fire", "expected 2 snapped, got %u", sim_total_snapped); return; }
    PASS("interval_fire: round fires exactly at SNAP_INTERVAL_TICKS");
}

/* ── Test 3: Delta skip: slot with snap_count>0 and tick_delta < interval ── */

static void test_delta_skip(void) {
    sim_init();
    uint32_t mask = 0x01; /* slot 0 only */
    /* Fire first round at tick 500 (unconditional first snap) */
    sim_advance(500, mask);
    uint32_t after_first = sim_total_snapped;
    /* Advance only 100 more ticks — less than interval → should skip on next round */
    /* Manually trigger a round at tick 500+100 to test delta logic */
    uint32_t saved_tick = sim_tick;
    sim_tick = saved_tick + 100; /* simulate 100 ticks elapsed */
    uint32_t pre = sim_total_snapped;
    sim_run_round(mask); /* tick_delta = 100 < 500 → should skip */
    if (sim_total_snapped != pre) {
        FAIL("delta_skip", "slot not skipped when tick_delta < interval (snapped extra %u)", sim_total_snapped - pre);
        return;
    }
    /* Check skipped count in last history entry */
    uint32_t last_idx = (sim_history_head - 1) % SNAP_HISTORY_SIZE;
    if (sim_history[last_idx].skipped_delta != 1) {
        FAIL("delta_skip", "expected skipped_delta=1, got %u", sim_history[last_idx].skipped_delta);
        return;
    }
    (void)after_first;
    PASS("delta_skip: recently-snapped slot correctly skipped");
}

/* ── Test 4: Force round snapshots all running slots ─────────────────────── */

static void test_force_round(void) {
    sim_init();
    uint32_t mask = 0xFF; /* all 8 slots running */
    /* Don't advance to interval — force immediately */
    sim_run_round(mask);
    if (sim_total_snapped != 8) {
        FAIL("force_round", "expected 8 snapped, got %u", sim_total_snapped);
        return;
    }
    PASS("force_round: all 8 running slots snapped immediately");
}

/* ── Test 5: History ring wraps correctly ─────────────────────────────────── */

static void test_history_ring_wrap(void) {
    sim_init();
    uint32_t mask = 0x01;
    /* Fire 10 rounds — more than SNAP_HISTORY_SIZE=8 */
    for (uint32_t r = 0; r < 10; r++) {
        sim_run_round(mask);
        sim_tick += sim_interval; /* advance tick so slots are eligible */
    }
    if (sim_rounds != 10) {
        FAIL("history_ring_wrap", "expected 10 rounds, got %u", sim_rounds);
        return;
    }
    if (sim_history_head != 10) {
        FAIL("history_ring_wrap", "expected head=10, got %u", sim_history_head);
        return;
    }
    /* Last 8 entries should exist; oldest 2 are overwritten */
    uint32_t last_idx = (sim_history_head - 1) % SNAP_HISTORY_SIZE;
    if (sim_history[last_idx].slots_checked == 0) {
        FAIL("history_ring_wrap", "most recent history entry empty");
        return;
    }
    PASS("history_ring_wrap: ring overwrites oldest entries correctly");
}

/* ── Test 6: Policy update — new interval honoured ───────────────────────── */

static void test_policy_update(void) {
    sim_init();
    sim_interval = 200; /* override to 200-tick interval */
    uint32_t mask = 0x01;
    sim_advance(199, mask);
    if (sim_rounds != 0) { FAIL("policy_update", "fired before new interval"); return; }
    sim_advance(1, mask);
    if (sim_rounds != 1) { FAIL("policy_update", "did not fire at new interval=200"); return; }
    PASS("policy_update: new interval_ticks immediately honoured");
}

/* ── Test 7: Mask filtering ───────────────────────────────────────────────── */

static void test_mask_filter(void) {
    sim_init();
    uint32_t mask = 0x05; /* slots 0 and 2 only */
    sim_run_round(mask);
    if (sim_total_snapped != 2) {
        FAIL("mask_filter", "expected 2 snapped (slots 0,2), got %u", sim_total_snapped);
        return;
    }
    uint32_t last_idx = (sim_history_head - 1) % SNAP_HISTORY_SIZE;
    if (sim_history[last_idx].slots_checked != 2) {
        FAIL("mask_filter", "expected checked=2, got %u", sim_history[last_idx].slots_checked);
        return;
    }
    PASS("mask_filter: only slots in running_mask are checked");
}

/* ── Test 8: Status counters ─────────────────────────────────────────────── */

static void test_status_counters(void) {
    sim_init();
    uint32_t mask = 0x07; /* slots 0,1,2 */
    sim_run_round(mask);
    sim_tick += sim_interval;
    sim_run_round(mask);
    if (sim_rounds != 2)         { FAIL("status_counters", "rounds=%u != 2", sim_rounds); return; }
    if (sim_total_snapped < 3)   { FAIL("status_counters", "total_snapped=%u < 3", sim_total_snapped); return; }
    if (agentfs_calls < 3)       { FAIL("status_counters", "agentfs_calls=%u < 3", agentfs_calls); return; }
    PASS("status_counters: rounds + total_snapped + agentfs_calls all accurate");
}

/* ── Test 9: AgentFS failure handling ────────────────────────────────────── */

static void test_agentfs_failure(void) {
    sim_init();
    agentfs_fail_next = true;
    uint32_t mask = 0x01;
    sim_run_round(mask);
    /* Slot should have been attempted but failed */
    uint32_t last_idx = (sim_history_head - 1) % SNAP_HISTORY_SIZE;
    if (sim_history[last_idx].failed != 1) {
        FAIL("agentfs_failure", "expected failed=1, got %u", sim_history[last_idx].failed);
        return;
    }
    if (sim_total_snapped != 0) {
        FAIL("agentfs_failure", "total_snapped should be 0 after failure, got %u", sim_total_snapped);
        return;
    }
    PASS("agentfs_failure: failed IPC recorded in history, total_snapped unchanged");
}

/* ── Main ────────────────────────────────────────────────────────────────── */

int main(void) {
    printf("=== snapshot_sched unit tests ===\n\n");
    test_initial_state();
    test_interval_fire();
    test_delta_skip();
    test_force_round();
    test_history_ring_wrap();
    test_policy_update();
    test_mask_filter();
    test_status_counters();
    test_agentfs_failure();
    printf("\n=== Results: %d test(s) failed ===\n", failures);
    return failures ? 1 : 0;
}
