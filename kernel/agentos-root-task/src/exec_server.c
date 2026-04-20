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

#include "agentos.h"
#include "contracts/exec_server_contract.h"
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
uintptr_t exec_shmem_vaddr;      /* 4KB shmem: path string at offset 0 */

/* ── Exec table entry ─────────────────────────────────────────────────── */
typedef struct {
    uint32_t exec_id;
    uint32_t auth_token;
    uint32_t cap_mask;
    uint32_t pid;
    uint8_t  state;
    uint8_t  app_slot_id;
    uint8_t  _pad[2];
    char     path[EXEC_PATH_MAX];
} exec_entry_t;

/* ── App slot availability tracking ──────────────────────────────────── */
typedef struct {
    bool     in_use;
    uint32_t exec_id;
} slot_state_t;

static exec_entry_t exec_table[EXEC_MAX_TABLE];
static slot_state_t slot_states[EXEC_APP_SLOTS];
static uint32_t     next_exec_id = 1;
static uint32_t     next_pid     = 100;  /* PIDs 100+ for exec-launched procs */

/* ── Helpers ──────────────────────────────────────────────────────────── */
static exec_entry_t *find_exec(uint32_t exec_id)
{
    for (uint32_t i = 0; i < EXEC_MAX_TABLE; i++) {
        if (exec_table[i].state != EXEC_FREE && exec_table[i].exec_id == exec_id)
            return &exec_table[i];
    }
    return NULL;
}

static int find_free_slot(void)
{
    for (uint32_t i = 0; i < EXEC_APP_SLOTS; i++) {
        if (!slot_states[i].in_use) return (int)i;
    }
    return -1;
}

static uint8_t peek_binary_type(void)
{
    /* Probe exec_shmem at offset 256 (after path string) for binary header.
     * For Phase 1, the binary isn't actually loaded; we infer from path
     * extension. This is a pragmatic simplification: real loading would
     * request OP_VFS_READ from vfs_server. */
    const char *path = (const char *)exec_shmem_vaddr;
    uint32_t len = 0;
    while (len < EXEC_PATH_MAX && path[len] != '\0') len++;

    /* Check extension: .wasm → WASM, otherwise → ELF */
    if (len >= 5 && path[len-5] == '.' && path[len-4] == 'w' &&
        path[len-3] == 'a' && path[len-2] == 's' && path[len-1] == 'm')
        return 1; /* WASM */
    return 0;     /* ELF */
}

/* Channel IDs for app_slot PPCs (app_slot PDs are channels 60..63 in controller) */
#define APP_SLOT_CH_BASE  60u

/* ── IPC dispatch ─────────────────────────────────────────────────────── */
static microkit_msginfo handle_launch(void)
{
    uint32_t auth_token = (uint32_t)microkit_mr_get(1);
    uint32_t cap_mask   = (uint32_t)microkit_mr_get(2);

    /* find free exec_table entry */
    uint32_t idx = EXEC_MAX_TABLE;
    for (uint32_t i = 0; i < EXEC_MAX_TABLE; i++) {
        if (exec_table[i].state == EXEC_FREE) { idx = i; break; }
    }
    if (idx == EXEC_MAX_TABLE) {
        microkit_dbg_puts("[exec_server] exec table full\n");
        microkit_mr_set(0, 0);
        return microkit_msginfo_new(0, 1);
    }

    int slot = find_free_slot();
    if (slot < 0) {
        microkit_dbg_puts("[exec_server] no free app_slot\n");
        microkit_mr_set(0, 0);
        return microkit_msginfo_new(0, 1);
    }

    /* copy path from shmem */
    const char *src_path = (const char *)exec_shmem_vaddr;
    uint32_t i = 0;
    for (; i < EXEC_PATH_MAX - 1 && src_path[i] != '\0'; i++)
        exec_table[idx].path[i] = src_path[i];
    exec_table[idx].path[i] = '\0';

    uint32_t exec_id = next_exec_id++;
    uint32_t pid     = next_pid++;

    exec_table[idx].exec_id    = exec_id;
    exec_table[idx].auth_token = auth_token;
    exec_table[idx].cap_mask   = cap_mask;
    exec_table[idx].pid        = pid;
    exec_table[idx].state      = EXEC_LOADING;
    exec_table[idx].app_slot_id = (uint8_t)slot;

    slot_states[slot].in_use  = true;
    slot_states[slot].exec_id = exec_id;

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

    microkit_notify((microkit_channel)(APP_SLOT_CH_BASE + (uint32_t)slot));
    exec_table[idx].state = EXEC_RUNNING;

    microkit_dbg_puts("[exec_server] launched exec_id=");
    microkit_dbg_puts(exec_table[idx].path);
    microkit_dbg_puts("\n");

    microkit_mr_set(0, 1);
    microkit_mr_set(1, exec_id);
    return microkit_msginfo_new(0, 2);
}

