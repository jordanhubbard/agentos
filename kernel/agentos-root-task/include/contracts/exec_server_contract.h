/*
 * ExecServer IPC Contract
 *
 * The ExecServer PD manages the lifecycle of executable images within
 * agentOS.  It accepts authenticated launch requests, stages the binary
 * from AgentFS into exec_shmem, and tracks execution state.  In the
 * current revision, actual execution is stubbed; the server tracks slots
 * and reports state transitions.
 *
 * Channel: CH_EXEC_SERVER = 28 (from controller perspective)
 * Opcodes: OP_EXEC_LAUNCH, OP_EXEC_STATUS, OP_EXEC_WAIT, OP_EXEC_KILL
 *
 * Shmem: exec_shmem (4 KB)
 *   Offset 0: NUL-terminated path string for OP_EXEC_LAUNCH
 *
 * Invariants:
 *   - The caller must place the executable path in exec_shmem before
 *     calling OP_EXEC_LAUNCH.
 *   - auth_token must be a valid token issued by AuthServer.
 *   - exec_id is opaque; the ExecServer owns the namespace.
 *   - OP_EXEC_WAIT blocks (seL4 reply withheld) until the exec_id exits.
 *   - OP_EXEC_KILL sends SIGKILL; reply is immediate regardless of state.
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs ────────────────────────────────────────────────────────── */
#define CH_EXEC_SERVER   28      /* controller → exec_server */

/* ─── Opcodes ─────────────────────────────────────────────────────────────── */
#define OP_EXEC_LAUNCH   0x01
#define OP_EXEC_STATUS   0x02
#define OP_EXEC_WAIT     0x03
#define OP_EXEC_KILL     0x04

/* ─── Execution states ───────────────────────────────────────────────────── */
#define EXEC_FREE        0       /* slot unused */
#define EXEC_LOADING     1       /* ELF being staged */
#define EXEC_RUNNING     2       /* process live */
#define EXEC_EXITED      3       /* process has exited cleanly */
#define EXEC_ERROR       4       /* load or runtime error */

/* ─── Request structs ────────────────────────────────────────────────────── */

struct exec_server_req_launch {
    uint32_t op;                 /* OP_EXEC_LAUNCH */
    uint32_t auth_token;         /* valid AuthServer token */
    uint32_t cap_mask;           /* AGENTOS_CAP_* to grant the new process */
    /* NUL-terminated path string in exec_shmem offset 0 */
};

struct exec_server_req_status {
    uint32_t op;                 /* OP_EXEC_STATUS */
    uint32_t exec_id;            /* exec_id from a prior LAUNCH reply */
};

struct exec_server_req_wait {
    uint32_t op;                 /* OP_EXEC_WAIT */
    uint32_t exec_id;
};

struct exec_server_req_kill {
    uint32_t op;                 /* OP_EXEC_KILL */
    uint32_t exec_id;
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct exec_server_reply_launch {
    uint32_t ok;                 /* EXEC_OK or error code */
    uint32_t exec_id;            /* opaque slot identifier; 0 on error */
};

struct exec_server_reply_status {
    uint32_t ok;                 /* EXEC_OK or error code */
    uint32_t state;              /* EXEC_FREE / EXEC_LOADING / ... */
    uint32_t pid;                /* kernel-assigned pid; 0 if not running */
};

struct exec_server_reply_wait {
    uint32_t ok;                 /* EXEC_OK or error code */
    uint32_t pid;                /* pid of the exited process */
};

struct exec_server_reply_kill {
    uint32_t ok;                 /* EXEC_OK or error code */
};

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum exec_server_error {
    EXEC_OK              = 0,
    EXEC_ERR_INVAL       = 1,    /* bad argument */
    EXEC_ERR_NO_SLOTS    = 2,    /* all exec slots occupied */
    EXEC_ERR_VFS         = 3,    /* AgentFS lookup or read failed */
    EXEC_ERR_NOT_FOUND   = 4,    /* exec_id does not exist */
};
