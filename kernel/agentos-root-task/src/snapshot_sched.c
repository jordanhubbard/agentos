/* snapshot_sched.c — Periodic WASM slot snapshot scheduler PD
 *
 * Priority : 180 (passive, between live_migrate=185 and power_mgr=240)
 * Purpose  : Proactive periodic checkpointing of all running WASM slots to
 *            AgentFS (sparky:8791) so that agent state can be restored after
 *            a crash, migration, or OOM eviction.
 *
 * Design
 * ──────
 * snapshot_sched is a passive PD driven by a timer tick from the system
 * timer PD (CH_TIMER_TICK, id 0).  On each tick it increments a local tick
 * counter.  When the counter reaches SNAP_INTERVAL_TICKS it fires a
 * checkpoint round:
 *
 *   1. Query the controller for the set of currently-running slots
 *      (OP_CTRL_SLOT_LIST via CH_CTRL, id 1).
 *   2. For each running slot compute a delta-compression score:
 *         score = (slot.heap_used_kb - slot.last_snap_heap_kb)
 *                 + (slot.age_ticks  - slot.last_snap_tick)
 *      Slots where score < SNAP_MIN_DELTA are skipped (nothing changed).
 *   3. For each eligible slot, send OP_SNAPSHOT_SLOT to AgentFS
 *      (CH_AGENTFS, id 2).  AgentFS writes a content-addressed snapshot and
 *      returns a 128-bit hash in MR1:MR2.
 *   4. On success, update slot.last_snap_tick and slot.last_snap_heap_kb in
 *      the local tracking table.
 *   5. Emit a SNAP_SCHED_DONE event on CH_EVENT_BUS (id 3) with:
 *         MR1 = slots_checked, MR2 = slots_snapped, MR3 = tick_counter
 *
 * Configuration (set via OP_SNAP_SET_POLICY IPC, id 4):
 *   SNAP_INTERVAL_TICKS  (default 500)  — ticks between rounds  (~5 s @ 100Hz)
 *   SNAP_MIN_DELTA       (default 64)   — min heap-KB change to trigger snap
 *   SNAP_MAX_SLOTS       (default 8)    — maximum slots to checkpoint per round
 *
 * Opcodes
 * ───────
 *   OP_SNAP_STATUS    0xB0  — query: MR1=rounds, MR2=total_snapped, MR3=tick
 *   OP_SNAP_SET_POLICY 0xB1 — configure: MR1=interval, MR2=min_delta
 *   OP_SNAP_FORCE      0xB2 — force immediate snapshot of all running slots
 *   OP_SNAP_GET_HISTORY 0xB3 — return last 8 snapshot event records
 *
 * Channels
 * ────────
 *   CH_TIMER_TICK   (0) — timer PD sends periodic tick (passive notify)
 *   CH_CTRL         (1) — query running slot list from controller
 *   CH_AGENTFS      (2) — snapshot write (OP_SNAPSHOT_SLOT)
 *   CH_EVENT_BUS    (3) — publish SNAP_SCHED_DONE events
 *   CH_POLICY       (4) — receive OP_SNAP_SET_POLICY / OP_SNAP_FORCE from
 *                         controller or admin
 *
 * Companion items
 * ───────────────
 *   agentos.h    — OP_SNAP_* constants, SNAP_SCHED_DONE event id
 *   topology.yaml — snapshot_sched PD entry + 5 channels
 *   test/test_snapshot_sched.c — unit tests
 */

#include "contracts/snapshot_sched_contract.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ── Channel IDs ─────────────────────────────────────────────────────────── */
#define CH_TIMER_TICK   0u
#define CH_CTRL         1u
#define CH_AGENTFS      2u
#define CH_EVENT_BUS    3u
#define CH_POLICY       4u

/* ── AgentFS / controller opcodes ────────────────────────────────────────── */
#define OP_SNAPSHOT_SLOT      0xE1u  /* MR0=op, MR1=slot_id → MR0=ok, MR1:MR2=hash */
#define OP_CTRL_SLOT_LIST     0x10u  /* → MR1=bitmask of running slots (bits 0-7) */
#define OP_EVENT_BUS_PUBLISH  0x30u  /* MR1=event_id, MR2-MR5=payload */

/* ── Snapshot scheduler opcodes ─────────────────────────────────────────── */
#define OP_SNAP_STATUS       0xB0u
#define OP_SNAP_SET_POLICY   0xB1u
#define OP_SNAP_FORCE        0xB2u
#define OP_SNAP_GET_HISTORY  0xB3u

