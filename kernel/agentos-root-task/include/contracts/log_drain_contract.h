#pragma once
/* LOG_DRAIN contract — version 1
 * PD: log_drain | Source: src/log_drain.c | Channel: CH_LOG_DRAIN=55 (aarch64) / 60 (other) (from controller)
 */
#include <stdint.h>
#include <stdbool.h>

#define LOG_DRAIN_CONTRACT_VERSION 1

/* ── Channel IDs (controller perspective) ── */
/* CH_LOG_DRAIN is board-dependent; defined in agentos.h */
#ifdef BOARD_qemu_virt_aarch64
#define CH_LOG_DRAIN               55   /* cross-ref: agentos.h */
#else
#define CH_LOG_DRAIN               60   /* cross-ref: agentos.h */
#endif

/* ── Opcodes ── */
#define LOG_DRAIN_OP_WRITE         0x01u  /* PD -> log_drain: flush ring slot */
#define LOG_DRAIN_OP_STATUS        0x02u  /* query drain statistics */

/* Additional log_drain-specific opcodes */
#define LOG_DRAIN_OP_REGISTER      0x03u  /* register a new PD ring slot */
#define LOG_DRAIN_OP_DEREGISTER    0x04u  /* deregister a PD ring slot */
#define LOG_DRAIN_OP_FLUSH_ALL     0x05u  /* force flush of all registered slots */

/* ── Ring buffer constants (from agentos.h) ── */
#define LOG_DRAIN_RING_MAGIC       0xC0DE4D55u
#define LOG_DRAIN_RING_SIZE        0x1000u      /* 4KB per PD slot */
#define LOG_DRAIN_RING_HDR_SZ      16u
#define LOG_DRAIN_DATA_SIZE        (LOG_DRAIN_RING_SIZE - LOG_DRAIN_RING_HDR_SZ)

/* ── Request / Reply structs ── */

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* LOG_DRAIN_OP_WRITE */
    uint32_t slot;            /* ring slot index (0 = controller, 1..N = worker slots) */
    uint32_t pd_id;           /* PD numeric identifier (TRACE_PD_* constant) */
} log_drain_req_write_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok, else log_drain_error_t */
    uint32_t bytes_drained;   /* bytes consumed from ring in this call */
} log_drain_reply_write_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* LOG_DRAIN_OP_STATUS */
} log_drain_req_status_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t slot_count;      /* number of registered ring slots */
    uint64_t bytes_drained;   /* total bytes drained since boot */
    uint32_t overflow_count;  /* number of ring overflows detected */
} log_drain_reply_status_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* LOG_DRAIN_OP_REGISTER */
    uint32_t pd_id;           /* TRACE_PD_* identifier */
    uint64_t ring_vaddr;      /* virtual address of log_drain_ring_t in shared region */
} log_drain_req_register_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t slot;            /* assigned slot index */
} log_drain_reply_register_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* LOG_DRAIN_OP_DEREGISTER */
    uint32_t slot;
} log_drain_req_deregister_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
} log_drain_reply_deregister_t;

/* ── Ring buffer header layout (matches agentos.h log_drain_ring_t) ── */
typedef struct __attribute__((packed)) {
    uint32_t magic;    /* LOG_DRAIN_RING_MAGIC */
    uint32_t pd_id;
    uint32_t head;     /* write offset (PD writes) */
    uint32_t tail;     /* read offset (log_drain reads) */
    /* data bytes follow: LOG_DRAIN_DATA_SIZE bytes */
} log_drain_ring_hdr_t;

/* ── Error codes ── */
typedef enum {
    LOG_DRAIN_OK              = 0,
    LOG_DRAIN_ERR_BAD_SLOT    = 1,  /* slot index out of range or not registered */
    LOG_DRAIN_ERR_BAD_MAGIC   = 2,  /* ring header magic incorrect */
    LOG_DRAIN_ERR_NO_SHMEM    = 3,  /* shared ring region not mapped */
    LOG_DRAIN_ERR_FULL        = 4,  /* registration table full */
} log_drain_error_t;

/* ── Invariants ──
 * - log_drain_rings shared memory must be mapped before LOG_DRAIN_OP_WRITE.
 * - Each PD slot is exactly LOG_DRAIN_RING_SIZE (4KB) bytes.
 * - Head is written by the PD; tail is advanced by log_drain after reading.
 * - Ring overflow is detected when (head + 1) % DATA_SIZE == tail; bytes are dropped.
 * - LOG_DRAIN_OP_WRITE is called inline via log_drain_write() in agentos.h.
 */
