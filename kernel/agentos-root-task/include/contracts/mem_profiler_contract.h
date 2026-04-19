#pragma once
/* MEM_PROFILER contract — version 1
 * PD: mem_profiler | Source: src/mem_profiler.c | Channel: CH_CONTROLLER_MEM_PROFILER=50 (from controller)
 */
#include <stdint.h>
#include <stdbool.h>

#define MEM_PROFILER_CONTRACT_VERSION 1

/* ── Channel IDs (controller perspective) ── */
#define CH_MEM_PROFILER            50   /* controller -> mem_profiler; cross-ref: channels_generated.h */

/* ── Opcodes ── */
#define MEM_PROFILER_OP_LEAK_ALERT  0x0C01u  /* mem_profiler -> watchdog: slot leak detected */
#define MEM_PROFILER_OP_STATUS      0xC000u  /* query profiler state and per-slot summary */
#define MEM_PROFILER_OP_DUMP        0xC001u  /* dump per-slot memory allocation detail */
#define MEM_PROFILER_OP_RESET       0xC002u  /* reset profiler counters for a slot */
#define MEM_PROFILER_OP_CONFIGURE   0xC003u  /* set leak detection threshold */

/* ── Leak alert severity ── */
#define MEM_PROFILER_ALERT_WARN     0u  /* usage growing but below hard limit */
#define MEM_PROFILER_ALERT_CRITICAL 1u  /* hard limit exceeded; revocation pending */

/* ── Request / Reply structs ── */

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* MEM_PROFILER_OP_STATUS */
} mem_profiler_req_status_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t tracked_slots;   /* number of worker slots being monitored */
    uint64_t total_bytes;     /* total bytes allocated across all tracked slots */
    uint32_t leak_alerts;     /* number of leak alerts raised since boot */
    uint32_t hard_limit_hits; /* slots that exceeded their hard memory limit */
} mem_profiler_reply_status_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* MEM_PROFILER_OP_DUMP */
    uint32_t shmem_offset;    /* byte offset in shared region for output */
    uint32_t max_entries;     /* max mem_profiler_slot_entry_t to write */
} mem_profiler_req_dump_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
    uint32_t entry_count;
} mem_profiler_reply_dump_t;

/* Per-slot memory profile entry written to shmem */
typedef struct __attribute__((packed)) {
    uint32_t slot_id;
    uint64_t agent_id_hi;
    uint64_t agent_id_lo;
    uint64_t bytes_allocated; /* current total allocated bytes */
    uint64_t bytes_peak;      /* peak allocated bytes in slot lifetime */
    uint32_t alloc_count;     /* number of live allocations */
    uint32_t free_count;      /* number of deallocations since reset */
    uint32_t leak_score;      /* heuristic leak score (0 = clean) */
    uint32_t flags;           /* MEM_PROFILER_FLAG_* */
} mem_profiler_slot_entry_t;

#define MEM_PROFILER_FLAG_LEAKING   (1u << 0)  /* slot flagged as leaking */
#define MEM_PROFILER_FLAG_AT_LIMIT  (1u << 1)  /* slot at or over hard limit */

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* MEM_PROFILER_OP_RESET */
    uint32_t slot_id;         /* 0xFFFFFFFF = reset all slots */
} mem_profiler_req_reset_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
} mem_profiler_reply_reset_t;

typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* MEM_PROFILER_OP_CONFIGURE */
    uint32_t slot_id;         /* target slot (0xFFFFFFFF = set global default) */
    uint32_t hard_limit_kb;   /* hard limit in KB (0 = no limit) */
    uint32_t leak_threshold_kb; /* bytes/period growth to flag as leak */
    uint32_t alert_period_ticks;
} mem_profiler_req_configure_t;

typedef struct __attribute__((packed)) {
    uint32_t status;          /* 0 = ok */
} mem_profiler_reply_configure_t;

/* Notification sent by mem_profiler to watchdog on leak detection */
typedef struct __attribute__((packed)) {
    uint32_t opcode;          /* MEM_PROFILER_OP_LEAK_ALERT */
    uint32_t slot_id;
    uint32_t severity;        /* MEM_PROFILER_ALERT_* */
    uint64_t bytes_allocated;
    uint32_t leak_score;
} mem_profiler_notify_leak_t;

/* ── Error codes ── */
typedef enum {
    MEM_PROFILER_OK           = 0,
    MEM_PROFILER_ERR_BAD_SLOT = 1,  /* slot_id not registered or out of range */
    MEM_PROFILER_ERR_NO_SHMEM = 2,  /* shared memory not mapped for dump */
    MEM_PROFILER_ERR_BAD_CFG  = 3,  /* invalid threshold or limit value */
} mem_profiler_error_t;

/* ── Invariants ──
 * - mem_profiler passively monitors all worker slots via shared memory regions.
 * - LEAK_ALERT is sent to the watchdog PD, not the controller, to avoid deadlock.
 * - DUMP output is mem_profiler_slot_entry_t[] in the shared region.
 * - hard_limit_kb=0 means the slot's quota (set by quota_pd) is the effective limit.
 * - RESET clears allocation counts but does not unmap or reclaim memory.
 */
