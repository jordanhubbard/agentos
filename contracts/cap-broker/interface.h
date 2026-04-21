/*
 * agentOS CapabilityBroker IPC Contract — interface.h
 *
 * Formal seL4 IPC API contract for the Capability Broker protection domain.
 * The capability broker is the only PD permitted to perform CNode operations
 * that move seL4 capabilities between address spaces.  All inter-PD
 * capability delegation flows through here.
 *
 * The broker enforces an ACL / RBAC policy: a (grantor, CapKind) pair must
 * appear in the policy table before the grantor can issue grants of that kind.
 * Policy is loaded from AgentFS at boot and can be hot-reloaded via
 * OP_CAP_BROKER_RELOAD (0x15, dispatched by monitor).
 *
 * Capabilities are identified by opaque 64-bit GrantTokens.  A token can be
 * delegated (sub-granted) if the Delegatable flag is set; derived rights
 * must be a subset of the parent's rights.
 *
 * IPC mechanism: seL4_Call / seL4_Reply.
 * MR0 carries the opcode on request; MR0 carries the status on reply.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/* ── Version ─────────────────────────────────────────────────────────────── */

#define CAP_BROKER_INTERFACE_VERSION  1

/* ── Rights bitmask ──────────────────────────────────────────────────────── */

#define CAP_BROKER_RIGHT_READ   0x01u
#define CAP_BROKER_RIGHT_WRITE  0x02u
#define CAP_BROKER_RIGHT_GRANT  0x04u
#define CAP_BROKER_RIGHT_ALL    0x07u

/* ── Capability kind values ──────────────────────────────────────────────── */

#define CAP_KIND_MEMORY         0u
#define CAP_KIND_ENDPOINT       1u
#define CAP_KIND_NOTIFICATION   2u
#define CAP_KIND_IRQ_HANDLER    3u
#define CAP_KIND_FRAME          4u
/* Custom kinds: 0x80 and above (opaque string ID passed in kind_name field) */
#define CAP_KIND_CUSTOM_BASE    0x80u

/* ── Opcodes ─────────────────────────────────────────────────────────────── */

/* High-level broker API (this contract) */
#define CAP_BROKER_OP_ISSUE         0x700u  /* issue new grant token */
#define CAP_BROKER_OP_VALIDATE      0x701u  /* validate token + rights */
#define CAP_BROKER_OP_DELEGATE      0x702u  /* delegate to third party */
#define CAP_BROKER_OP_REVOKE        0x703u  /* revoke token */
#define CAP_BROKER_OP_GRANT         0x704u  /* store agent+kind grant */
#define CAP_BROKER_OP_CHECK         0x705u  /* check agent+kind grant */
#define CAP_BROKER_OP_REVOKE_BY_AC  0x706u  /* revoke all grants for agent+kind */
#define CAP_BROKER_OP_AUDIT_RECENT  0x707u  /* fetch recent audit entries */
#define CAP_BROKER_OP_HEALTH        0x708u  /* liveness probe */

/* Low-level seL4 dispatch opcodes (MR0 for PPCs into monitor PD) */
#define OP_CAP_BROKER_RELOAD        0x15u   /* hot-reload policy blob */
#define OP_CAP_STATUS               0x16u   /* query cap count / policy version */

/* ── Error codes ─────────────────────────────────────────────────────────── */

#define CAP_BROKER_ERR_OK               0u
#define CAP_BROKER_ERR_UNKNOWN_TOKEN    1u   /* token not known / revoked */
#define CAP_BROKER_ERR_INSUFF_RIGHTS    2u   /* caller lacks required rights */
#define CAP_BROKER_ERR_POLICY_DENIED    3u   /* ACL does not allow this grant */
#define CAP_BROKER_ERR_NOT_DELEGATABLE  4u   /* token has delegatable=false */
#define CAP_BROKER_ERR_TABLE_FULL       5u   /* grant table at 1024 limit */
#define CAP_BROKER_ERR_INVALID_ARG      6u   /* null field or bad kind */
#define CAP_BROKER_ERR_EXPIRED          7u   /* grant has passed expires_at */
#define CAP_BROKER_ERR_INTERNAL         99u

