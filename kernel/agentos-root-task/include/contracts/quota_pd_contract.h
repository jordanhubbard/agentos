/*
 * QuotaPD IPC Contract
 *
 * The QuotaPD enforces per-agent CPU and memory resource quotas.
 * Agents register their limits at spawn; the controller ticks usage each
 * scheduling quantum.  When a quota is exceeded, QuotaPD notifies the
 * controller to revoke the agent's capabilities.
 *
 * Channel: CH_QUOTA_CTRL (controller), CH_QUOTA_INIT (init_agent)
 * Opcodes: OP_QUOTA_REGISTER, OP_QUOTA_TICK, OP_QUOTA_STATUS, OP_QUOTA_SET
 *
 * Invariants:
 *   - An agent must be registered before any TICK or STATUS call.
 *   - TICK is called once per scheduling quantum per active agent.
 *   - When cpu_used >= cpu_limit or mem_kb >= mem_limit, QuotaPD sends
 *     MSG_QUOTA_REVOKE to the controller (CH_QUOTA_NOTIFY).
 *   - QUOTA_SET updates limits without requiring re-registration.
 *   - Flags QUOTA_FLAG_CPU_EXCEED and QUOTA_FLAG_MEM_EXCEED are set in
 *     the quota state when the respective limit is breached.
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs ────────────────────────────────────────────────────────── */
#define QUOTAPD_CH_CONTROLLER   CH_QUOTA_CTRL
#define QUOTAPD_CH_INIT_AGENT   CH_QUOTA_INIT
#define QUOTAPD_CH_NOTIFY       CH_QUOTA_NOTIFY   /* QuotaPD → controller on breach */

/* ─── Request structs ────────────────────────────────────────────────────── */

struct quota_req_register {
    uint32_t agent_id;
    uint32_t cpu_limit_us;      /* CPU budget per period in microseconds */
    uint32_t mem_limit_kb;      /* memory ceiling in kilobytes */
    uint32_t flags;             /* QUOTA_FLAG_ACTIVE */
};

struct quota_req_tick {
    uint32_t agent_id;
    uint32_t cpu_used_us;       /* CPU used this quantum */
    uint32_t mem_used_kb;       /* current memory usage */
};

struct quota_req_status {
    uint32_t agent_id;
};

struct quota_req_set {
    uint32_t agent_id;
    uint32_t cpu_limit_us;
    uint32_t mem_limit_kb;
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct quota_reply_register {
    uint32_t ok;
};

struct quota_reply_tick {
    uint32_t ok;
    uint32_t flags;             /* QUOTA_FLAG_* — non-zero means breach */
};

struct quota_reply_status {
    uint32_t ok;
    uint32_t state;             /* QUOTA_FLAG_ACTIVE | QUOTA_FLAG_REVOKED | ... */
    uint32_t cpu_used_us;       /* cumulative this period */
    uint32_t mem_used_kb;
    uint32_t cpu_limit_us;
    uint32_t mem_limit_kb;
};

struct quota_reply_set {
    uint32_t ok;
};

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum quota_error {
    QUOTA_OK                 = 0,
    QUOTA_ERR_NOT_REGISTERED = 1,
    QUOTA_ERR_ALREADY_REG    = 2,
    QUOTA_ERR_TABLE_FULL     = 3,
    QUOTA_ERR_BAD_AGENT      = 4,
};
