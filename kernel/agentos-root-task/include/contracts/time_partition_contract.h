/*
 * TimePart (Time Partition) IPC Contract
 *
 * The TimePart PD allocates and manages seL4 MCS scheduling contexts.
 * PDs request a CPU budget+period pair; TimePart returns a scheduling
 * context handle that can be passed to seL4_TCB_SetSchedParams.
 *
 * Channel: CH_TIME_PART (see agentos.h)
 * Opcodes: MSG_TPART_ALLOC, MSG_TPART_FREE, MSG_TPART_STATUS
 *
 * Invariants:
 *   - ALLOC returns a sched_ctx handle; FREE releases it.
 *   - A handle not freed before the owning PD exits is leaked until reboot.
 *   - STATUS queries a single sched_ctx; it is read-only.
 *   - budget_us must be ≤ period_us; violation returns TPART_ERR_BAD_BUDGET.
 *   - The total CPU bandwidth across all allocated contexts must not exceed
 *     TPART_MAX_UTILIZATION_PCT; if it would, ALLOC returns TPART_ERR_OVERCOMMIT.
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs ────────────────────────────────────────────────────────── */
#define TIMEPART_CH_CONTROLLER  CH_TIME_PART

/* ─── Configuration ──────────────────────────────────────────────────────── */
#define TPART_MAX_UTILIZATION_PCT  90u  /* max CPU utilization across all contexts */

/* ─── Request structs ────────────────────────────────────────────────────── */

struct tpart_req_alloc {
    uint32_t budget_us;         /* MCS budget per period in microseconds */
    uint32_t period_us;         /* MCS period in microseconds */
    uint32_t priority;          /* seL4 priority (0-254) */
};

struct tpart_req_free {
    uint32_t sched_ctx;         /* handle from ALLOC */
};

struct tpart_req_status {
    uint32_t sched_ctx;
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct tpart_reply_alloc {
    uint32_t ok;
    uint32_t sched_ctx;         /* opaque handle; 0 = invalid */
};

struct tpart_reply_free {
    uint32_t ok;
};

struct tpart_reply_status {
    uint32_t ok;
    uint32_t state;             /* TPART_STATE_* */
    uint32_t remaining_us;      /* budget remaining in current period */
    uint32_t budget_us;
    uint32_t period_us;
};

#define TPART_STATE_ACTIVE   0
#define TPART_STATE_INACTIVE 1  /* budget exhausted, waiting for replenishment */

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum time_partition_error {
    TPART_OK                 = 0,
    TPART_ERR_BAD_BUDGET     = 1,  /* budget_us > period_us */
    TPART_ERR_OVERCOMMIT     = 2,  /* would exceed TPART_MAX_UTILIZATION_PCT */
    TPART_ERR_NO_CONTEXTS    = 3,  /* seL4 scheduling context pool exhausted */
    TPART_ERR_BAD_CTX        = 4,  /* sched_ctx not found */
};
