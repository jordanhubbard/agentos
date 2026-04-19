/*
 * agentOS CapStore IPC Contract — interface.h
 *
 * Formal seL4 IPC API contract for the CapStore protection domain.
 * CapStore is the semantic capability database.  It tracks every
 * capability derivation, grant, and revocation at the agentOS application
 * layer (above seL4's own CNode machinery).  Every capability check in the
 * system is audited here.
 *
 * IPC mechanism: seL4_Call / seL4_Reply over badged endpoints.
 * MR0 carries the opcode on request; MR0 carries the status on reply.
 * Bulk parameters (labels, owner IDs) are packed inline into the request
 * structs; these fit within the seL4 IPC message register budget (120 bytes).
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/* ── Version ─────────────────────────────────────────────────────────────── */

#define CAPSTORE_INTERFACE_VERSION  1

/* ── Limits ──────────────────────────────────────────────────────────────── */

#define CAPSTORE_MAX_ENTRIES        4096
#define CAPSTORE_AGENT_ID_BYTES     32    /* AgentID is 32 bytes (SHA-256) */
#define CAPSTORE_LABEL_MAX          64

/* ── Capability types ────────────────────────────────────────────────────── */

#define CAPSTORE_CAP_TOOL           0x01u  /* invoke a registered tool */
#define CAPSTORE_CAP_MODEL          0x02u  /* call an inference endpoint */
#define CAPSTORE_CAP_MEMORY         0x03u  /* heap allocation budget */
#define CAPSTORE_CAP_MSG            0x04u  /* publish/subscribe to MsgBus */
#define CAPSTORE_CAP_STORE          0x05u  /* read/write to AgentFS */
#define CAPSTORE_CAP_SPAWN          0x06u  /* spawn child agents */
#define CAPSTORE_CAP_NET            0x07u  /* network access */
#define CAPSTORE_CAP_SELF           0x08u  /* introspect own capabilities */
#define CAPSTORE_CAP_SERVICE        0x09u  /* bind to a named service endpoint */

/* ── Rights bitmask ──────────────────────────────────────────────────────── */

#define CAPSTORE_RIGHT_READ         (1u << 0)
#define CAPSTORE_RIGHT_WRITE        (1u << 1)
#define CAPSTORE_RIGHT_GRANT        (1u << 2)  /* can derive sub-caps */
#define CAPSTORE_RIGHT_ALL          0x07u

/* ── Capability status flags (returned in capstore_entry_info_t) ────────── */

#define CAPSTORE_FLAG_REVOKED       (1u << 0)
#define CAPSTORE_FLAG_DELEGATABLE   (1u << 1)

/* ── Opcodes ─────────────────────────────────────────────────────────────── */

#define CAPSTORE_OP_REGISTER        0x200u  /* create root capability */
#define CAPSTORE_OP_DERIVE          0x201u  /* derive sub-capability */
#define CAPSTORE_OP_REVOKE          0x202u  /* revoke cap and descendants */
#define CAPSTORE_OP_CHECK           0x203u  /* check if cap is valid */
#define CAPSTORE_OP_QUERY_OWNER     0x204u  /* list caps owned by agent */
#define CAPSTORE_OP_INFO            0x205u  /* retrieve metadata for a cap */
#define CAPSTORE_OP_HEALTH          0x206u  /* liveness probe */

/* ── Error codes ─────────────────────────────────────────────────────────── */

#define CAPSTORE_ERR_OK             0u
#define CAPSTORE_ERR_INVALID_ARG    1u   /* bad opcode or null field */
#define CAPSTORE_ERR_NOT_FOUND      2u   /* cap_id does not exist */
#define CAPSTORE_ERR_REVOKED        3u   /* capability has been revoked */
#define CAPSTORE_ERR_DENIED         4u   /* rights escalation attempt */
#define CAPSTORE_ERR_NOMEM          5u   /* entry table full */
#define CAPSTORE_ERR_INTERNAL       99u

/* ── Inline cap info structure (returned by CAPSTORE_OP_INFO) ────────────── */

typedef struct capstore_entry_info {
    uint64_t cap_id;                        /* unique capability identifier */
    uint64_t parent_id;                     /* parent cap; 0 for root */
    uint8_t  owner[CAPSTORE_AGENT_ID_BYTES];
    uint8_t  creator[CAPSTORE_AGENT_ID_BYTES];
    uint32_t type;                          /* CAPSTORE_CAP_* */
    uint32_t rights;                        /* CAPSTORE_RIGHT_* bitmask */
    uint32_t flags;                         /* CAPSTORE_FLAG_* bitmask */
    uint64_t created_at;                    /* monotonic microseconds */
    char     label[CAPSTORE_LABEL_MAX];     /* human-readable label */
} __attribute__((packed)) capstore_entry_info_t;

/* ── Request / reply structs ─────────────────────────────────────────────── */

/*
 * CAPSTORE_OP_REGISTER
 *
 * Create a root capability (no parent) for the given owner agent.
 * Only the init task (badge == 0) or the controller may call this.
 *
 * Request:  opcode, type, rights, owner (32 bytes), label
 * Reply:    status, cap_id
 */
