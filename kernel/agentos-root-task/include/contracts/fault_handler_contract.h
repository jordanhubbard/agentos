/*
 * FaultHandler IPC Contract
 *
 * The FaultHandler PD receives seL4 fault notifications for all PDs and
 * applies configured restart policies.  It tracks fault history per PD.
 *
 * Channel: (controller → fault_handler; fault_handler → controller on escalation)
 * Opcodes: MSG_FAULT_NOTIFY, MSG_FAULT_QUERY, MSG_FAULT_HISTORY, OP_FAULT_POLICY_SET
 *
 * Invariants:
 *   - MSG_FAULT_NOTIFY is sent by the monitor to fault_handler on every fault.
 *   - The reply to NOTIFY contains the policy decision (restart, escalate, etc.).
 *   - After FAULT_POLICY_ESCALATE_AFTER restarts, the fault is escalated to
 *     the Witness via EventBus.
 *   - FAULT_QUERY is read-only; it returns the fault history summary.
 *   - FAULT_HISTORY copies the full history to shared memory.
 *   - OP_FAULT_POLICY_SET updates the per-slot policy; takes effect immediately.
 */

#pragma once
#include "../agentos.h"

/* ─── Request structs ────────────────────────────────────────────────────── */

struct fault_handler_req_notify {
    uint32_t pd_id;
    uint32_t fault_kind;        /* FAULT_KIND_* */
    uint64_t addr;              /* faulting address (if applicable) */
    uint64_t ip;                /* instruction pointer at fault */
};

#define FAULT_KIND_VM_FAULT      0   /* page fault */
#define FAULT_KIND_CAP_FAULT     1   /* capability access fault */
#define FAULT_KIND_UNKNOWN       2

struct fault_handler_req_query {
    uint32_t pd_id;
};

struct fault_handler_req_history {
    uint32_t pd_id;
    uint32_t max_entries;       /* max entries to copy to shmem */
};

struct fault_handler_req_policy_set {
    uint32_t slot_id;
    uint32_t max_restarts;
    uint32_t escalate_after;    /* escalate after this many restarts */
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct fault_handler_reply_notify {
    uint32_t ok;
    uint32_t action;            /* FAULT_ACTION_* */
};

#define FAULT_ACTION_RESTART    0
#define FAULT_ACTION_ESCALATE   1
#define FAULT_ACTION_IGNORE     2
#define FAULT_ACTION_KILL       3

struct fault_handler_reply_query {
    uint32_t ok;
    uint32_t restart_count;     /* total restarts for this PD */
    uint32_t last_fault_kind;
    uint64_t last_fault_ip;
};

struct fault_handler_reply_history {
    uint32_t ok;
    uint32_t count;             /* entries written to shmem */
};

struct fault_handler_reply_policy_set {
    uint32_t ok;
};

/* ─── Shmem layout: fault history entry ─────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint64_t timestamp_ns;
    uint32_t pd_id;
    uint32_t fault_kind;
    uint64_t addr;
    uint64_t ip;
    uint32_t restart_num;       /* which restart this was (0-indexed) */
} fault_history_entry_t;

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum fault_handler_error {
    FAULT_HANDLER_OK            = 0,
    FAULT_HANDLER_ERR_BAD_PD    = 1,
    FAULT_HANDLER_ERR_NO_POLICY = 2,
    FAULT_HANDLER_ERR_BAD_SLOT  = 3,
};
