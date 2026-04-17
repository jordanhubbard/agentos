/*
 * PerfCounters IPC Contract
 *
 * The PerfCounters PD collects hardware performance counter samples
 * (cycles, cache misses, branch mispredictions) per PD.
 *
 * Channel: (controller → perf_counters; assigned in agentos.system)
 * Opcodes: MSG_PERF_STATUS, MSG_PERF_RESET, MSG_PERF_DUMP
 *
 * Invariants:
 *   - STATUS returns current counter snapshot; non-destructive.
 *   - RESET clears all counters for the specified PD to zero.
 *   - DUMP copies counter records to shared memory for external analysis.
 *   - Counters wrap at UINT64_MAX without error; callers must handle wrap.
 *   - PD id 0xFFFFFFFF means "all PDs" for RESET and STATUS.
 */

#pragma once
#include "../agentos.h"

/* ─── Request structs ────────────────────────────────────────────────────── */

struct perf_req_status {
    uint32_t pd_id;             /* TRACE_PD_* or 0xFFFFFFFF for all */
};

struct perf_req_reset {
    uint32_t pd_id;
};

struct perf_req_dump {
    uint32_t pd_id;
    uint32_t max_records;
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct perf_reply_status {
    uint32_t ok;
    uint64_t cycles;
    uint64_t cache_misses;
    uint64_t branch_mispredicts;
    uint64_t instructions_retired;
    uint32_t pd_id;
};

struct perf_reply_reset {
    uint32_t ok;
};

struct perf_reply_dump {
    uint32_t ok;
    uint32_t count;             /* records written to shmem */
};

/* ─── Shmem layout: perf sample record ──────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t pd_id;
    uint32_t _pad;
    uint64_t timestamp_ns;
    uint64_t cycles;
    uint64_t cache_misses;
    uint64_t branch_mispredicts;
    uint64_t instructions_retired;
} perf_sample_t;

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum perf_counters_error {
    PERF_OK                 = 0,
    PERF_ERR_BAD_PD         = 1,
    PERF_ERR_NOT_SUPPORTED  = 2,  /* hardware counters unavailable on this board */
};