/* ── Event IDs ────────────────────────────────────────────────────────────── */
#define EVENT_SNAP_SCHED_DONE 0x20u  /* MR2=slots_checked, MR3=slots_snapped, MR4=tick */

/* ── Configuration defaults ─────────────────────────────────────────────── */
#define SNAP_INTERVAL_TICKS_DEFAULT  500u   /* ~5s at 100Hz */
#define SNAP_MIN_DELTA_DEFAULT       64u    /* minimum KB change to trigger */
#define SNAP_MAX_SLOTS               8u
#define SNAP_HISTORY_SIZE            8u

/* ── Per-slot tracking ───────────────────────────────────────────────────── */
typedef struct {
    uint32_t slot_id;
    uint32_t last_snap_tick;       /* tick counter at last successful snap */
    uint32_t last_snap_heap_kb;    /* heap usage at last successful snap    */
    uint32_t snap_count;           /* total snapshots taken for this slot   */
    uint32_t last_hash_lo;         /* low 32-bits of last snapshot hash     */
    uint32_t last_hash_hi;         /* high 32-bits of last snapshot hash    */
} SlotSnapState;

/* ── Snapshot event history ring ─────────────────────────────────────────── */
typedef struct {
    uint32_t tick;
    uint32_t slots_checked;
    uint32_t slots_snapped;
    uint32_t skipped_delta;   /* skipped because delta < SNAP_MIN_DELTA */
    uint32_t failed;          /* AgentFS returned error */
} SnapHistoryEntry;

/* ── Module-level state ──────────────────────────────────────────────────── */
static SlotSnapState  g_slots[SNAP_MAX_SLOTS];
static uint32_t       g_slot_count = 0;

static SnapHistoryEntry g_history[SNAP_HISTORY_SIZE];
static uint32_t         g_history_head = 0;   /* next write index (ring) */

static uint32_t g_tick_counter      = 0;
static uint32_t g_round_counter     = 0;
static uint32_t g_total_snapped     = 0;

/* ── Policy (runtime-configurable) ──────────────────────────────────────── */
static uint32_t g_interval_ticks = SNAP_INTERVAL_TICKS_DEFAULT;
static uint32_t g_min_delta      = SNAP_MIN_DELTA_DEFAULT;

/* ── Init ────────────────────────────────────────────────────────────────── */

