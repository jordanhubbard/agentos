/*
 * CapBroker IPC Contract
 *
 * The CapBroker PD manages runtime capability grants between PDs.
 * It enforces the current cap_policy and maintains a grant ledger used by
 * cap_audit_log for attestation.
 *
 * Channel: (controller → cap_broker; mapped via monitor dispatch)
 * Opcodes: MSG_CAP_GRANT, MSG_CAP_REVOKE_GRANT, MSG_CAP_GRANT_STATUS, MSG_CAP_LIST
 *          OP_CAP_BROKER_RELOAD, OP_CAP_STATUS (see agentos.h)
 *
 * Invariants:
 *   - CAP_GRANT requires the granting PD to hold the capability class itself.
 *   - A grant persists until CAP_REVOKE_GRANT or until the target PD exits.
 *   - CAP_LIST enumerates all active grants (not just for a single PD).
 *   - OP_CAP_BROKER_RELOAD atomically updates policy and revokes violating grants.
 *   - CAP_GRANT_STATUS for a PD that has never been granted returns 0 cap_mask.
 *   - All grants are logged to cap_audit_log automatically.
 */

#pragma once
#include "../agentos.h"

/* ─── Request structs ────────────────────────────────────────────────────── */

struct cap_broker_req_grant {
    uint32_t target_pd;         /* PD receiving the capability */
    uint32_t cap_class;         /* AGENTOS_CAP_* constant */
    uint32_t rights;            /* CAP_RIGHT_* bitmask */
    uint32_t ttl_ticks;         /* 0 = permanent until revoke */
};

#define CAP_RIGHT_READ    (1u << 0)
#define CAP_RIGHT_WRITE   (1u << 1)
#define CAP_RIGHT_EXECUTE (1u << 2)

struct cap_broker_req_revoke {
    uint32_t target_pd;
    uint32_t cap_class;
};

struct cap_broker_req_status {
    uint32_t target_pd;
};

struct cap_broker_req_list {
    uint32_t max_entries;       /* max cap_grant_entry_t entries in shmem */
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct cap_broker_reply_grant {
    uint32_t ok;
    uint32_t grant_id;          /* opaque ID for this grant */
};

struct cap_broker_reply_revoke {
    uint32_t ok;
};

struct cap_broker_reply_status {
    uint32_t ok;
    uint32_t cap_mask;          /* AGENTOS_CAP_* bitmask of active grants */
    uint32_t policy_version;    /* current policy version */
    uint32_t active_grants;     /* number of individual grants for this PD */
};

struct cap_broker_reply_list {
    uint32_t ok;
    uint32_t count;             /* entries written to shmem */
};

/* ─── Shmem layout: grant entry ─────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t grant_id;
    uint32_t target_pd;
    uint32_t cap_class;
    uint32_t rights;
    uint32_t ttl_ticks;         /* 0 = permanent */
    uint32_t age_ticks;         /* ticks since grant */
} cap_grant_entry_t;

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum cap_broker_error {
    CAP_BROKER_OK              = 0,
    CAP_BROKER_ERR_NOT_HELD    = 1,  /* granting PD doesn't hold the cap */
    CAP_BROKER_ERR_POLICY_DENY = 2,  /* policy rejects this grant */
    CAP_BROKER_ERR_NOT_FOUND   = 3,  /* grant_id or target_pd not found */
    CAP_BROKER_ERR_TABLE_FULL  = 4,
};
