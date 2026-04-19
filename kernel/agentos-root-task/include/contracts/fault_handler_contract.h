#pragma once
/* FAULT_HANDLER contract — version 1
 * PD: fault_handler | Source: src/fault_handler.c | Channel: (receives fault IPCs from seL4 kernel)
 */
#include <stdint.h>
#include <stdbool.h>

#define FAULT_HANDLER_CONTRACT_VERSION 1

/* ── Opcodes ── */
#define FAULT_HANDLER_OP_NOTIFY        0xE0u  /* seL4 fault notification delivered to handler */
#define FAULT_HANDLER_OP_QUERY         0xE1u  /* query fault record for a slot */
#define FAULT_HANDLER_OP_HISTORY       0xE2u  /* retrieve fault history for a slot */
#define FAULT_HANDLER_OP_POLICY_SET    0xE3u  /* set restart policy for a slot */
#define FAULT_HANDLER_OP_CLEAR         0xE4u  /* clear fault record and restart counter */

/* ── Fault kinds ── */
#define FAULT_KIND_VM_FAULT            0u  /* page fault / access violation */
#define FAULT_KIND_CAP_FAULT           1u  /* invalid capability access */
#define FAULT_KIND_UNKNOWN_SYSCALL     2u  /* unrecognized system call */
#define FAULT_KIND_WASM_TRAP           3u  /* WASM execution trap */
#define FAULT_KIND_STACK_OVERFLOW      4u  /* guard page hit */
#define FAULT_KIND_TIMEOUT             5u  /* scheduling timeout fault */

/* ── Restart policy flags ── */
#define FAULT_POLICY_RESTART           (1u << 0)  /* restart slot on fault */
#define FAULT_POLICY_ESCALATE          (1u << 1)  /* escalate to controller after max_restarts */
#define FAULT_POLICY_COREDUMP          (1u << 2)  /* write core dump to AgentFS on fault */

/* ── Default policy constants (cross-ref: agentos.h) ── */
#define FAULT_POLICY_MAX_RESTARTS_DEFAULT   3u
#define FAULT_POLICY_RESTART_DELAY_MS     100u
#define FAULT_POLICY_ESCALATE_AFTER         5u

/* ── Request / Reply structs ── */

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* FAULT_HANDLER_OP_NOTIFY */
    uint32_t slot_id;         /* faulting PD slot */
    uint32_t fault_kind;      /* FAULT_KIND_* */
    uint64_t fault_addr;      /* faulting virtual address (if applicable) */
    uint64_t fault_ip;        /* instruction pointer at fault */
    uint32_t fault_info;      /* architecture-specific fault status word */
} fault_handler_req_notify_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok (handler took action) */
    uint32_t action;          /* FAULT_ACTION_* taken */
} fault_handler_reply_notify_t;

#define FAULT_ACTION_RESTARTED         0u  /* slot was restarted */
#define FAULT_ACTION_ESCALATED         1u  /* escalated to controller */
#define FAULT_ACTION_KILLED            2u  /* slot was terminated */
#define FAULT_ACTION_IGNORED           3u  /* fault logged but no action */

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* FAULT_HANDLER_OP_QUERY */
    uint32_t slot_id;
} fault_handler_req_query_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t fault_count;     /* total faults since last CLEAR */
    uint32_t restart_count;   /* number of restarts performed */
    uint32_t last_fault_kind; /* FAULT_KIND_* of most recent fault */
    uint64_t last_fault_addr;
    uint64_t last_fault_ip;
    uint64_t last_fault_ns;   /* timestamp of most recent fault */
} fault_handler_reply_query_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* FAULT_HANDLER_OP_HISTORY */
    uint32_t slot_id;
    uint32_t shmem_offset;    /* byte offset for history output */
    uint32_t max_entries;     /* max fault_handler_entry_t to write */
} fault_handler_req_history_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t entry_count;
} fault_handler_reply_history_t;

typedef struct __attribute__((packed)) {
    uint64_t timestamp_ns;
    uint32_t fault_kind;
    uint32_t action;
    uint64_t fault_addr;
    uint64_t fault_ip;
} fault_handler_entry_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* FAULT_HANDLER_OP_POLICY_SET */
    uint32_t slot_id;
    uint32_t max_restarts;    /* max restarts before escalation */
    uint32_t escalate_after;  /* total faults (across restarts) before escalation */
    uint32_t policy_flags;    /* FAULT_POLICY_* bitmask */
    uint32_t restart_delay_ms;
} fault_handler_req_policy_set_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
} fault_handler_reply_policy_set_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* FAULT_HANDLER_OP_CLEAR */
    uint32_t slot_id;
} fault_handler_req_clear_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
} fault_handler_reply_clear_t;

/* ── Error codes ── */
typedef enum {
    FAULT_HANDLER_OK          = 0,
    FAULT_HANDLER_ERR_BAD_SLOT = 1, /* slot_id not tracked */
    FAULT_HANDLER_ERR_NO_SHMEM = 2, /* shared memory not mapped for history */
    FAULT_HANDLER_ERR_NO_CAP  = 3,  /* caller lacks privilege */
} fault_handler_error_t;

/* ── Invariants ──
 * - NOTIFY is delivered by seL4 on PD fault; the handler must reply to unblock the kernel.
 * - Restart delay is enforced by scheduling the restart via the timer service.
 * - After max_restarts, the slot is killed and ESCALATED action is reported.
 * - COREDUMP writes a snapshot of the faulting PD state to AgentFS.
 * - Policy defaults apply if POLICY_SET has not been called for a slot.
 */
