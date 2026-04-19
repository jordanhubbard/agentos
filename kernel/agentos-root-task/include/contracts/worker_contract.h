#pragma once
/* WORKER contract — version 1
 * PD: worker_N (N=0..7) | Source: src/worker.c | Channel: CH_CONTROLLER_WORKER_N=10+N (from controller)
 */
#include <stdint.h>
#include <stdbool.h>

#define WORKER_CONTRACT_VERSION 1

/* ── Channel IDs (controller perspective) ── */
/* Worker channels: 10+N for worker N (0-7); cross-ref: agentos.h WORKER_POOL_BASE_CH=20 */
#define CH_WORKER_0            10   /* controller -> worker_0 */
#define CH_WORKER_1            11
#define CH_WORKER_2            12
#define CH_WORKER_3            13
#define CH_WORKER_4            14
#define CH_WORKER_5            15
#define CH_WORKER_6            16
#define CH_WORKER_7            17
#define WORKER_COUNT            8

/* ── Opcodes ── */
#define WORKER_OP_RETRIEVE          0x0701u  /* worker -> controller: request AgentFS object */
#define WORKER_OP_RETRIEVE_REPLY    0x0702u  /* controller -> worker: AgentFS data response */
#define WORKER_OP_DEMO_TASK_RETRIEVE 0x0710u /* task type: retrieve object from AgentFS */

/* New opcodes for direct controller->worker dispatch */
#define WORKER_OP_PING              0xW001u  /* controller -> worker: liveness check */
#define WORKER_OP_RUN               0xW002u  /* controller -> worker: run queued WASM task */
#define WORKER_OP_ABORT             0xW003u  /* controller -> worker: abort current task */
#define WORKER_OP_STATUS            0xW004u  /* controller -> worker: query worker state */

/* Correct hex values for new opcodes (W not valid hex — defined distinctly) */
#undef WORKER_OP_PING
#undef WORKER_OP_RUN
#undef WORKER_OP_ABORT
#undef WORKER_OP_STATUS
#define WORKER_OP_PING              0xC001u
#define WORKER_OP_RUN               0xC002u
#define WORKER_OP_ABORT             0xC003u
#define WORKER_OP_STATUS            0xC004u

/* ── Worker states ── */
#define WORKER_STATE_IDLE           0u
#define WORKER_STATE_RUNNING        1u
#define WORKER_STATE_BLOCKED        2u  /* waiting on IPC reply */
#define WORKER_STATE_ABORTING       3u

/* ── Request / Reply structs ── */

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* WORKER_OP_RETRIEVE */
    uint64_t hash_lo;         /* AgentFS object hash lower 64 bits */
    uint64_t hash_hi;         /* AgentFS object hash upper 64 bits */
    uint32_t shmem_offset;    /* offset in shared region for returned data */
} worker_req_retrieve_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok, else worker_error_t */
    uint32_t bytes_returned;  /* bytes written into shmem */
} worker_reply_retrieve_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* WORKER_OP_RUN */
    uint64_t wasm_hash_lo;    /* module to execute */
    uint64_t wasm_hash_hi;
    uint32_t task_id;         /* caller-assigned task identifier */
    uint32_t cap_mask;        /* AGENTOS_CAP_* for this execution */
} worker_req_run_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = accepted */
    uint32_t task_id;         /* echoed */
} worker_reply_run_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* WORKER_OP_ABORT */
    uint32_t task_id;
} worker_req_abort_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok; WORKER_ERR_NO_TASK if task_id not current */
} worker_reply_abort_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* WORKER_OP_STATUS */
} worker_req_status_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t state;           /* WORKER_STATE_* */
    uint32_t current_task_id; /* 0 if idle */
    uint32_t tasks_completed; /* lifetime counter */
    uint64_t cpu_ticks;       /* CPU ticks consumed since last reset */
} worker_reply_status_t;

/* ── Error codes ── */
typedef enum {
    WORKER_OK                 = 0,
    WORKER_ERR_BUSY           = 1,  /* worker already running a task */
    WORKER_ERR_NO_TASK        = 2,  /* abort/status for unknown task_id */
    WORKER_ERR_BAD_HASH       = 3,  /* WASM hash not in AgentFS */
    WORKER_ERR_LOAD_FAIL      = 4,  /* WASM load or instantiation failed */
    WORKER_ERR_EXEC_FAULT     = 5,  /* WASM trap / unhandled fault during execution */
} worker_error_t;

/* ── Invariants ──
 * - Each worker PD runs exactly one task at a time (single-threaded WASM host).
 * - WORKER_OP_RUN is rejected with WORKER_ERR_BUSY if worker is not IDLE.
 * - WORKER_OP_RETRIEVE is initiated by the worker toward the controller on CH_WORKER_N.
 * - CPU quota for each worker is enforced by quota_pd; excess triggers MSG_QUOTA_REVOKE.
 */
