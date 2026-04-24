/*
 * proc_server.c — Process management for agentOS
 *
 * HURD-equivalent: proc server
 * Priority: 160 (passive)
 *
 * Maintains a process table of up to PROC_MAX processes.
 * Process states: FREE → RUNNING → ZOMBIE/STOPPED
 *
 * Channel assignments:
 *   id=0: receives PPC from controller
 *
 * Shared memory:
 *   proc_shmem: 8KB output region for OP_PROC_LIST (proc_info_t[])
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef AGENTOS_TEST_HOST
#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "sel4_server.h"
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* ── Configuration ─────────────────────────────────────────────────────── */
#define PROC_MAX           32
#define PROC_STATE_FREE    0u
#define PROC_STATE_RUNNING 1u
#define PROC_STATE_ZOMBIE  2u
#define PROC_STATE_STOPPED 3u

/* ── Shared memory ─────────────────────────────────────────────────────── */
#ifndef AGENTOS_TEST_HOST
uintptr_t proc_shmem_vaddr;  /* set by Microkit linker */
#endif

/* ── Process entry ─────────────────────────────────────────────────────── */
typedef struct {
    uint32_t pid;
    uint32_t parent_pid;
    uint32_t auth_token;   /* from auth_server */
    uint32_t cap_mask;
    uint8_t  state;
    uint8_t  exit_code;
    uint8_t  _pad[2];
    char     name[16];
} proc_entry_t;

/* On-wire process info for OP_PROC_LIST output in shmem */
typedef struct __attribute__((packed)) {
    uint32_t pid;
    uint32_t parent_pid;
    uint8_t  state;
    uint8_t  exit_code;
    uint8_t  _pad[2];
    char     name[16];
} proc_info_t;

static proc_entry_t procs[PROC_MAX];
static uint32_t next_pid = 1;

/* ── Helpers ───────────────────────────────────────────────────────────── */
static proc_entry_t *find_proc(uint32_t pid) {
    for (int i = 0; i < PROC_MAX; i++)
        if (procs[i].state != PROC_STATE_FREE && procs[i].pid == pid)
            return &procs[i];
    return NULL;
}

static proc_entry_t *alloc_proc(void) {
    for (int i = 0; i < PROC_MAX; i++)
        if (procs[i].state == PROC_STATE_FREE) return &procs[i];
    return NULL;
}

/* ── Microkit entry points ─────────────────────────────────────────────── */

static void proc_server_pd_init(void)
{
    for (int i = 0; i < PROC_MAX; i++) procs[i].state = PROC_STATE_FREE;

    /* Pre-insert pid=0 "kernel" */
    procs[0].pid        = 0;
    procs[0].parent_pid = 0;
    procs[0].auth_token = 0;
    procs[0].cap_mask   = 0xFFu;
    procs[0].state      = PROC_STATE_RUNNING;
    procs[0].exit_code  = 0;
    procs[0].name[0] = 'k'; procs[0].name[1] = 'e';
    procs[0].name[2] = 'r'; procs[0].name[3] = 'n';
    procs[0].name[4] = 'e'; procs[0].name[5] = 'l';
    procs[0].name[6] = '\0';

    sel4_dbg_puts("[proc_server] READY: process table initialized\n");
}

static void proc_server_pd_notified(uint32_t ch) { (void)ch; }