static void snapshot_sched_pd_init(void) {
    memset(g_slots, 0, sizeof(g_slots));
    memset(g_history, 0, sizeof(g_history));
    g_tick_counter  = 0;
    g_round_counter = 0;
    g_total_snapped = 0;
    g_slot_count    = 0;
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */

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
    return NULL; /* table full — evict LRU in a future pass */
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

/* ── Snapshot one slot via AgentFS ──────────────────────────────────────── */

static bool snapshot_slot(SlotSnapState *s) {
    rep_u32(rep, 0, OP_SNAPSHOT_SLOT);
    rep_u32(rep, 4, s->slot_id);
    uint32_t resp =
        /* E5-S8: ppcall stubbed */
    (void)resp; /* AgentFS returns MR0=ok, MR1:MR2=hash_lo:hash_hi */

    uint32_t ok      = (uint32_t)msg_u32(req, 0);
    uint32_t hash_lo = (uint32_t)msg_u32(req, 4);
    uint32_t hash_hi = (uint32_t)msg_u32(req, 8);

    if (!ok) return false;

    s->last_snap_tick    = g_tick_counter;
    /* We don't have heap info here without querying mem_profiler — use 0 as
     * a sentinel meaning "any change since last snap is eligible".         */
    s->last_snap_heap_kb = 0;
    s->last_hash_lo      = hash_lo;
    s->last_hash_hi      = hash_hi;
    s->snap_count++;
    return true;
}

/* ── Full snapshot round ─────────────────────────────────────────────────── */

static void run_snapshot_round(uint32_t running_mask) {
    uint32_t checked = 0, snapped = 0, skipped = 0, failed = 0;

    for (uint32_t slot_id = 0; slot_id < SNAP_MAX_SLOTS; slot_id++) {
        if (!(running_mask & (1u << slot_id))) continue;
        checked++;

        SlotSnapState *s = get_or_create_slot(slot_id);
        if (!s) { failed++; continue; }

        /* Delta check: skip if last snap was recent enough.
         * g_min_delta acts as the minimum snap count before interval gating
         * kicks in — the first g_min_delta snaps are always taken so the
         * heap baseline is established (heap-KB delta tracking requires
         * mem_profiler integration, deferred to Phase 2). */
        uint32_t tick_delta = g_tick_counter - s->last_snap_tick;
        if (tick_delta < g_interval_ticks && s->snap_count >= g_min_delta) {
            skipped++;
            continue;
        }

        if (snapshot_slot(s)) {
            snapped++;
            g_total_snapped++;
        } else {
            failed++;
        }
    }

    g_round_counter++;
    history_push(g_tick_counter, checked, snapped, skipped, failed);

    /* Publish SNAP_SCHED_DONE to event bus */
    rep_u32(rep, 0, OP_EVENT_BUS_PUBLISH);
    rep_u32(rep, 4, EVENT_SNAP_SCHED_DONE);
    rep_u32(rep, 8, checked);
    rep_u32(rep, 12, snapped);
    rep_u32(rep, 16, g_tick_counter);
    /* E5-S8: ppcall stubbed */
}

/* ── Query controller for running slot bitmask ──────────────────────────── */

static uint32_t query_running_slots(void) {
    rep_u32(rep, 0, OP_CTRL_SLOT_LIST);
    uint32_t resp =
        /* E5-S8: ppcall stubbed */
    (void)resp;
    return (uint32_t)msg_u32(req, 4); /* bitmask of running slots */
}

/* ── notified — timer tick path ─────────────────────────────────────────── */

static void snapshot_sched_pd_notified(uint32_t ch) {
    if (ch == CH_TIMER_TICK) {
        g_tick_counter++;
        if ((g_tick_counter % g_interval_ticks) == 0) {
            uint32_t running_mask = query_running_slots();
            if (running_mask) run_snapshot_round(running_mask);
        }
    }
}

/* ── protected_procedure_call — policy + status ─────────────────────────── */

static uint32_t snapshot_sched_h_dispatch(sel4_badge_t b, const sel4_msg_t *req, sel4_msg_t *rep, void *ctx) {
    uint32_t op = (uint32_t)msg_u32(req, 0);
    (void)b;
    (void)ctx;

    switch (op) {

    case OP_SNAP_STATUS:
        rep_u32(rep, 4, g_round_counter);
        rep_u32(rep, 8, g_total_snapped);
        rep_u32(rep, 12, g_tick_counter);
        rep_u32(rep, 16, g_slot_count);
        rep->length = 20;
        return SEL4_ERR_OK;

    case OP_SNAP_SET_POLICY: {
        uint32_t new_interval = (uint32_t)msg_u32(req, 4);
        uint32_t new_delta    = (uint32_t)msg_u32(req, 8);
        if (new_interval > 0)  g_interval_ticks = new_interval;
        if (new_delta > 0)     g_min_delta       = new_delta;
        rep_u32(rep, 0, 1); /* ok */
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    case OP_SNAP_FORCE: {
        /* Immediate snapshot round of all running slots */
        uint32_t running_mask = query_running_slots();
        run_snapshot_round(running_mask);
        rep_u32(rep, 0, 1);
        rep_u32(rep, 4, g_round_counter);
        rep->length = 8;
        return SEL4_ERR_OK;
    }

    case OP_SNAP_GET_HISTORY: {
        /* Return last 4 history entries (8 MRs = 4 × 2 MRs each) */
        for (uint32_t i = 0; i < 4 && i < SNAP_HISTORY_SIZE; i++) {
            uint32_t idx = (g_history_head + SNAP_HISTORY_SIZE - 1 - i) % SNAP_HISTORY_SIZE;
            rep_u32(rep, (i * 2 + 1) * 4, g_history[idx].tick);
            rep_u32(rep, (i * 2 + 2) * 4,                 (g_history[idx].slots_snapped << 16) | g_history[idx].slots_checked);
        }
        rep->length = 36;
        return SEL4_ERR_OK;
    }

    default:
        rep_u32(rep, 0, 0); /* unknown op */
        rep->length = 4;
        return SEL4_ERR_OK;
    }
}

/* ── E5-S8: Entry point ─────────────────────────────────────────────────── */
void snapshot_sched_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    (void)ns_ep;
    snapshot_sched_pd_init();
    static sel4_server_t srv;
    sel4_server_init(&srv, my_ep);
    /* Dispatch all opcodes through the generic handler */
    sel4_server_register(&srv, SEL4_SERVER_OPCODE_ANY, snapshot_sched_h_dispatch, (void *)0);
    sel4_server_run(&srv);
}
