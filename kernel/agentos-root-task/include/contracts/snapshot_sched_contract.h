/*
 * SnapshotSched IPC Contract
 *
 * The SnapshotSched PD periodically checkpoints live swap slot state to
 * AgentFS.  Snapshots are triggered by a configurable tick interval or
 * by a heap-delta threshold.
 *
 * Channel: (controller → snapshot_sched; assigned in agentos.system)
 * Opcodes: OP_SNAP_STATUS, OP_SNAP_SET_POLICY, OP_SNAP_FORCE, OP_SNAP_GET_HISTORY
 *
 * Invariants:
 *   - OP_SNAP_STATUS is read-only; it never triggers a snapshot.
 *   - OP_SNAP_FORCE runs a snapshot round immediately regardless of policy.
 *   - OP_SNAP_SET_POLICY updates the tick interval and delta threshold;
 *     takes effect on the next scheduled round.
 *   - OP_SNAP_GET_HISTORY returns the last 4 round summaries in MR1..MR8.
 *   - EVENT_SNAP_SCHED_DONE is published to EventBus after each round.
 */

#pragma once
#include "../agentos.h"

/* ─── Request structs ────────────────────────────────────────────────────── */

struct snap_sched_req_status {
    /* no fields */
};

struct snap_sched_req_set_policy {
    uint32_t interval_ticks;    /* ticks between rounds (SNAP_INTERVAL_TICKS_DEFAULT) */
    uint32_t min_delta_kb;      /* min heap-KB change to force snap (SNAP_MIN_DELTA_DEFAULT) */
};

struct snap_sched_req_force {
    uint32_t flags;             /* SNAP_FORCE_FLAG_* */
};

#define SNAP_FORCE_FLAG_ALL_SLOTS (1u << 0)  /* snapshot all slots, ignore delta */

struct snap_sched_req_get_history {
    /* no fields — last 4 rounds returned in MRs */
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct snap_sched_reply_status {
    uint32_t ok;
    uint32_t rounds;            /* total rounds since boot */
    uint32_t total_snapped;     /* total slots snapshotted since boot */
    uint32_t tick;              /* current scheduler tick */
    uint32_t slot_count;        /* slots currently tracked */
};

struct snap_sched_reply_set_policy {
    uint32_t ok;
};

struct snap_sched_reply_force {
    uint32_t ok;
    uint32_t round_number;      /* round index of the forced snapshot */
};

struct snap_sched_reply_get_history {
    uint32_t ok;
    /* Round summaries packed into MRs: MR1..MR2 = round0 (round#, snapped),
     * MR3..MR4 = round1, MR5..MR6 = round2, MR7..MR8 = round3 */
};

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum snap_sched_error {
    SNAP_SCHED_OK             = 0,
    SNAP_SCHED_ERR_BAD_POLICY = 1,  /* interval_ticks == 0 */
    SNAP_SCHED_ERR_BUSY       = 2,  /* forced snapshot while round in progress */
};
