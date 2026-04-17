/*
 * DebugBridge IPC Contract
 *
 * The DebugBridge PD provides a seL4-aware debug interface for inspecting
 * and controlling running Protection Domains.  Only available in debug builds.
 *
 * Channel: CH_DEBUG_BRIDGE (see agentos.h)
 * Opcodes: MSG_DBG_* (see agentos.h)
 *
 * Invariants:
 *   - ATTACH returns a session_id; all subsequent operations require it.
 *   - Only one session per target PD may be active at a time.
 *   - BREAKPOINT sets a hardware breakpoint via seL4 debug capabilities.
 *   - STEP single-steps the target; the reply returns the new PC.
 *   - READ_MEM reads from the target's address space via seL4 cap inspection.
 *   - DETACH releases the session; the target PD resumes normal scheduling.
 *   - DebugBridge is excluded from production builds (CONFIG_DEBUG_BRIDGE=0).
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs ────────────────────────────────────────────────────────── */
#define DEBUG_BRIDGE_CH_CONTROLLER  CH_DEBUG_BRIDGE

/* ─── Request structs ────────────────────────────────────────────────────── */

struct dbg_req_attach {
    uint32_t target_pd;         /* seL4 PD badge / slot ID of target */
};

struct dbg_req_detach {
    uint32_t session_id;
};

struct dbg_req_breakpoint {
    uint32_t session_id;
    uint32_t bp_type;           /* DBG_BP_* */
    uint64_t addr;              /* virtual address in target PD */
};

#define DBG_BP_EXECUTE  0  /* break on instruction fetch */
#define DBG_BP_READ     1  /* break on data read */
#define DBG_BP_WRITE    2  /* break on data write */
#define DBG_BP_ACCESS   3  /* break on read or write */

struct dbg_req_step {
    uint32_t session_id;
    uint32_t steps;             /* number of instructions to step (1 = single) */
};

struct dbg_req_read_mem {
    uint32_t session_id;
    uint64_t addr;              /* virtual address in target PD */
    uint32_t len;               /* bytes to read (max: debug_shmem size) */
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct dbg_reply_attach {
    uint32_t ok;
    uint32_t session_id;
};

struct dbg_reply_detach {
    uint32_t ok;
};

struct dbg_reply_breakpoint {
    uint32_t ok;
    uint32_t bp_id;             /* hardware breakpoint index */
};

struct dbg_reply_step {
    uint32_t ok;
    uint64_t pc;                /* instruction pointer after step */
};

struct dbg_reply_read_mem {
    uint32_t ok;
    uint32_t actual;            /* bytes read (placed in debug_shmem) */
};

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum debug_bridge_error {
    DBG_OK                  = 0,
    DBG_ERR_NOT_FOUND       = 1,  /* target PD does not exist */
    DBG_ERR_ALREADY_ATTACHED = 2,
    DBG_ERR_BAD_SESSION     = 3,
    DBG_ERR_NO_CAP          = 4,  /* no seL4 debug cap for target */
    DBG_ERR_BAD_ADDR        = 5,  /* address not mapped in target */
    DBG_ERR_NO_HW_BP        = 6,  /* hardware breakpoint registers exhausted */
};
