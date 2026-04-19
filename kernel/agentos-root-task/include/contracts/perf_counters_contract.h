#pragma once
/* PERF_COUNTERS contract — version 1
 * PD: perf_counters | Source: src/perf_counters.c | Channel: CH_CONTROLLER_PERF_COUNTERS=42 (from controller)
 */
#include <stdint.h>
#include <stdbool.h>

#define PERF_COUNTERS_CONTRACT_VERSION 1

/* ── Channel IDs (controller perspective) ── */
#define CH_PERF_COUNTERS           42   /* controller -> perf_counters; cross-ref: channels_generated.h */

/* ── Opcodes ── */
#define PERF_COUNTERS_OP_STATUS    0xB800u  /* query aggregate counter summary */
#define PERF_COUNTERS_OP_RESET     0xB801u  /* reset counters for one or all slots */
#define PERF_COUNTERS_OP_DUMP      0xB802u  /* dump per-slot counters to shared memory */
#define PERF_COUNTERS_OP_CONFIGURE 0xB803u  /* select hardware performance events */
#define PERF_COUNTERS_OP_SAMPLE    0xB804u  /* trigger an immediate sample from all slots */

/* ── Hardware event selector ── */
#define PERF_EVENT_CYCLES          0x0001u  /* CPU cycles */
#define PERF_EVENT_INSTRUCTIONS    0x0002u  /* retired instructions */
#define PERF_EVENT_CACHE_MISS      0x0004u  /* L1/L2/LLC cache misses */
#define PERF_EVENT_BRANCH_MISS     0x0008u  /* branch mispredictions */
#define PERF_EVENT_IPC_CALLS       0x0010u  /* seL4 IPC call count */
#define PERF_EVENT_IPC_FAULTS      0x0020u  /* seL4 IPC fault count */
#define PERF_EVENT_MEM_STALLS      0x0040u  /* memory stall cycles */

/* ── Request / Reply structs ── */

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* PERF_COUNTERS_OP_STATUS */
} perf_counters_req_status_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t tracked_slots;   /* number of slots being sampled */
    uint32_t event_mask;      /* currently active PERF_EVENT_* mask */
    uint64_t total_cycles;    /* aggregate cycles across all slots */
    uint64_t total_instructions;
    uint64_t sample_count;    /* total samples collected since boot */
} perf_counters_reply_status_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* PERF_COUNTERS_OP_RESET */
    uint32_t slot_id;         /* 0xFFFFFFFF = reset all slots */
} perf_counters_req_reset_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
} perf_counters_reply_reset_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* PERF_COUNTERS_OP_DUMP */
    uint32_t shmem_offset;    /* byte offset in shared region for output */
    uint32_t max_entries;     /* max perf_counters_slot_entry_t to write */
} perf_counters_req_dump_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t entry_count;
} perf_counters_reply_dump_t;

/* Per-slot performance counter entry written to shmem */
typedef struct __attribute__((packed)) {
    uint32_t slot_id;
    uint64_t agent_id_hi;
    uint64_t agent_id_lo;
    uint64_t cycles;
    uint64_t instructions;
    uint64_t cache_misses;
    uint64_t branch_misses;
    uint64_t ipc_calls;
    uint64_t ipc_faults;
    uint64_t mem_stalls;
    uint64_t sample_count;    /* samples taken for this slot */
} perf_counters_slot_entry_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* PERF_COUNTERS_OP_CONFIGURE */
    uint32_t event_mask;      /* PERF_EVENT_* events to enable */
    uint32_t sample_period_ticks; /* ticks between automatic samples */
} perf_counters_req_configure_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t effective_mask;  /* events actually enabled (hardware may limit) */
} perf_counters_reply_configure_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* PERF_COUNTERS_OP_SAMPLE */
} perf_counters_req_sample_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t slots_sampled;
} perf_counters_reply_sample_t;

/* ── Error codes ── */
typedef enum {
    PERF_COUNTERS_OK          = 0,
    PERF_COUNTERS_ERR_BAD_SLOT = 1, /* slot_id not tracked */
    PERF_COUNTERS_ERR_NO_SHMEM = 2, /* shared memory not mapped */
    PERF_COUNTERS_ERR_HW_LIMIT = 3, /* hardware supports fewer events than requested */
    PERF_COUNTERS_ERR_BAD_CFG  = 4, /* invalid period or zero event_mask */
} perf_counters_error_t;

/* ── Invariants ──
 * - perf_counters reads ARM PMU registers on each sample; no kernel modifications needed.
 * - CONFIGURE selects events globally; per-slot override is not supported.
 * - effective_mask may be a subset of event_mask on hardware with limited PMU slots.
 * - DUMP output is perf_counters_slot_entry_t[] packed into the shared region.
 * - sample_period_ticks=0 disables automatic sampling; SAMPLE must be called explicitly.
 */
