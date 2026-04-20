/*
 * ProcServer IPC Contract
 *
 * The ProcServer PD is the process table for agentOS.  It tracks all
 * user-mode processes, their parent relationships, capability masks, and
 * lifecycle state.  SpawnServer and ExecServer delegate process record
 * management here; all other PDs query ProcServer for pid-to-state lookups.
 *
 * Channel: CH_PROC_SERVER = 27 (from controller perspective)
 * Opcodes: OP_PROC_SPAWN, OP_PROC_EXIT, OP_PROC_WAIT, OP_PROC_STATUS,
 *          OP_PROC_LIST, OP_PROC_KILL, OP_PROC_SETCAP
 *
 * Shmem: proc_shmem (8 KB)
 *   Used by OP_PROC_LIST to write an array of proc_entry_t records.
 *
 * Invariants:
 *   - OP_PROC_SPAWN creates a process record; actual execution is handled
 *     by ExecServer or SpawnServer.  ProcServer owns the pid namespace.
 *   - OP_PROC_WAIT blocks (reply deferred) until the target pid exits.
 *     Once the pid transitions to PROC_STATE_ZOMBIE the reply is sent.
 *   - OP_PROC_EXIT sets state to PROC_STATE_ZOMBIE and unblocks any waiter.
 *   - OP_PROC_KILL sends a signal; the target process handles termination.
 *   - OP_PROC_SETCAP requires the caller to hold AGENTOS_CAP_ADMIN.
 *   - OP_PROC_LIST writes up to proc_shmem capacity; count is always exact.
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs ────────────────────────────────────────────────────────── */
#define CH_PROC_SERVER   27      /* controller → proc_server */

/* ─── Opcodes ─────────────────────────────────────────────────────────────── */
#define OP_PROC_SPAWN    0x01
#define OP_PROC_EXIT     0x02
#define OP_PROC_WAIT     0x03
#define OP_PROC_STATUS   0x04
#define OP_PROC_LIST     0x05
#define OP_PROC_KILL     0x06
#define OP_PROC_SETCAP   0x07

/* ─── Process states ─────────────────────────────────────────────────────── */
#define PROC_STATE_FREE     0    /* slot unused */
#define PROC_STATE_RUNNING  1    /* process is live */
#define PROC_STATE_ZOMBIE   2    /* exited; waiting for wait() collection */
#define PROC_STATE_STOPPED  3    /* suspended (SIGSTOP or equivalent) */

/* ─── Request structs ────────────────────────────────────────────────────── */

struct proc_server_req_spawn {
    uint32_t op;                 /* OP_PROC_SPAWN */
    uint32_t parent_pid;         /* 0 = root (init) is parent */
    uint32_t auth_token;         /* valid AuthServer token for the spawner */
    uint32_t cap_mask;           /* AGENTOS_CAP_* bitmask to grant child */
};

struct proc_server_req_exit {
    uint32_t op;                 /* OP_PROC_EXIT */
    uint32_t pid;
    uint32_t exit_code;
};

struct proc_server_req_wait {
    uint32_t op;                 /* OP_PROC_WAIT */
    uint32_t pid;                /* pid to wait on */
};

struct proc_server_req_status {
    uint32_t op;                 /* OP_PROC_STATUS */
    uint32_t pid;
};

struct proc_server_req_list {
    uint32_t op;                 /* OP_PROC_LIST */
};

struct proc_server_req_kill {
    uint32_t op;                 /* OP_PROC_KILL */
    uint32_t pid;
    uint32_t signal;             /* POSIX signal number */
};

struct proc_server_req_setcap {
    uint32_t op;                 /* OP_PROC_SETCAP */
    uint32_t pid;
    uint32_t cap_mask;           /* new AGENTOS_CAP_* bitmask */
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct proc_server_reply_spawn {
    uint32_t ok;                 /* PROC_OK or error code */
    uint32_t pid;                /* assigned pid; 0 on error */
};

struct proc_server_reply_exit {
    uint32_t ok;                 /* PROC_OK or error code */
};

struct proc_server_reply_wait {
    uint32_t ok;                 /* PROC_OK or error code */
    uint32_t exit_code;          /* exit code from OP_PROC_EXIT */
    uint32_t state;              /* final PROC_STATE_* */
};

struct proc_server_reply_status {
    uint32_t ok;                 /* PROC_OK or error code */
    uint32_t state;              /* current PROC_STATE_* */
    uint32_t cap_mask;           /* AGENTOS_CAP_* bitmask */
};

struct proc_server_reply_list {
    uint32_t ok;                 /* PROC_OK or error code */
    uint32_t count;              /* proc_entry_t records in proc_shmem */
};

struct proc_server_reply_kill {
    uint32_t ok;                 /* PROC_OK or error code */
};

struct proc_server_reply_setcap {
    uint32_t ok;                 /* PROC_OK or error code */
};

/* ─── Shmem layout: process list entry ──────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t pid;
    uint32_t parent_pid;
    uint32_t state;              /* PROC_STATE_* */
    uint32_t cap_mask;           /* AGENTOS_CAP_* */
    uint32_t exit_code;          /* valid when state == PROC_STATE_ZOMBIE */
} proc_entry_t;

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum proc_server_error {
    PROC_OK              = 0,
    PROC_ERR_INVAL       = 1,    /* bad argument */
    PROC_ERR_NOT_FOUND   = 2,    /* pid does not exist */
    PROC_ERR_FULL        = 3,    /* process table full */
    PROC_ERR_PERM        = 4,    /* caller lacks required capability */
};
