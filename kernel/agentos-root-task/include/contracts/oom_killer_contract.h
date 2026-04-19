/*
 * OOMKiller IPC Contract
 *
 * The OOMKiller PD monitors system memory pressure and terminates agents
 * when free memory falls below a configured threshold.
 *
 * Channel: CH_OOM_KILLER (see agentos.h)
 * Opcodes: MSG_OOM_STATUS, MSG_OOM_POLICY_SET, MSG_OOM_NOTIFY
 *
 * Invariants:
 *   - MSG_OOM_STATUS is read-only; it returns current memory pressure and
 *     the number of candidate PDs ordered by memory usage (highest first).
 *   - MSG_OOM_POLICY_SET updates the kill threshold; takes effect immediately.
 *   - MSG_OOM_NOTIFY is an EventBus notification (not a PPC reply) sent after
 *     each kill action; it carries the killed PD and bytes reclaimed.
 *   - The default policy is OOM_POLICY_KILL_LARGEST.
 *   - threshold_kb == 0 disables OOM killing.
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs ────────────────────────────────────────────────────────── */
#define OOM_KILLER_CH_CONTROLLER  CH_OOM_KILLER

/* ─── Request structs ────────────────────────────────────────────────────── */

struct oom_req_status {
    /* no fields */
};

struct oom_req_policy_set {
    uint32_t threshold_kb;      /* free memory threshold; kill if below */
    uint32_t policy;            /* OOM_POLICY_* */
};

#define OOM_POLICY_KILL_LARGEST  0  /* kill PD with highest memory usage */
#define OOM_POLICY_KILL_LOWEST   1  /* kill PD with lowest priority */
#define OOM_POLICY_NONE          2  /* log alert only; do not kill */

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct oom_reply_status {
    uint32_t ok;
    uint32_t pressure_kb;       /* current free memory (KB) */
    uint32_t candidates;        /* PDs eligible for kill */
    uint32_t killed_total;      /* total PDs killed since boot */
    uint32_t threshold_kb;      /* current threshold */
    uint32_t policy;
};

struct oom_reply_policy_set {
    uint32_t ok;
};

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum oom_killer_error {
    OOM_OK                  = 0,
    OOM_ERR_BAD_POLICY      = 1,
};
