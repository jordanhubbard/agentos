#pragma once
/* CAP_BROKER contract — version 1
 * PD: cap_broker (implemented within controller/monitor) | Source: src/cap_policy.c
 * Channel: dispatched by monitor on PPCs; no separate PD channel
 */
#include <stdint.h>
#include <stdbool.h>

#define CAP_BROKER_CONTRACT_VERSION 1

/* ── Opcodes ── */
#define CAP_BROKER_OP_RELOAD       0x15u  /* hot-reload policy blob; revoke violating grants atomically */
#define CAP_BROKER_OP_STATUS       0x16u  /* query: cap_count, policy_version, active_grants */

/* cap_policy sub-opcodes (dispatched from cap_broker to cap_policy PD) */
#define CAP_POLICY_OP_RELOAD       0xC0u  /* fetch+parse+validate new policy from AgentFS */
#define CAP_POLICY_OP_STATUS       0xC1u  /* -> loaded, version, count, hash */
#define CAP_POLICY_OP_RESET        0xC2u  /* revert to static compile-time policy */
#define CAP_POLICY_OP_DIFF         0xC3u  /* -> revoked, classes, version */

/* ── Capability class bitmask (cross-ref: agentos.h CAP_CLASS_*) ── */
#define CAP_CLASS_FS               (1u << 0)
#define CAP_CLASS_NET              (1u << 1)
#define CAP_CLASS_GPU              (1u << 2)
#define CAP_CLASS_IPC              (1u << 3)
#define CAP_CLASS_TIMER            (1u << 4)
#define CAP_CLASS_STDIO            (1u << 5)
#define CAP_CLASS_SPAWN            (1u << 6)
#define CAP_CLASS_SWAP             (1u << 7)

/* ── Fine-grained capability constants (cross-ref: agentos.h AGENTOS_CAP_*) ── */
#define CAP_COMPUTE                0x01u
#define CAP_MEMORY                 0x02u
#define CAP_OBJECTSTORE            0x04u
#define CAP_NETWORK                0x08u
#define CAP_SPAWN                  0x10u
#define CAP_AUDIT                  0x20u
#define CAP_SWAP_WRITE             0x40u
#define CAP_SWAP_READ              0x80u

/* ── Request / Reply structs ── */

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* CAP_BROKER_OP_RELOAD */
    uint64_t policy_hash_lo;  /* AgentFS hash of new policy blob */
    uint64_t policy_hash_hi;
    uint32_t flags;           /* CAP_BROKER_RELOAD_FLAG_* */
} cap_broker_req_reload_t;

#define CAP_BROKER_RELOAD_FLAG_ATOMIC   (1u << 0)  /* revoke all violating grants atomically */
#define CAP_BROKER_RELOAD_FLAG_DRY_RUN  (1u << 1)  /* compute diff without applying */

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok, else cap_broker_error_t */
    uint32_t policy_version;  /* new policy version number */
    uint32_t grants_revoked;  /* number of grants revoked during reload */
    uint32_t classes_affected; /* CAP_CLASS_* bitmask of affected classes */
} cap_broker_reply_reload_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* CAP_BROKER_OP_STATUS */
} cap_broker_req_status_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t cap_count;       /* total capabilities currently managed */
    uint32_t policy_version;  /* version of currently active policy */
    uint32_t active_grants;   /* number of currently valid grant records */
    uint32_t revoke_count;    /* total revocations since boot */
} cap_broker_reply_status_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* CAP_POLICY_OP_DIFF */
} cap_policy_req_diff_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t grants_would_revoke; /* grants that would be revoked by pending reload */
    uint32_t classes_mask;    /* CAP_CLASS_* bitmask of affected classes */
    uint32_t new_version;     /* version of pending policy */
} cap_policy_reply_diff_t;

/* ── Grant record (used internally; exposed for audit) ── */
typedef struct __attribute__((packed)) {
    uint64_t agent_id_hi;
    uint64_t agent_id_lo;
    uint32_t slot_id;
    uint32_t cap_mask;        /* AGENTOS_CAP_* granted */
    uint32_t policy_version;  /* policy version at time of grant */
    uint32_t flags;           /* CAP_GRANT_FLAG_* */
} cap_broker_grant_record_t;

#define CAP_GRANT_FLAG_DELEGATED    (1u << 0)  /* capability was delegated, not minted */
#define CAP_GRANT_FLAG_REVOKED      (1u << 1)  /* grant has been revoked */

/* ── Error codes ── */
typedef enum {
    CAP_BROKER_OK             = 0,
    CAP_BROKER_ERR_NOT_FOUND  = 1,  /* policy hash not in AgentFS */
    CAP_BROKER_ERR_BAD_POLICY = 2,  /* policy blob failed validation */
    CAP_BROKER_ERR_VERSION    = 3,  /* new version is not greater than current */
    CAP_BROKER_ERR_NO_CAP     = 4,  /* caller lacks privilege to reload policy */
    CAP_BROKER_ERR_BUSY       = 5,  /* reload already in progress */
} cap_broker_error_t;

/* ── Invariants ──
 * - Policy version must be monotonically increasing; downgrade is rejected.
 * - CAP_BROKER_RELOAD_FLAG_ATOMIC guarantees that either all violating grants are revoked
 *   or none are (no partial revocation).
 * - DRY_RUN does not modify any state; it only returns the diff.
 * - All grant/revoke events are forwarded to cap_audit_log for the audit trail.
 * - policy_version=0 means the compile-time static policy is active.
 */
