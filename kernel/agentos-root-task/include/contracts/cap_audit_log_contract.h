#pragma once
/* CAP_AUDIT_LOG contract — version 1
 * PD: cap_audit_log | Source: src/cap_audit_log.c | Channel: CH_CAP_AUDIT_CTRL=57 (from controller)
 */
#include <stdint.h>
#include <stdbool.h>

#define CAP_AUDIT_LOG_CONTRACT_VERSION 1

/* ── Channel IDs (controller perspective) ── */
#define CH_CAP_AUDIT_CTRL          57   /* controller -> cap_audit_log (PPC); cross-ref: agentos.h */
#define CH_CAP_AUDIT_INIT           5   /* init_agent -> cap_audit_log (from init_agent perspective) */

/* ── Opcodes ── */
#define CAP_AUDIT_OP_LOG           0x50u  /* log a capability grant or revoke event */
#define CAP_AUDIT_OP_STATUS        0x51u  /* query ring buffer fill level and stats */
#define CAP_AUDIT_OP_DUMP          0x52u  /* read entries from ring into shared memory */
#define CAP_AUDIT_OP_ATTEST        0x53u  /* generate signed attestation report */

/* ── Capability event types (cross-ref: agentos.h CAP_EVENT_*) ── */
#define CAP_AUDIT_EVENT_GRANT       1u  /* capability granted to a PD */
#define CAP_AUDIT_EVENT_REVOKE      2u  /* capability revoked from a PD */
#define CAP_AUDIT_EVENT_DELEGATE    3u  /* capability delegated between PDs */
#define CAP_AUDIT_EVENT_MINT        4u  /* new capability minted by root task */
#define CAP_AUDIT_EVENT_POLICY_RELOAD 8u /* policy hot-reload event */

/* ── Request / Reply structs ── */

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* CAP_AUDIT_OP_LOG */
    uint32_t event_type;      /* CAP_AUDIT_EVENT_* */
    uint32_t agent_id_lo;     /* lower 32 bits of agent ID involved */
    uint32_t agent_id_hi;     /* upper 32 bits of agent ID involved */
    uint32_t cap_class;       /* AGENTOS_CAP_* bitmask */
    uint32_t slot_id;         /* slot or policy version (event-dependent) */
    uint64_t timestamp_ns;    /* event timestamp */
} cap_audit_req_log_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok, else cap_audit_error_t */
    uint32_t entry_seq;       /* monotonic sequence number assigned to this entry */
} cap_audit_reply_log_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* CAP_AUDIT_OP_STATUS */
} cap_audit_req_status_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t ring_capacity;   /* total ring entries */
    uint32_t ring_used;       /* entries currently in ring */
    uint64_t total_logged;    /* lifetime event count */
    uint32_t overflow_count;  /* events dropped due to ring full */
} cap_audit_reply_status_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* CAP_AUDIT_OP_DUMP */
    uint32_t start_seq;       /* first sequence number to retrieve */
    uint32_t max_entries;     /* maximum entries to copy to shmem */
    uint32_t shmem_offset;    /* byte offset in shared region for output */
} cap_audit_req_dump_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t entry_count;     /* number of cap_audit_entry_t written to shmem */
    uint32_t next_seq;        /* sequence number of next unread entry */
} cap_audit_reply_dump_t;

/* Entry format written into shmem during DUMP */
typedef struct __attribute__((packed)) {
    uint64_t seq;             /* monotonic sequence number */
    uint64_t timestamp_ns;
    uint32_t event_type;      /* CAP_AUDIT_EVENT_* */
    uint32_t agent_id_lo;
    uint32_t agent_id_hi;
    uint32_t cap_class;       /* AGENTOS_CAP_* */
    uint32_t slot_id;
    uint32_t _pad;
} cap_audit_entry_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* CAP_AUDIT_OP_ATTEST */
    uint32_t shmem_offset;    /* byte offset for attestation report output */
} cap_audit_req_attest_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t report_size;     /* bytes written to shmem */
    uint64_t report_hash_lo;  /* SHA-256 of report (for verification) */
    uint64_t report_hash_hi;
} cap_audit_reply_attest_t;

/* ── Error codes ── */
typedef enum {
    CAP_AUDIT_OK              = 0,
    CAP_AUDIT_ERR_RING_FULL   = 1,  /* ring full; entry dropped */
    CAP_AUDIT_ERR_BAD_SEQ     = 2,  /* start_seq already overwritten */
    CAP_AUDIT_ERR_NO_SHMEM    = 3,  /* shared memory region not mapped */
    CAP_AUDIT_ERR_NO_CAP      = 4,  /* caller lacks AGENTOS_CAP_AUDIT */
    CAP_AUDIT_ERR_ATTEST_FAIL = 5,  /* cryptographic attestation failed */
} cap_audit_error_t;

/* ── Invariants ──
 * - All capability lifecycle events must be logged; logging failures are themselves logged.
 * - seq is strictly monotonically increasing; gaps indicate overflow.
 * - Callers of CAP_AUDIT_OP_DUMP must hold AGENTOS_CAP_AUDIT.
 * - ATTEST produces a signed report over all entries since last attestation.
 * - Ring overflow increments overflow_count; oldest entries are evicted.
 */
