/*
 * cap_accounting.c — capability ownership tracking
 *
 * Records every kernel object capability created during root-task boot in a
 * fixed-size static table.  Each entry tracks the CPtr, seL4 object type,
 * owning PD index, and a short diagnostic name.
 *
 * The table is append-only during boot; no deletion or rebalancing is
 * performed.  The monitor PD and fault handler may query it via
 * cap_acct_get() to enumerate capabilities owned by a specific PD.
 *
 * No libc, no malloc.  All storage is in static arrays.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "cap_accounting.h"
#include "boot_info.h"

/* ── Module state ─────────────────────────────────────────────────────────── */

static cap_acct_entry_t  g_table[CAP_ACCT_MAX_ENTRIES];
static uint32_t          g_count;

/* ── Internal helpers ─────────────────────────────────────────────────────── */

/* Minimal strncpy substitute — no libc dependency */
static void acct_strlcpy(char *dst, const char *src, uint32_t max)
{
    uint32_t i = 0u;
    while (i + 1u < max && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* ── Initialisation ───────────────────────────────────────────────────────── */

void cap_acct_init(const seL4_BootInfo *bi)
{
    g_count = 0u;

    /* Zero the entire table for deterministic initial state */
    for (uint32_t i = 0u; i < CAP_ACCT_MAX_ENTRIES; i++) {
        g_table[i].cap      = seL4_CapNull;
        g_table[i].obj_type = 0u;
        g_table[i].pd_index = 0u;
        g_table[i].name[0]  = '\0';
    }

    /*
     * Record the root task's well-known initial capabilities.
     * These are the caps seL4 places in fixed slots before the root task
     * begins execution.  We record them as owned by PD 0 (root task itself).
     */
    (void)bi;  /* BootInfo consulted in future for dynamic slot layout */

    cap_acct_record(seL4_CapNull, seL4_CapInitThreadCNode,
                    seL4_CapTableObject, 0u, "root-cnode");
    cap_acct_record(seL4_CapNull, seL4_CapInitThreadVSpace,
                    seL4_ARM_VSpaceObject, 0u, "root-vspace");
    cap_acct_record(seL4_CapNull, seL4_CapInitThreadTCB,
                    seL4_TCBObject, 0u, "root-tcb");
}

/* ── Public interface ─────────────────────────────────────────────────────── */

int cap_acct_record(seL4_CPtr   parent,
                    seL4_CPtr   cap,
                    uint32_t    obj_type,
                    uint32_t    pd_index,
                    const char *name)
{
    (void)parent;  /* reserved for future revocation-tree tracking */

    if (g_count >= CAP_ACCT_MAX_ENTRIES) {
        return -1;
    }

    cap_acct_entry_t *e = &g_table[g_count];
    e->cap       = cap;
    e->obj_type  = obj_type;
    e->pd_index  = pd_index;
    acct_strlcpy(e->name, name ? name : "", sizeof(e->name));

    g_count++;
    return 0;
}

uint32_t cap_acct_count(void)
{
    return g_count;
}

const cap_acct_entry_t *cap_acct_get(uint32_t index)
{
    if (index >= g_count) {
        return (const cap_acct_entry_t *)0;
    }
    return &g_table[index];
}