typedef struct capstore_register_req {
    uint32_t opcode;                        /* CAPSTORE_OP_REGISTER */
    uint32_t type;                          /* CAPSTORE_CAP_* */
    uint32_t rights;                        /* CAPSTORE_RIGHT_* */
    uint32_t _pad;
    uint8_t  owner[CAPSTORE_AGENT_ID_BYTES];
    char     label[CAPSTORE_LABEL_MAX];
} __attribute__((packed)) capstore_register_req_t;

typedef struct capstore_register_rep {
    uint32_t status;                        /* CAPSTORE_ERR_* */
    uint32_t _pad;
    uint64_t cap_id;
} __attribute__((packed)) capstore_register_rep_t;

/*
 * CAPSTORE_OP_DERIVE
 *
 * Derive a new capability from an existing one.
 * The new rights must be a subset of the parent's rights.
 * The caller must hold the parent with CAPSTORE_RIGHT_GRANT.
 *
 * Request:  opcode, rights, parent_id, new_owner (32 bytes)
 * Reply:    status, new_cap_id
 */
typedef struct capstore_derive_req {
    uint32_t opcode;                        /* CAPSTORE_OP_DERIVE */
    uint32_t rights;                        /* subset of parent rights */
    uint64_t parent_id;
    uint8_t  new_owner[CAPSTORE_AGENT_ID_BYTES];
} __attribute__((packed)) capstore_derive_req_t;

typedef struct capstore_derive_rep {
    uint32_t status;
    uint32_t _pad;
    uint64_t new_cap_id;
} __attribute__((packed)) capstore_derive_rep_t;

/*
 * CAPSTORE_OP_REVOKE
 *
 * Revoke a capability and cascade to all descendants.
 * Only the cap's creator, its parent's owner, or init may revoke.
 *
 * Request:  opcode, cap_id
 * Reply:    status, revoked_count (includes descendants)
 */
typedef struct capstore_revoke_req {
    uint32_t opcode;                        /* CAPSTORE_OP_REVOKE */
    uint32_t _pad;
    uint64_t cap_id;
} __attribute__((packed)) capstore_revoke_req_t;

typedef struct capstore_revoke_rep {
    uint32_t status;
    uint32_t revoked_count;
} __attribute__((packed)) capstore_revoke_rep_t;

/*
 * CAPSTORE_OP_CHECK
 *
 * Check whether a capability is valid and the caller holds the given rights.
 * Returns CAPSTORE_ERR_OK if valid; CAPSTORE_ERR_REVOKED or
 * CAPSTORE_ERR_DENIED otherwise.
 *
 * Request:  opcode, rights, cap_id, requester (32 bytes)
 * Reply:    status
 */
typedef struct capstore_check_req {
    uint32_t opcode;                        /* CAPSTORE_OP_CHECK */
    uint32_t rights;
    uint64_t cap_id;
    uint8_t  requester[CAPSTORE_AGENT_ID_BYTES];
} __attribute__((packed)) capstore_check_req_t;

typedef struct capstore_check_rep {
    uint32_t status;
} __attribute__((packed)) capstore_check_rep_t;

/*
 * CAPSTORE_OP_QUERY_OWNER
 *
 * Enumerate all live capabilities owned by an agent.
 * The cap_id list is written into the caller's shmem MR
 * (array of uint64_t, up to buf_count entries).
 *
 * Request:  opcode, buf_count, buf_shmem_offset, owner (32 bytes)
 * Reply:    status, found_count
 */
typedef struct capstore_query_owner_req {
    uint32_t opcode;                        /* CAPSTORE_OP_QUERY_OWNER */
    uint32_t buf_count;                     /* max cap_ids to write */
    uint32_t buf_shmem_offset;
    uint32_t _pad;
    uint8_t  owner[CAPSTORE_AGENT_ID_BYTES];
} __attribute__((packed)) capstore_query_owner_req_t;

typedef struct capstore_query_owner_rep {
    uint32_t status;
    uint32_t found_count;
} __attribute__((packed)) capstore_query_owner_rep_t;

/*
 * CAPSTORE_OP_INFO
 *
 * Retrieve full metadata for a capability.
 * The capstore_entry_info_t is written into the caller's shmem MR
 * at info_shmem_offset.
 *
 * Request:  opcode, info_shmem_offset, cap_id
 * Reply:    status
 */
typedef struct capstore_info_req {
    uint32_t opcode;                        /* CAPSTORE_OP_INFO */
    uint32_t info_shmem_offset;
    uint64_t cap_id;
} __attribute__((packed)) capstore_info_req_t;

typedef struct capstore_info_rep {
    uint32_t status;
} __attribute__((packed)) capstore_info_rep_t;

/*
 * CAPSTORE_OP_HEALTH
 *
 * Liveness probe.
 *
 * Request:  opcode
 * Reply:    status, entry_count, version
 */
typedef struct capstore_health_req {
    uint32_t opcode;                        /* CAPSTORE_OP_HEALTH */
} __attribute__((packed)) capstore_health_req_t;

typedef struct capstore_health_rep {
    uint32_t status;
    uint32_t entry_count;
    uint32_t version;                       /* CAPSTORE_INTERFACE_VERSION */
} __attribute__((packed)) capstore_health_rep_t;