static uint32_t proc_server_pd_dispatch(sel4_badge_t b, const sel4_msg_t *req, sel4_msg_t *rep, void *ctx)
{
    (void)b; (void)ctx;
    uint32_t op = (uint32_t)msg_u32(req, 0);

    switch (op) {

    /* OP_PROC_SPAWN: MR1=parent_pid, MR2=auth_token, MR3=cap_mask → MR0=ok, MR1=pid */
    case OP_PROC_SPAWN: {
        uint32_t parent_pid  = (uint32_t)msg_u32(req, 4);
        uint32_t auth_token  = (uint32_t)msg_u32(req, 8);
        uint32_t cap_mask    = (uint32_t)msg_u32(req, 12);
        proc_entry_t *p = alloc_proc();
        if (!p) {
            rep_u32(rep, 0, 0xFFu);  /* PROC_ERR_FULL */
            rep->length = 4;
        return SEL4_ERR_OK;
        }
        p->pid        = next_pid++;
        p->parent_pid = parent_pid;
        p->auth_token = auth_token;
        p->cap_mask   = cap_mask;
        p->state      = PROC_STATE_RUNNING;
        p->exit_code  = 0;
        /* Read name from proc_shmem if available */
        if (proc_shmem_vaddr) {
            const char *name = (const char *)(uintptr_t)proc_shmem_vaddr;
            for (int i = 0; i < 15; i++) {
                p->name[i] = name[i];
                if (!name[i]) break;
            }
            p->name[15] = '\0';
        } else {
            p->name[0] = 'p'; p->name[1] = '\0';
        }
        rep_u32(rep, 0, 0u);
        rep_u32(rep, 4, p->pid);
        rep->length = 8;
        return SEL4_ERR_OK;
    }

    /* OP_PROC_EXIT: MR1=pid, MR2=exit_code → MR0=ok */
    case OP_PROC_EXIT: {
        uint32_t pid       = (uint32_t)msg_u32(req, 4);
        uint8_t  exit_code = (uint8_t)msg_u32(req, 8);
        proc_entry_t *p = find_proc(pid);
        if (p) {
            p->state     = PROC_STATE_ZOMBIE;
            p->exit_code = exit_code;
        }
        rep_u32(rep, 0, 0u);
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    /* OP_PROC_WAIT: MR1=pid → MR0=ok, MR1=exit_code, MR2=state
     * Returns immediately if already ZOMBIE (Phase 1: no blocking) */
    case OP_PROC_WAIT: {
        uint32_t pid = (uint32_t)msg_u32(req, 4);
        proc_entry_t *p = find_proc(pid);
        if (!p) {
            rep_u32(rep, 0, 0xFFu);
            rep->length = 4;
        return SEL4_ERR_OK;
        }
        /* For Phase 1: return current state; don't actually block */
        rep_u32(rep, 0, 0u);
        rep_u32(rep, 4, p->exit_code);
        rep_u32(rep, 8, p->state);
        rep->length = 12;
        return SEL4_ERR_OK;
    }

    /* OP_PROC_STATUS: MR1=pid → MR0=ok, MR1=state, MR2=cap_mask */
    case OP_PROC_STATUS: {
        uint32_t pid = (uint32_t)msg_u32(req, 4);
        proc_entry_t *p = find_proc(pid);
        if (!p) {
            rep_u32(rep, 0, 0xFFu);
            rep->length = 4;
        return SEL4_ERR_OK;
        }
        rep_u32(rep, 0, 0u);
        rep_u32(rep, 4, p->state);
        rep_u32(rep, 8, p->cap_mask);
        rep->length = 12;
        return SEL4_ERR_OK;
    }

    /* OP_PROC_LIST: → MR0=ok, MR1=count; proc_info_t[] in proc_shmem */
    case OP_PROC_LIST: {
        uint32_t count = 0;
        if (proc_shmem_vaddr) {
            proc_info_t *infos = (proc_info_t *)(uintptr_t)proc_shmem_vaddr;
            for (int i = 0; i < PROC_MAX; i++) {
                if (procs[i].state != PROC_STATE_FREE) {
                    infos[count].pid        = procs[i].pid;
                    infos[count].parent_pid = procs[i].parent_pid;
                    infos[count].state      = procs[i].state;
                    infos[count].exit_code  = procs[i].exit_code;
                    for (int j = 0; j < 16; j++) infos[count].name[j] = procs[i].name[j];
                    count++;
                }
            }
        }
        rep_u32(rep, 0, 0u);
        rep_u32(rep, 4, count);
        rep->length = 8;
        return SEL4_ERR_OK;
    }

    /* OP_PROC_KILL: MR1=pid, MR2=signal → MR0=ok */
    case OP_PROC_KILL: {
        uint32_t pid    = (uint32_t)msg_u32(req, 4);
        uint32_t signal = (uint32_t)msg_u32(req, 8);
        proc_entry_t *p = find_proc(pid);
        if (!p) {
            rep_u32(rep, 0, 0xFFu);
            rep->length = 4;
        return SEL4_ERR_OK;
        }
        if (signal == 9 || signal == 15) {
            /* SIGKILL / SIGTERM: immediately zombie */
            p->state     = PROC_STATE_ZOMBIE;
            p->exit_code = (uint8_t)(128 + signal);
        } else if (signal == 19 || signal == 17) {
            /* SIGSTOP / SIGTSTP */
            p->state = PROC_STATE_STOPPED;
        } else if (signal == 18) {
            /* SIGCONT */
            if (p->state == PROC_STATE_STOPPED)
                p->state = PROC_STATE_RUNNING;
        }
        rep_u32(rep, 0, 0u);
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    /* OP_PROC_SETCAP: MR1=pid, MR2=cap_mask → MR0=ok */
    case OP_PROC_SETCAP: {
        uint32_t pid      = (uint32_t)msg_u32(req, 4);
        uint32_t cap_mask = (uint32_t)msg_u32(req, 8);
        proc_entry_t *p = find_proc(pid);
        if (p) {
            p->cap_mask = cap_mask;
            rep_u32(rep, 0, 0u);
        } else {
            rep_u32(rep, 0, 0xFFu);
        }
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    default:
        rep_u32(rep, 0, 0xFFu);
        rep->length = 4;
        return SEL4_ERR_OK;
    }
}
