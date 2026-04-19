#pragma once
/* SNAPSHOT_SCHED contract — version 1
 * PD: snapshot_sched | Source: src/snapshot_sched.c | Channel: (passive PD; no fixed controller channel)
 */
#include <stdint.h>
#include <stdbool.h>

#define SNAPSHOT_SCHED_CONTRACT_VERSION 1

/* ── Opcodes ── */
#define SNAPSHOT_SCHED_OP_STATUS       0xB0u  /* query: rounds, total_snapped, tick, slot_count */
#define SNAPSHOT_SCHED_OP_SET_POLICY   0xB1u  /* set interval_ticks and min_delta_kb */
#define SNAPSHOT_SCHED_OP_FORCE        0xB2u  /* force immediate snapshot round */
#define SNAPSHOT_SCHED_OP_GET_HISTORY  0xB3u  /* retrieve last 4 round summaries */

/* ── EventBus event ── */
#define EVENT_SNAP_SCHED_DONE          0x20u  /* emitted after each completed round */

/* ── Configuration defaults (cross-ref: agentos.h) ── */
#define SNAP_INTERVAL_TICKS_DEFAULT    500u   /* ticks between rounds (~5s @ 100Hz) */
#define SNAP_MIN_DELTA_DEFAULT          64u   /* min heap-KB change to force snap */
#define SNAP_MAX_SLOTS                   8u   /* max simultaneously tracked slots */

/* ── Request / Reply structs ── */

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* SNAPSHOT_SCHED_OP_STATUS */
} snapshot_sched_req_status_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t rounds;          /* total rounds completed since boot */
    uint32_t total_snapped;   /* total slots snapshotted across all rounds */
    uint32_t tick;            /* current scheduler tick */
    uint32_t slot_count;      /* number of slots currently tracked */
    uint32_t interval_ticks;  /* current interval setting */
    uint32_t min_delta_kb;    /* current delta threshold */
} snapshot_sched_reply_status_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* SNAPSHOT_SCHED_OP_SET_POLICY */
    uint32_t interval_ticks;  /* ticks between snapshot rounds (0 = disable) */
    uint32_t min_delta_kb;    /* minimum heap change in KB to trigger snapshot */
} snapshot_sched_req_set_policy_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t effective_interval; /* actual interval applied (may be clamped) */
} snapshot_sched_reply_set_policy_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* SNAPSHOT_SCHED_OP_FORCE */
} snapshot_sched_req_force_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t round_number;    /* round number assigned to forced round */
    uint32_t slots_checked;   /* slots examined in this round */
    uint32_t slots_snapped;   /* slots where snapshot was triggered */
} snapshot_sched_reply_force_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* SNAPSHOT_SCHED_OP_GET_HISTORY */
} snapshot_sched_req_history_t;

/* One summary entry for a completed round */
typedef struct __attribute__((packed)) {
    uint32_t round_number;
    uint32_t tick;
    uint32_t slots_checked;
    uint32_t slots_snapped;
} snapshot_sched_round_summary_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t entry_count;     /* number of valid summaries (up to 4) */
    snapshot_sched_round_summary_t summaries[4];
} snapshot_sched_reply_history_t;

/* EventBus payload for EVENT_SNAP_SCHED_DONE */
typedef struct __attribute__((packed)) {
    uint32_t round_number;
    uint32_t slots_checked;
    uint32_t slots_snapped;
    uint32_t tick;
} snapshot_sched_event_done_t;

/* ── Error codes ── */
typedef enum {
    SNAPSHOT_SCHED_OK         = 0,
    SNAPSHOT_SCHED_ERR_BUSY   = 1,  /* force requested while round already in progress */
    SNAPSHOT_SCHED_ERR_NO_SLOTS = 2, /* no slots registered for tracking */
    SNAPSHOT_SCHED_ERR_BAD_POLICY = 3, /* invalid interval or delta value */
} snapshot_sched_error_t;

/* ── Invariants ──
 * - snapshot_sched is a passive PD: it runs only when triggered by timer notification.
 * - SET_POLICY with interval_ticks=0 disables automatic rounds (FORCE still works).
 * - A forced round counts toward the round_number sequence.
 * - min_delta_kb of 0 means every slot is snapshotted every round (expensive).
 * - At most SNAP_MAX_SLOTS (8) slots can be tracked simultaneously.
 * - EVENT_SNAP_SCHED_DONE is published to EventBus after every completed round.
 */
