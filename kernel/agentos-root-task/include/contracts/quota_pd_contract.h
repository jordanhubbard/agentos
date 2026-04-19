#pragma once
/* QUOTA_PD contract — version 1
 * PD: quota_pd | Source: src/quota_pd.c | Channel: CH_QUOTA_CTRL=52 (from controller)
 */
#include <stdint.h>
#include <stdbool.h>

#define QUOTA_PD_CONTRACT_VERSION 1

/* ── Channel IDs (controller perspective) ── */
#define CH_QUOTA_CTRL              52   /* controller -> quota_pd (PPC); cross-ref: agentos.h */
#define CH_QUOTA_INIT               7   /* init_agent -> quota_pd (from init_agent perspective) */
#define CH_QUOTA_NOTIFY            58   /* quota_pd notifies controller on revoke */

/* ── Opcodes ── */
#define QUOTA_PD_OP_REGISTER       0x60u  /* register agent with cpu/mem limits */
#define QUOTA_PD_OP_TICK           0x61u  /* tick agent cpu/mem usage counters */
#define QUOTA_PD_OP_STATUS         0x62u  /* query agent quota state */
#define QUOTA_PD_OP_SET            0x63u  /* update agent quota limits */
#define QUOTA_PD_OP_REVOKE_NOTIFY  0x0B01u /* quota_pd -> controller: revoke agent caps */

/* ── Quota flags (cross-ref: agentos.h QUOTA_FLAG_*) ── */
#define QUOTA_FLAG_ACTIVE          (1u << 0)  /* agent is being tracked */
#define QUOTA_FLAG_CPU_EXCEED      (1u << 1)  /* CPU budget exceeded */
#define QUOTA_FLAG_MEM_EXCEED      (1u << 2)  /* memory budget exceeded */
#define QUOTA_FLAG_REVOKED         (1u << 3)  /* capabilities have been revoked */

/* ── Request / Reply structs ── */

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* QUOTA_PD_OP_REGISTER */
    uint64_t agent_id_hi;
    uint64_t agent_id_lo;
    uint32_t slot_id;         /* worker slot this agent occupies */
    uint32_t cpu_budget_ticks; /* max CPU ticks per quota period */
    uint32_t mem_budget_kb;   /* max memory in kilobytes */
    uint32_t period_ticks;    /* quota measurement period in scheduler ticks */
} quota_pd_req_register_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok, else quota_pd_error_t */
    uint32_t quota_id;        /* handle for subsequent TICK/STATUS/SET calls */
} quota_pd_reply_register_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* QUOTA_PD_OP_TICK */
    uint32_t quota_id;
    uint32_t cpu_ticks_used;  /* CPU ticks consumed since last TICK */
    uint32_t mem_current_kb;  /* current memory footprint in KB */
} quota_pd_req_tick_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t flags;           /* QUOTA_FLAG_* current state */
    uint32_t cpu_remaining;   /* ticks remaining in current period */
    uint32_t mem_remaining_kb;
} quota_pd_reply_tick_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* QUOTA_PD_OP_STATUS */
    uint32_t quota_id;
} quota_pd_req_status_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t flags;           /* QUOTA_FLAG_* */
    uint32_t cpu_budget_ticks;
    uint32_t cpu_used_ticks;
    uint32_t mem_budget_kb;
    uint32_t mem_used_kb;
    uint32_t violations;      /* number of budget violations in lifetime */
} quota_pd_reply_status_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* QUOTA_PD_OP_SET */
    uint32_t quota_id;
    uint32_t new_cpu_budget_ticks;
    uint32_t new_mem_budget_kb;
    uint32_t new_period_ticks;
} quota_pd_req_set_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
} quota_pd_reply_set_t;

/* Notification sent by quota_pd to controller when caps should be revoked */
typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* QUOTA_PD_OP_REVOKE_NOTIFY / MSG_QUOTA_REVOKE */
    uint32_t quota_id;
    uint32_t slot_id;
    uint32_t flags;           /* QUOTA_FLAG_* indicating which budget was exceeded */
    uint64_t agent_id_hi;
    uint64_t agent_id_lo;
} quota_pd_notify_revoke_t;

/* ── Error codes ── */
typedef enum {
    QUOTA_PD_OK               = 0,
    QUOTA_PD_ERR_FULL         = 1,  /* quota table full */
    QUOTA_PD_ERR_BAD_ID       = 2,  /* quota_id not registered */
    QUOTA_PD_ERR_ALREADY_REG  = 3,  /* agent_id already registered */
    QUOTA_PD_ERR_BAD_BUDGET   = 4,  /* zero or overflow budget value */
} quota_pd_error_t;

/* ── Invariants ──
 * - Each agent must be registered before TICK or STATUS is called.
 * - TICK must be called at least once per period_ticks to prevent stale detection.
 * - When CPU or memory budget is exceeded, REVOKE_NOTIFY is sent to CH_QUOTA_NOTIFY.
 * - SET does not reset accumulated usage; it takes effect at the next period boundary.
 * - quota_id is unique per session and must be deregistered when the agent terminates.
 */
