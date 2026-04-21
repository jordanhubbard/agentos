/*
 * CapAuditLog IPC Contract
 *
 * The CapAuditLog PD maintains a tamper-evident ring of capability grant/revoke
 * events.  It can generate signed attestation reports for external verifiers.
 *
 * Channel: CH_CAP_AUDIT_CTRL (see agentos.h)
 * Opcodes: OP_CAP_LOG, OP_CAP_LOG_STATUS, OP_CAP_LOG_DUMP, OP_CAP_ATTEST
 *
 * Invariants:
 *   - OP_CAP_LOG appends an immutable entry to the ring; entries are never
 *     modified or deleted.
 *   - When the ring is full, new entries overwrite the oldest (rolling window).
 *   - OP_CAP_ATTEST uses Ed25519 to sign the current ring state; the signing
 *     key is provisioned at boot from the boot integrity PD.
 *   - OP_CAP_LOG_DUMP copies entries to the shared cap_audit_shmem region.
 *   - All opcodes are idempotent with respect to dump and status queries.
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs ────────────────────────────────────────────────────────── */
#define CAP_AUDIT_CH_CONTROLLER   CH_CAP_AUDIT_CTRL
#define CAP_AUDIT_CH_INIT_AGENT   CH_CAP_AUDIT_INIT

/* ─── Request structs ────────────────────────────────────────────────────── */

struct cap_audit_req_log {
    uint32_t event_type;        /* CAP_EVENT_GRANT / CAP_EVENT_REVOKE / ... */
    uint32_t agent_id;          /* agent receiving or losing the capability */
    uint32_t caps_mask;         /* AGENTOS_CAP_* bitmask */
    uint32_t slot_id;           /* associated swap slot (0 = N/A) */
};

struct cap_audit_req_status {
    /* no fields */
};

struct cap_audit_req_dump {
    uint32_t start_seq;         /* first sequence number to return (0 = oldest) */
    uint32_t max_entries;       /* max entries to copy to shmem */
};

struct cap_audit_req_attest {
    /* no fields — signs current ring state */
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct cap_audit_reply_log {
    uint32_t ok;
    uint64_t seq;               /* sequence number assigned to this entry */
};

struct cap_audit_reply_status {
    uint32_t ok;
    uint64_t total_entries;     /* since boot (may exceed ring capacity) */
    uint32_t ring_used;         /* entries currently in ring */
    uint32_t ring_capacity;
};

struct cap_audit_reply_dump {
    uint32_t ok;
    uint32_t count;             /* entries written to shmem */
    uint64_t next_seq;          /* seq of next entry after this dump */
};

struct cap_audit_reply_attest {
    uint32_t ok;
    uint8_t  signature[64];     /* Ed25519 signature over ring state */
};

/* ─── Shmem layout: audit log entry ─────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint64_t seq;
    uint64_t timestamp_ns;
    uint32_t event_type;
    uint32_t agent_id;
    uint32_t caps_mask;
    uint32_t slot_id;
} cap_audit_entry_t;

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum cap_audit_error {
    CAP_AUDIT_OK              = 0,
    CAP_AUDIT_ERR_NOT_INIT    = 1,
    CAP_AUDIT_ERR_NO_KEY      = 2,  /* signing key not provisioned */
    CAP_AUDIT_ERR_BAD_SEQ     = 3,  /* start_seq > current head */
};
