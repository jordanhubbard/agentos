/*
 * exec_server.c — ELF/WASM loader and execution dispatcher for agentOS
 *
 * HURD-equivalent: exec server
 * Priority: 150 (passive)
 *
 * Receives OP_EXEC_LAUNCH requests, reads the named ELF/WASM binary from
 * VFS, validates the header, and dispatches execution to a free app_slot PD.
 * Maintains a table of exec_id → (app_slot_id, pid) mappings.
 *
 * Channel assignments:
 *   id=0: receives PPC from controller (CH_EXEC_SERVER = 28)
 *   id=34..37: bidirectional notify with app_slot_0..3 (CH_APP_SLOT_0 + slot_id)
 *              exec_server → slot: signal ELF load
 *              slot → exec_server: confirm running (arrives as notified(34..37))
 *
 * Shared memory:
 *   exec_shmem (4KB): path for LAUNCH request + exec status output
 *
 * Opcodes (all in agentos.h):
 *   OP_EXEC_LAUNCH  0xE0  path in exec_shmem, MR1=auth_token, MR2=cap_mask → ok+exec_id
 *   OP_EXEC_STATUS  0xE1  MR1=exec_id → ok+state+pid
 *   OP_EXEC_WAIT    0xE2  MR1=exec_id → ok+pid (poll; returns immediately)
 *   OP_EXEC_KILL    0xE3  MR1=exec_id → ok
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "sel4_server.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ── Configuration ─────────────────────────────────────────────────────── */
#define EXEC_MAX_TABLE     8u
#define EXEC_PATH_MAX      64u
#define EXEC_APP_SLOTS     4u     /* must match agentos.system app_slot count */

/* Exec states */
#define EXEC_FREE      0u
#define EXEC_LOADING   1u
#define EXEC_RUNNING   2u
#define EXEC_EXITED    3u
#define EXEC_ERROR     4u

#define EXEC_STATE_FREE     0
#define EXEC_STATE_LOADING  1
#define EXEC_STATE_RUNNING  2
#define EXEC_STATE_DONE     3
#define EXEC_STATE_FAILED   4

/* ELF magic */
#define ELF_MAGIC0  0x7Fu
#define ELF_MAGIC1  'E'
#define ELF_MAGIC2  'L'
#define ELF_MAGIC3  'F'

/* WASM magic */
#define WASM_MAGIC0  0x00u
#define WASM_MAGIC1  'a'
#define WASM_MAGIC2  's'
#define WASM_MAGIC3  'm'

/* ── Shared memory ─────────────────────────────────────────────────────── */
uintptr_t exec_shmem_vaddr;      /* shmem: path string at offset 0 */
#define EXEC_SHMEM_SIZE  0x10000u

/* ── Exec table entry ─────────────────────────────────────────────────── */
typedef struct {
    uint8_t  state;
    uint8_t  app_slot_id;   /* which app_slot PD (0–3) */
    uint8_t  _pad[2];
    uint32_t exec_id;
    uint32_t auth_token;
    uint32_t cap_mask;
    uint32_t pid;
    char     path[EXEC_PATH_MAX];
} exec_entry_t;

static exec_entry_t exec_table[EXEC_MAX_TABLE];
static uint32_t     next_exec_id = 1;
static uint32_t     next_pid     = 100;  /* PIDs 100+ for exec-launched procs */

/* ── Helpers ──────────────────────────────────────────────────────────── */
static void dbg(const char *s) { sel4_dbg_puts(s); }

