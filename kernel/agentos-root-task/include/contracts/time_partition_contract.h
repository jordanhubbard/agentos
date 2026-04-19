#pragma once
/* TIME_PARTITION contract — version 1
 * PD: time_partition | Source: src/time_partition.c | Channel: CH_CONTROLLER_TIME_PARTITION=41 (from controller)
 */
#include <stdint.h>
#include <stdbool.h>

#define TIME_PARTITION_CONTRACT_VERSION 1

/* ── Channel IDs (controller perspective) ── */
#define CH_TIME_PARTITION          41   /* controller -> time_partition; cross-ref: channels_generated.h */

/* ── Opcodes ── */
#define TIME_PARTITION_OP_ALLOC    0xD100u  /* allocate a time partition for a slot */
#define TIME_PARTITION_OP_FREE     0xD101u  /* release a time partition */
#define TIME_PARTITION_OP_STATUS   0xD102u  /* query partition table and utilization */
#define TIME_PARTITION_OP_SET      0xD103u  /* update budget/period for existing partition */
#define TIME_PARTITION_OP_SAMPLE   0xD104u  /* trigger immediate CPU utilization sample */

/* ── Partition scheduling modes ── */
#define TPART_MODE_SPORADIC        0u  /* seL4 MCS sporadic server (default) */
#define TPART_MODE_PERIODIC        1u  /* strict periodic execution */
#define TPART_MODE_BEST_EFFORT     2u  /* no hard reservation; shares slack */

/* ── Request / Reply structs ── */

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* TIME_PARTITION_OP_ALLOC */
    uint32_t slot_id;         /* worker/swap slot to partition */
    uint64_t budget_ns;       /* CPU budget per period in nanoseconds */
    uint64_t period_ns;       /* scheduling period in nanoseconds */
    uint32_t mode;            /* TPART_MODE_* */
    uint32_t priority;        /* seL4 MCS scheduling context priority */
} time_partition_req_alloc_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok, else time_partition_error_t */
    uint32_t partition_id;    /* opaque handle for subsequent operations */
} time_partition_reply_alloc_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* TIME_PARTITION_OP_FREE */
    uint32_t partition_id;
} time_partition_req_free_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
} time_partition_reply_free_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* TIME_PARTITION_OP_STATUS */
} time_partition_req_status_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t partition_count; /* total allocated partitions */
    uint32_t free_contexts;   /* seL4 MCS scheduling context objects available */
    uint64_t total_budget_ns; /* sum of all partition budgets */
    uint64_t cpu_capacity_ns; /* total CPU capacity per second */
} time_partition_reply_status_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* TIME_PARTITION_OP_SET */
    uint32_t partition_id;
    uint64_t new_budget_ns;
    uint64_t new_period_ns;
} time_partition_req_set_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
} time_partition_reply_set_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* TIME_PARTITION_OP_SAMPLE */
    uint32_t partition_id;    /* 0xFFFFFFFF = sample all partitions */
    uint32_t shmem_offset;    /* byte offset for output */
    uint32_t max_entries;
} time_partition_req_sample_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t entry_count;
} time_partition_reply_sample_t;

/* Per-partition sample entry written to shmem */
typedef struct __attribute__((packed)) {
    uint32_t partition_id;
    uint32_t slot_id;
    uint64_t budget_ns;
    uint64_t period_ns;
    uint64_t consumed_ns;     /* budget consumed in last period */
    uint32_t overruns;        /* number of period overruns */
    uint32_t mode;            /* TPART_MODE_* */
} time_partition_sample_entry_t;

/* ── Error codes ── */
typedef enum {
    TIME_PARTITION_OK         = 0,
    TIME_PARTITION_ERR_FULL   = 1,  /* no seL4 MCS scheduling context objects available */
    TIME_PARTITION_ERR_BAD_ID = 2,  /* partition_id not valid */
    TIME_PARTITION_ERR_OVERCOMMIT = 3, /* total budget_ns > cpu_capacity_ns */
    TIME_PARTITION_ERR_BAD_PERIOD = 4, /* period_ns too small or zero */
    TIME_PARTITION_ERR_NO_SHMEM = 5,   /* sample output region not mapped */
} time_partition_error_t;

/* ── Invariants ──
 * - budget_ns must be less than period_ns.
 * - Sum of all partition budgets must not exceed the system's CPU capacity.
 * - TPART_MODE_SPORADIC allows unused budget to be reclaimed by lower-priority PDs.
 * - TPART_MODE_PERIODIC strictly enforces budget; excess CPU time is forfeited.
 * - SET does not take effect until the start of the next period.
 * - FREE reclaims the seL4 MCS scheduling context; the slot reverts to best-effort.
 */