static microkit_msginfo handle_status(void)
{
    uint32_t exec_id = (uint32_t)microkit_mr_get(1);
    exec_entry_t *e  = find_exec(exec_id);
    if (!e) {
        microkit_mr_set(0, 0);
        return microkit_msginfo_new(0, 1);
    }
    microkit_mr_set(0, 1);
    microkit_mr_set(1, e->state);
    microkit_mr_set(2, e->pid);
    return microkit_msginfo_new(0, 3);
}

static microkit_msginfo handle_wait(void)
{
    uint32_t exec_id = (uint32_t)microkit_mr_get(1);
    exec_entry_t *e  = find_exec(exec_id);
    if (!e) {
        microkit_mr_set(0, 0);
        return microkit_msginfo_new(0, 1);
    }
    /* Non-blocking poll: returns current pid; caller must retry if LOADING */
    microkit_mr_set(0, 1);
    microkit_mr_set(1, e->pid);
    return microkit_msginfo_new(0, 2);
}

static microkit_msginfo handle_kill(void)
{
    uint32_t exec_id = (uint32_t)microkit_mr_get(1);
    exec_entry_t *e  = find_exec(exec_id);
    if (!e) {
        microkit_mr_set(0, 0);
        return microkit_msginfo_new(0, 1);
    }

    uint8_t slot = e->app_slot_id;
    if (slot < EXEC_APP_SLOTS) {
        slot_states[slot].in_use  = false;
        slot_states[slot].exec_id = 0;
    }
    e->state = EXEC_FREE;

    microkit_mr_set(0, 1);
    return microkit_msginfo_new(0, 1);
}

/* ── Microkit entry points ────────────────────────────────────────────── */
void init(void)
{
    for (uint32_t i = 0; i < EXEC_MAX_TABLE; i++)
        exec_table[i].state = EXEC_FREE;
    for (uint32_t i = 0; i < EXEC_APP_SLOTS; i++) {
        slot_states[i].in_use  = false;
        slot_states[i].exec_id = 0;
    }
    microkit_dbg_puts("[exec_server] ELF/WASM loader ready\n");
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msg)
{
    (void)ch;
    (void)msg;
    uint32_t op = (uint32_t)microkit_mr_get(0);

    switch (op) {
    case OP_EXEC_LAUNCH: return handle_launch();
    case OP_EXEC_STATUS: return handle_status();
    case OP_EXEC_WAIT:   return handle_wait();
    case OP_EXEC_KILL:   return handle_kill();
    default:
        microkit_dbg_puts("[exec_server] unknown opcode\n");
        microkit_mr_set(0, 0);
        return microkit_msginfo_new(0, 1);
    }
}

void notified(microkit_channel ch)
{
    /* app_slot signals completion back via notify */
    for (uint32_t i = 0; i < EXEC_APP_SLOTS; i++) {
        if ((microkit_channel)(APP_SLOT_CH_BASE + i) == ch && slot_states[i].in_use) {
            exec_entry_t *e = find_exec(slot_states[i].exec_id);
            if (e && e->state == EXEC_LOADING)
                e->state = EXEC_RUNNING;
            break;
        }
    }
}
