/*
 * agentOS Quota Userspace Stub
 *
 * Provides a simple C API for agents to interact with the quota_pd
 * Protection Domain. Agents typically don't call these directly —
 * init_agent registers quotas on spawn and calls QUOTA_TICK per
 * scheduler round. This stub is for system services that need
 * direct quota management.
 *
 * Usage:
 *   #include "quota.h"
 *
 *   quota_register(agent_id, cpu_limit_ms, mem_limit_kb);
 *   quota_tick(agent_id, cpu_delta_ms, mem_current_kb);
 *   quota_status(agent_id, &status);
 *   quota_set(agent_id, new_cpu_ms, new_mem_kb);
 */

#include <microkit.h>
#include <stdint.h>
#include <stdbool.h>

/* ── Opcodes (must match quota_pd.c) ──────────────────────────────────────── */
#define OP_QUOTA_REGISTER  0x60
#define OP_QUOTA_TICK      0x61
#define OP_QUOTA_STATUS    0x62
#define OP_QUOTA_SET       0x63

/* ── Quota flags (must match quota_pd.c) ──────────────────────────────────── */
#define QUOTA_FLAG_ACTIVE     (1 << 0)
#define QUOTA_FLAG_CPU_EXCEED (1 << 1)
#define QUOTA_FLAG_MEM_EXCEED (1 << 2)
#define QUOTA_FLAG_REVOKED    (1 << 3)

/* ── Status structure ─────────────────────────────────────────────────────── */
typedef struct {
    uint32_t cpu_used_ms;
    uint32_t cpu_limit_ms;
    uint32_t mem_used_kb;
    uint32_t mem_limit_kb;
    uint32_t flags;
} quota_status_t;

/* ── Channel to quota_pd (caller must define or pass) ─────────────────────── */
/* Convention: init_agent uses CH_QUOTA from its channel map */

/*
 * Register an agent with the quota system.
 *
 * @param ch          Channel ID to quota_pd from the caller's perspective
 * @param agent_id    Agent/PD identifier
 * @param cpu_ms      CPU time limit in milliseconds (0 = unlimited)
 * @param mem_kb      Memory limit in kilobytes (0 = unlimited)
 * @return            Slot index on success, negative on error
 */
static inline int quota_register(microkit_channel ch, uint32_t agent_id,
                                  uint32_t cpu_ms, uint32_t mem_kb) {
    microkit_mr_set(0, OP_QUOTA_REGISTER);
    microkit_mr_set(1, agent_id);
    microkit_mr_set(2, cpu_ms);
    microkit_mr_set(3, mem_kb);
    microkit_ppcall(ch, microkit_msginfo_new(0, 4));

    uint32_t slot   = (uint32_t)microkit_mr_get(0);
    uint32_t status = (uint32_t)microkit_mr_get(1);

    if (status == 0xE1) return -1;  /* table full */
    if (slot == 0xFFFFFFFF) return -1;

    return (int)slot;
}

/*
 * Tick an agent's CPU usage.
 * Called once per scheduler round with the delta CPU time consumed.
 *
 * @param ch              Channel ID to quota_pd
 * @param agent_id        Agent/PD identifier
 * @param cpu_delta_ms    CPU time consumed this round (ms)
 * @param mem_current_kb  Current memory usage (kb)
 * @return                Agent flags (check for QUOTA_FLAG_REVOKED)
 */
static inline uint32_t quota_tick(microkit_channel ch, uint32_t agent_id,
                                   uint32_t cpu_delta_ms, uint32_t mem_current_kb) {
    microkit_mr_set(0, OP_QUOTA_TICK);
    microkit_mr_set(1, agent_id);
    microkit_mr_set(2, cpu_delta_ms);
    microkit_mr_set(3, mem_current_kb);
    microkit_ppcall(ch, microkit_msginfo_new(0, 4));

    uint32_t result = (uint32_t)microkit_mr_get(0);
    if (result != 0) return result;  /* error code */

    return (uint32_t)microkit_mr_get(1);  /* flags */
}

/*
 * Query quota status for an agent.
 *
 * @param ch          Channel ID to quota_pd
 * @param agent_id    Agent/PD identifier
 * @param out         Pointer to status structure to fill
 * @return            0 on success, non-zero on error
 */
static inline int quota_query_status(microkit_channel ch, uint32_t agent_id,
                                      quota_status_t *out) {
    microkit_mr_set(0, OP_QUOTA_STATUS);
    microkit_mr_set(1, agent_id);
    microkit_ppcall(ch, microkit_msginfo_new(0, 2));

    /* Check for error (single MR return = error code) */
    uint32_t mr0 = (uint32_t)microkit_mr_get(0);
    if (mr0 == 0xE2) return -1;  /* not found */

    out->cpu_used_ms  = mr0;
    out->cpu_limit_ms = (uint32_t)microkit_mr_get(1);
    out->mem_used_kb  = (uint32_t)microkit_mr_get(2);
    out->mem_limit_kb = (uint32_t)microkit_mr_get(3);
    out->flags        = (uint32_t)microkit_mr_get(4);

    return 0;
}

/*
 * Update quota limits for an existing agent.
 *
 * @param ch          Channel ID to quota_pd
 * @param agent_id    Agent/PD identifier
 * @param new_cpu_ms  New CPU time limit (0 = unlimited)
 * @param new_mem_kb  New memory limit (0 = unlimited)
 * @return            0 on success, non-zero on error
 */
static inline int quota_set(microkit_channel ch, uint32_t agent_id,
                             uint32_t new_cpu_ms, uint32_t new_mem_kb) {
    microkit_mr_set(0, OP_QUOTA_SET);
    microkit_mr_set(1, agent_id);
    microkit_mr_set(2, new_cpu_ms);
    microkit_mr_set(3, new_mem_kb);
    microkit_ppcall(ch, microkit_msginfo_new(0, 4));

    return (int)microkit_mr_get(0);
}

/*
 * Check if an agent's quota has been exceeded.
 *
 * @param flags   Flags returned from quota_tick or quota_query_status
 * @return        true if any quota was exceeded
 */
static inline bool quota_is_exceeded(uint32_t flags) {
    return (flags & (QUOTA_FLAG_CPU_EXCEED | QUOTA_FLAG_MEM_EXCEED)) != 0;
}

/*
 * Check if an agent's capabilities have been revoked due to quota.
 *
 * @param flags   Flags returned from quota_tick or quota_query_status
 * @return        true if caps have been revoked
 */
static inline bool quota_is_revoked(uint32_t flags) {
    return (flags & QUOTA_FLAG_REVOKED) != 0;
}