/* ── Audit operation codes (returned in audit entries) ───────────────────── */

#define CAP_BROKER_AUDIT_GRANT   0u
#define CAP_BROKER_AUDIT_CHECK   1u
#define CAP_BROKER_AUDIT_REVOKE  2u

/* ── Audit result codes ──────────────────────────────────────────────────── */

#define CAP_BROKER_AUDIT_ALLOWED    0u
#define CAP_BROKER_AUDIT_DENIED     1u
#define CAP_BROKER_AUDIT_NOT_FOUND  2u

/* ── Audit entry structure (returned by CAP_BROKER_OP_AUDIT_RECENT) ──────── */

#define CAP_BROKER_AGENT_ID_MAX  64   /* string agent identifier */
#define CAP_BROKER_KIND_NAME_MAX 64

typedef struct cap_broker_audit_entry {
    uint64_t timestamp_ms;
    uint32_t op;                            /* CAP_BROKER_AUDIT_* */
    uint32_t result;                        /* CAP_BROKER_AUDIT_ALLOWED/DENIED/NOT_FOUND */
    uint32_t cap_kind;                      /* CAP_KIND_* or CAP_KIND_CUSTOM_BASE+N */
    uint32_t _pad;
    char     agent_id[CAP_BROKER_AGENT_ID_MAX];
    char     kind_name[CAP_BROKER_KIND_NAME_MAX]; /* non-empty for custom kinds */
} __attribute__((packed)) cap_broker_audit_entry_t;

/* ── Request / reply structs ─────────────────────────────────────────────── */

/*
 * CAP_BROKER_OP_ISSUE
 *
 * Issue a new capability grant token from grantor to grantee.
 * The grantor must appear in the policy table for the given cap_kind.
 *
 * Request:  opcode, cap_kind, rights, delegatable,
 *           grantor (NUL-term string), grantee (NUL-term string)
 * Reply:    status, grant_token
 */
typedef struct cap_broker_issue_req {
    uint32_t opcode;                        /* CAP_BROKER_OP_ISSUE */
    uint32_t cap_kind;                      /* CAP_KIND_* */
    uint32_t rights;                        /* CAP_BROKER_RIGHT_* */
    uint32_t delegatable;                   /* 0 = no, 1 = yes */
    char     grantor[CAP_BROKER_AGENT_ID_MAX];
    char     grantee[CAP_BROKER_AGENT_ID_MAX];
} __attribute__((packed)) cap_broker_issue_req_t;

typedef struct cap_broker_issue_rep {
    uint32_t status;                        /* CAP_BROKER_ERR_* */
    uint32_t _pad;
    uint64_t grant_token;
} __attribute__((packed)) cap_broker_issue_rep_t;

/*
 * CAP_BROKER_OP_VALIDATE
 *
 * Validate that holder holds grant_token with at least required rights.
 *
 * Request:  opcode, rights, grant_token, holder
 * Reply:    status, delegatable, cap_kind
 */
typedef struct cap_broker_validate_req {
    uint32_t opcode;                        /* CAP_BROKER_OP_VALIDATE */
    uint32_t rights;
    uint64_t grant_token;
    char     holder[CAP_BROKER_AGENT_ID_MAX];
} __attribute__((packed)) cap_broker_validate_req_t;

typedef struct cap_broker_validate_rep {
    uint32_t status;
    uint32_t cap_kind;
    uint32_t delegatable;
} __attribute__((packed)) cap_broker_validate_rep_t;

/*
 * CAP_BROKER_OP_DELEGATE
 *
 * Derive a new grant token from an existing one for a new grantee.
 * Derived rights must be a subset of the parent's rights.
 * The derived grant is always non-delegatable (one level of delegation).
 *
 * Request:  opcode, rights, parent_token, current_holder, new_grantee
 * Reply:    status, new_token
 */
typedef struct cap_broker_delegate_req {
    uint32_t opcode;                        /* CAP_BROKER_OP_DELEGATE */
    uint32_t rights;
    uint64_t parent_token;
    char     current_holder[CAP_BROKER_AGENT_ID_MAX];
    char     new_grantee[CAP_BROKER_AGENT_ID_MAX];
} __attribute__((packed)) cap_broker_delegate_req_t;