static void copy_path(char *dst, const char *src, int max)
{
    int i;
    for (i = 0; i < max - 1 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static exec_entry_t *find_exec(uint32_t exec_id)
{
    for (uint32_t i = 0; i < EXEC_MAX_TABLE; i++) {
        if (exec_table[i].state != EXEC_FREE && exec_table[i].exec_id == exec_id)
            return &exec_table[i];
    }
    return NULL;
}

/* Find task by exec_id (stored as index+1 for non-zero IDs) */
static exec_entry_t *find_task_by_id(uint32_t exec_id)
{
    if (exec_id == 0 || exec_id > (uint32_t)EXEC_MAX_TABLE)
        return NULL;
    exec_entry_t *t = &exec_table[exec_id - 1];
    if (t->state == EXEC_FREE)
        return NULL;
    return t;
}

/*
 * Round-robin app_slot allocator.
 * Scans exec_table to find which app_slot_ids are in active use, then
 * picks the lowest slot_id (0–3) that is not currently LOADING/RUNNING.
 */
static int find_free_app_slot(void)
{
    bool in_use[4] = { false, false, false, false };
    for (uint32_t i = 0; i < EXEC_MAX_TABLE; i++) {
        if (exec_table[i].state == EXEC_STATE_LOADING ||
            exec_table[i].state == EXEC_STATE_RUNNING) {
            uint8_t sid = exec_table[i].app_slot_id;
            if (sid < 4)
                in_use[sid] = true;
        }
    }
    for (int s = 0; s < 4; s++) {
        if (!in_use[s])
            return s;
    }
    return -1;  /* all slots busy */
}

static uint8_t peek_binary_type(void)
{
    /* Probe exec_shmem at offset 256 (after path string) for binary header.
     * For Phase 1, the binary isn't actually loaded; we infer from path
     * extension. */
    const char *path = (const char *)exec_shmem_vaddr;
    uint32_t len = 0;
    while (len < EXEC_PATH_MAX && path[len] != '\0') len++;

    /* Check extension: .wasm → WASM, otherwise → ELF */
    if (len >= 5 && path[len-5] == '.' && path[len-4] == 'w' &&
        path[len-3] == 'a' && path[len-2] == 's' && path[len-1] == 'm')
        return 1; /* WASM */
    return 0;     /* ELF */
}

/* ── IPC dispatch ─────────────────────────────────────────────────────── */
static uint32_t handle_launch(void)
{
    IPC_STUB_LOCALS
    if (!exec_shmem_vaddr) {
        dbg("[exec_server] launch: exec_shmem not mapped\n");
        rep_u32(rep, 0, 1);
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    uint32_t auth_token = (uint32_t)msg_u32(req, 4);
    uint32_t cap_mask   = (uint32_t)msg_u32(req, 8);

    /* find free exec_table entry */
    uint32_t idx = EXEC_MAX_TABLE;
    for (uint32_t i = 0; i < EXEC_MAX_TABLE; i++) {
        if (exec_table[i].state == EXEC_FREE) { idx = i; break; }
    }
    if (idx == EXEC_MAX_TABLE) {
        sel4_dbg_puts("[exec_server] exec table full\n");
        rep_u32(rep, 0, 2);
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    int slot = find_free_app_slot();
    if (slot < 0) {
        sel4_dbg_puts("[exec_server] no free app_slot\n");
        rep_u32(rep, 0, 3);
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    /* copy path from shmem */
    const char *src_path = (const char *)exec_shmem_vaddr;
    copy_path(exec_table[idx].path, src_path, (int)EXEC_PATH_MAX);

    /* Assign exec_id: use 1-based slot index for stable identity */
    uint32_t exec_id = (uint32_t)(idx + 1);
    uint32_t pid     = next_pid++;

    exec_table[idx].exec_id    = exec_id;
    exec_table[idx].auth_token = auth_token;
    exec_table[idx].cap_mask   = cap_mask;
    exec_table[idx].pid        = pid;
    exec_table[idx].state      = EXEC_LOADING;
    exec_table[idx].app_slot_id = (uint8_t)slot;

    uint8_t bin_type = peek_binary_type();

    /* Notify the target app_slot to begin execution.
     * We write the exec_id, pid, cap_mask and binary type into the shmem
     * region at offset 128 (after the path), then notify the app_slot.
     *
     * In the real system, app_slot reads the VFS path, loads the binary,
     * and calls back via OP_PROC_SPAWN.  For Phase 1, the notification
     * triggers app_slot.c's notified() path. */
    volatile uint32_t *meta = (volatile uint32_t *)(exec_shmem_vaddr + 128u);
    meta[0] = exec_id;
    meta[1] = pid;
    meta[2] = cap_mask;
    meta[3] = (uint32_t)bin_type;

    /* Signal app_slot PD to load the staged ELF.
     * CH_APP_SLOT_0 = 34; app_slot_N is at channel (34 + N). */
    sel4_dbg_puts("[E5-S8] notify-stub\n"); /* TODO: seL4_Signal(notify_cap_for_app_slot) */

    /* Phase 1: optimistically transition to RUNNING immediately */
    exec_table[idx].state = EXEC_RUNNING;

    dbg("[exec_server] launched exec_id=");
    dbg(exec_table[idx].path);
    dbg("\n");

    rep_u32(rep, 0, 0);
    rep_u32(rep, 4, exec_id);
    rep->length = 8;
        return SEL4_ERR_OK;
}

static uint32_t handle_status(void)
{
    IPC_STUB_LOCALS
    uint32_t exec_id = (uint32_t)msg_u32(req, 4);
    exec_entry_t *e  = find_task_by_id(exec_id);
    if (!e) e = find_exec(exec_id);
    if (!e) {
        rep_u32(rep, 0, 1);
        rep->length = 4;
        return SEL4_ERR_OK;
    }
    rep_u32(rep, 0, 0);
    rep_u32(rep, 4, e->state);
    rep_u32(rep, 8, e->pid);
    rep->length = 12;
        return SEL4_ERR_OK;
}

static uint32_t handle_wait(void)
{
    IPC_STUB_LOCALS
    uint32_t exec_id = (uint32_t)msg_u32(req, 4);
    exec_entry_t *e  = find_task_by_id(exec_id);
    if (!e) e = find_exec(exec_id);
    if (!e) {
        rep_u32(rep, 0, 1);
        rep->length = 4;
        return SEL4_ERR_OK;
    }
    if (e->state == EXEC_STATE_RUNNING) {
        /* Non-blocking poll: returns current pid; caller must retry if LOADING */
        rep_u32(rep, 0, 0);
        rep_u32(rep, 4, e->pid);
        rep->length = 8;
        return SEL4_ERR_OK;
    }
    /* Still loading — return busy status */
    rep_u32(rep, 0, 1);
    rep->length = 4;
        return SEL4_ERR_OK;
}

static uint32_t handle_kill(void)
{
    IPC_STUB_LOCALS
    uint32_t exec_id = (uint32_t)msg_u32(req, 4);
    exec_entry_t *e  = find_task_by_id(exec_id);
    if (!e) e = find_exec(exec_id);
    if (!e) {
        rep_u32(rep, 0, 1);
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    e->state = EXEC_STATE_DONE;

    rep_u32(rep, 0, 0);
    rep->length = 4;
        return SEL4_ERR_OK;
}

/* ── Microkit entry points ────────────────────────────────────────────── */
static void exec_server_pd_init(void)
{
    for (uint32_t i = 0; i < EXEC_MAX_TABLE; i++) {
        exec_table[i].state      = EXEC_FREE;
        exec_table[i].app_slot_id = 0;
        exec_table[i].exec_id    = 0;
        exec_table[i].pid        = 0;
        exec_table[i].auth_token = 0;
        exec_table[i].cap_mask   = 0;
        exec_table[i].path[0]    = '\0';
    }
    dbg("[exec_server] ELF/WASM loader ready (8 exec slots)\n");
}

static uint32_t exec_server_h_dispatch(sel4_badge_t b, const sel4_msg_t *req, sel4_msg_t *rep, void *ctx)
{
    (void)b; (void)ctx;
    uint32_t op = (uint32_t)msg_u32(req, 0);

    switch (op) {
    case OP_EXEC_LAUNCH: return handle_launch();
    case OP_EXEC_STATUS: return handle_status();
    case OP_EXEC_WAIT:   return handle_wait();
    case OP_EXEC_KILL:   return handle_kill();
    default:
        sel4_dbg_puts("[exec_server] unknown opcode\n");
        rep_u32(rep, 0, 0xFF);
        rep->length = 4;
        return SEL4_ERR_OK;
    }
}

static void exec_server_pd_notified(uint32_t ch)
{
    /*
     * app_slot_N fires back on its end of the bidirectional channel that
     * exec_server holds as id=(CH_APP_SLOT_0 + N).  Map the channel to the
     * corresponding app_slot_id, then mark the matching task DONE.
     */
    if (ch < CH_APP_SLOT_0 || ch > CH_APP_SLOT_0 + 3u)
        return;

    uint8_t slot_id = (uint8_t)(ch - CH_APP_SLOT_0);

    for (uint32_t i = 0; i < EXEC_MAX_TABLE; i++) {
        if (exec_table[i].app_slot_id == slot_id &&
            (exec_table[i].state == EXEC_STATE_LOADING ||
             exec_table[i].state == EXEC_STATE_RUNNING)) {
            exec_table[i].state = EXEC_STATE_DONE;
            dbg("[exec_server] notified: slot=");
            char sc[2] = { '0' + (char)slot_id, '\0' };
            sel4_dbg_puts(sc);
            dbg(" -> DONE\n");
            break;
        }
    }
}

/* ── E5-S8: Entry point ─────────────────────────────────────────────────── */
void exec_server_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    (void)ns_ep;
    exec_server_pd_init();
    static sel4_server_t srv;
    sel4_server_init(&srv, my_ep);
    /* Dispatch all opcodes through the generic handler */
    sel4_server_register(&srv, SEL4_SERVER_OPCODE_ANY, exec_server_h_dispatch, (void *)0);
    sel4_server_run(&srv);
}
