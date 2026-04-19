#pragma once
/* GPU_SCHED contract — version 1
 * PD: gpu_sched | Source: src/gpu_sched.c | Channel: (no fixed controller channel — agents call directly)
 */
#include <stdint.h>
#include <stdbool.h>

#define GPU_SCHED_CONTRACT_VERSION 1

/* ── Opcodes ── */
#define GPU_SCHED_OP_SUBMIT         0x0901u  /* submit GPU task: hash_lo, hash_hi, priority, flags */
#define GPU_SCHED_OP_SUBMIT_REPLY   0x0902u  /* reply: ticket_id or error */
#define GPU_SCHED_OP_STATUS         0x0903u  /* query scheduler state */
#define GPU_SCHED_OP_STATUS_REPLY   0x0904u  /* reply: queue_depth, busy_slots, idle_slots */
#define GPU_SCHED_OP_CANCEL         0x0905u  /* cancel pending ticket by ticket_id */
#define GPU_SCHED_OP_CANCEL_REPLY   0x0906u  /* reply: ok or not-found */
#define GPU_SCHED_OP_COMPLETE       0x0910u  /* EventBus event: task completed */
#define GPU_SCHED_OP_FAILED         0x0911u  /* EventBus event: task failed */

/* ── GPU task flags ── */
#define GPU_FLAG_PREEMPTIBLE        (1u << 0)  /* task may be preempted */
#define GPU_FLAG_EXCLUSIVE          (1u << 1)  /* requires dedicated GPU slot */
#define GPU_FLAG_REALTIME           (1u << 2)  /* hard-deadline scheduling */

/* ── Request / Reply structs ── */

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* GPU_SCHED_OP_SUBMIT */
    uint64_t wasm_hash_lo;    /* WASM compute kernel hash */
    uint64_t wasm_hash_hi;
    uint32_t priority;        /* 0 = lowest, 255 = highest */
    uint32_t flags;           /* GPU_FLAG_* bitmask */
    uint32_t timeout_ms;      /* max execution time; 0 = no limit */
} gpu_sched_req_submit_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok, else gpu_sched_error_t */
    uint32_t ticket_id;       /* opaque handle for cancel/status */
} gpu_sched_reply_submit_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* GPU_SCHED_OP_STATUS */
} gpu_sched_req_status_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t queue_depth;     /* tasks waiting in queue */
    uint32_t busy_slots;      /* GPU execution slots currently active */
    uint32_t idle_slots;      /* GPU execution slots available */
    uint64_t tasks_completed; /* lifetime completed count */
    uint64_t tasks_failed;    /* lifetime failed count */
} gpu_sched_reply_status_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* GPU_SCHED_OP_CANCEL */
    uint32_t ticket_id;
} gpu_sched_req_cancel_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok, GPU_SCHED_ERR_NOT_FOUND if ticket unknown */
} gpu_sched_reply_cancel_t;

/* EventBus payload for GPU_SCHED_OP_COMPLETE / GPU_SCHED_OP_FAILED */
typedef struct __attribute__((packed)) {
    uint32_t ticket_id;
    uint32_t result_code;     /* 0 = success, else gpu_sched_error_t */
    uint64_t exec_ns;         /* measured execution time in nanoseconds */
} gpu_sched_event_t;

/* ── Error codes ── */
typedef enum {
    GPU_SCHED_OK              = 0,
    GPU_SCHED_ERR_FULL        = 1,  /* queue is full */
    GPU_SCHED_ERR_NOT_FOUND   = 2,  /* ticket_id unknown */
    GPU_SCHED_ERR_BAD_HASH    = 3,  /* kernel WASM hash not in AgentFS */
    GPU_SCHED_ERR_TIMEOUT     = 4,  /* task exceeded timeout_ms */
    GPU_SCHED_ERR_EXEC_FAULT  = 5,  /* GPU kernel trapped */
    GPU_SCHED_ERR_NO_CAP      = 6,  /* caller lacks AGENTOS_CAP_GPU */
} gpu_sched_error_t;

/* ── Invariants ──
 * - Callers must hold AGENTOS_CAP_GPU to submit tasks.
 * - ticket_id is unique per scheduler instance; never reused within a session.
 * - Completion and failure are published to EventBus (not replied inline).
 * - GPU_FLAG_EXCLUSIVE tasks wait until all slots are idle before running.
 */