typedef struct cap_broker_delegate_rep {
    uint32_t status;
    uint32_t _pad;
    uint64_t new_token;
} __attribute__((packed)) cap_broker_delegate_rep_t;

/*
 * CAP_BROKER_OP_REVOKE
 *
 * Revoke a token-based capability grant.
 *
 * Request:  opcode, grant_token
 * Reply:    status
 */
typedef struct cap_broker_revoke_req {
    uint32_t opcode;                        /* CAP_BROKER_OP_REVOKE */
    uint32_t _pad;
    uint64_t grant_token;
} __attribute__((packed)) cap_broker_revoke_req_t;

typedef struct cap_broker_revoke_rep {
    uint32_t status;
} __attribute__((packed)) cap_broker_revoke_rep_t;

/*
 * CAP_BROKER_OP_GRANT
 *
 * Store an agent+kind capability grant with expiry.
 * expires_at_ms: monotonic millisecond timestamp; 0xFFFFFFFFFFFFFFFF = never.
 *
 * Request:  opcode, cap_kind, rights, expires_at_ms, now_ms, agent_id
 * Reply:    status
 */
typedef struct cap_broker_grant_req {
    uint32_t opcode;                        /* CAP_BROKER_OP_GRANT */
    uint32_t cap_kind;
    uint32_t rights;
    uint32_t _pad;
    uint64_t expires_at_ms;
    uint64_t now_ms;
    char     agent_id[CAP_BROKER_AGENT_ID_MAX];
} __attribute__((packed)) cap_broker_grant_req_t;

typedef struct cap_broker_grant_rep {
    uint32_t status;
} __attribute__((packed)) cap_broker_grant_rep_t;

/*
 * CAP_BROKER_OP_CHECK
 *
 * Check whether agent_id holds cap_kind with at least required rights
 * and the grant has not expired.
 *
 * Request:  opcode, cap_kind, rights, now_ms, agent_id
 * Reply:    status (ALLOWED = CAP_BROKER_ERR_OK, DENIED = CAP_BROKER_ERR_INSUFF_RIGHTS)
 */
typedef struct cap_broker_check_req {
    uint32_t opcode;                        /* CAP_BROKER_OP_CHECK */
    uint32_t cap_kind;
    uint32_t rights;
    uint32_t _pad;
    uint64_t now_ms;
    char     agent_id[CAP_BROKER_AGENT_ID_MAX];
} __attribute__((packed)) cap_broker_check_req_t;

typedef struct cap_broker_check_rep {
    uint32_t status;
    uint32_t allowed;                       /* 1 = allowed, 0 = denied */
} __attribute__((packed)) cap_broker_check_rep_t;

/*
 * CAP_BROKER_OP_AUDIT_RECENT
 *
 * Fetch the last n audit entries.  Written as an array of
 * cap_broker_audit_entry_t into the caller's shmem MR at buf_shmem_offset.
 *
 * Request:  opcode, max_entries, buf_shmem_offset, buf_len
 * Reply:    status, entry_count, bytes_written
 */
typedef struct cap_broker_audit_recent_req {
    uint32_t opcode;                        /* CAP_BROKER_OP_AUDIT_RECENT */
    uint32_t max_entries;
    uint32_t buf_shmem_offset;
    uint32_t buf_len;
} __attribute__((packed)) cap_broker_audit_recent_req_t;

typedef struct cap_broker_audit_recent_rep {
    uint32_t status;
    uint32_t entry_count;
    uint32_t bytes_written;
} __attribute__((packed)) cap_broker_audit_recent_rep_t;

/*
 * CAP_BROKER_OP_HEALTH
 *
 * Liveness probe.
 *
 * Request:  opcode
 * Reply:    status, grant_count, version
 */
typedef struct cap_broker_health_req {
    uint32_t opcode;                        /* CAP_BROKER_OP_HEALTH */
} __attribute__((packed)) cap_broker_health_req_t;

typedef struct cap_broker_health_rep {
    uint32_t status;
    uint32_t grant_count;
    uint32_t version;                       /* CAP_BROKER_INTERFACE_VERSION */
} __attribute__((packed)) cap_broker_health_rep_t;
