/*
 * MemProfiler IPC Contract
 *
 * The MemProfiler PD tracks heap allocation patterns per swap slot and
 * sends leak alerts to the Watchdog when anomalies are detected.
 *
 * Channel: (controller → mem_profiler; varies per system description)
 * Opcodes: MSG_MEM_LEAK_ALERT, MSG_MEMPROF_STATUS, MSG_MEMPROF_DUMP
 *
 * Invariants:
 *   - MSG_MEM_LEAK_ALERT is sent by mem_profiler to the Watchdog (not a reply).
 *   - MSG_MEMPROF_STATUS and MSG_MEMPROF_DUMP are PPC calls from the controller.
 *   - DUMP copies allocation records to shared memory; STATUS is always safe.
 *   - Slot 0 is the controller itself; slots 1..N correspond to workers/agents.
 */

#pragma once
#include "../agentos.h"

/* ─── Request structs ────────────────────────────────────────────────────── */

struct memprof_req_status {
    uint32_t slot;              /* swap slot or worker slot to query */
};

struct memprof_req_dump {
    uint32_t slot;
    uint32_t max_records;       /* max allocation records to return */
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct memprof_reply_status {
    uint32_t ok;
    uint32_t heap_kb;           /* current heap usage in KB */
    uint32_t peak_kb;           /* peak heap usage since boot */
    uint32_t alloc_count;       /* total allocations since last reset */
    uint32_t flags;             /* MEMPROF_FLAG_* */
};

#define MEMPROF_FLAG_LEAK_SUSPECTED (1u << 0)
#define MEMPROF_FLAG_FRAGMENTED     (1u << 1)

struct memprof_reply_dump {
    uint32_t ok;
    uint32_t count;             /* records written to shmem */
};

/* ─── Shmem layout: allocation record ───────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint64_t ptr;               /* virtual address of allocation */
    uint32_t size;              /* bytes */
    uint32_t age_ticks;         /* ticks since allocation */
    uint32_t slot;
    uint32_t _pad;
} memprof_alloc_record_t;

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum memprof_error {
    MEMPROF_OK              = 0,
    MEMPROF_ERR_BAD_SLOT    = 1,
    MEMPROF_ERR_NOT_TRACKED = 2,
};
