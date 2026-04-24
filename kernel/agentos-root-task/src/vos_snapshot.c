/*
 * vos_snapshot.c — VOS instance SNAPSHOT operation and in-memory blob store
 *
 * Captures a point-in-time snapshot of a guest OS instance.  Under
 * AGENTOS_TEST_HOST the guest's vos_instance_t is serialised into an in-memory
 * blob store (no AgentFS, no seL4 capabilities).
 *
 * The blob store is also used by vos_restore.c to retrieve snapshots.  Both
 * compilation units share the store through the functions declared in
 * vos_snap_store.h:
 *   agentfs_put_blob()     — store a blob, return a slot index (snap_lo, snap_hi)
 *   agentfs_get_blob()     — retrieve a blob by slot index
 *   vos_test_snap_store_*  — test-only accessors / reset
 *
 * Public API:
 *   vos_snapshot_init()    — reset blob store (call before each test run)
 *   vos_snapshot()         — capture snapshot; transitions state to SUSPENDED
 *   vos_test_alloc_snap()  — test-only: directly insert a blob into the store
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdint.h>
#include <string.h>
#include "vos_types.h"
#include "vos_snap_store.h"

/* ── Blob store ──────────────────────────────────────────────────────────────── */

static snap_store_entry_t g_store[VOS_SNAP_STORE_MAX];

/* ── vos_snapshot_init ───────────────────────────────────────────────────────── */

void vos_snapshot_init(void)
{
    memset(g_store, 0, sizeof(g_store));
}

/* ── agentfs_put_blob ────────────────────────────────────────────────────────── */

/*
 * Slot 0 is reserved as the null/invalid sentinel (token (0,0) always means
 * "no snapshot").  Allocations start from index 1.
 */
int agentfs_put_blob(const void *data, uint32_t len,
                     uint32_t *snap_lo_out, uint32_t *snap_hi_out)
{
    uint32_t i;

    if (!data || len == 0 || len > VOS_SNAP_BLOB_MAX)
        return -1;

    /* Start from index 1; slot 0 is the null sentinel */
    for (i = 1; i < VOS_SNAP_STORE_MAX; i++) {
        if (!g_store[i].used) {
            memcpy(g_store[i].blob, data, len);
            g_store[i].blob_len = len;
            g_store[i].used     = 1;
            if (snap_lo_out) *snap_lo_out = i;
            if (snap_hi_out) *snap_hi_out = 0u;
            return 0;
        }
    }
    return -1;  /* no free slot */
}

/* ── agentfs_get_blob ────────────────────────────────────────────────────────── */

int agentfs_get_blob(uint32_t snap_lo, uint32_t snap_hi,
                     void *buf, uint32_t buf_len, uint32_t *len_out)
{
    (void)snap_hi;  /* unused in stub */

    if (snap_lo >= VOS_SNAP_STORE_MAX)
        return -1;
    if (!g_store[snap_lo].used)
        return -1;
    if (!buf || buf_len < g_store[snap_lo].blob_len)
        return -1;

    memcpy(buf, g_store[snap_lo].blob, g_store[snap_lo].blob_len);
    if (len_out) *len_out = g_store[snap_lo].blob_len;
    return 0;
}

/* ── Test-only accessors ─────────────────────────────────────────────────────── */

snap_store_entry_t *vos_test_snap_store_get(uint32_t idx)
{
    if (idx >= VOS_SNAP_STORE_MAX) return NULL;
    return &g_store[idx];
}

void vos_test_snap_store_put(uint32_t idx, const snap_store_entry_t *e)
{
    if (idx >= VOS_SNAP_STORE_MAX || !e) return;
    g_store[idx] = *e;
}

void vos_test_snap_store_reset(void)
{
    vos_snapshot_init();
}

/* ── vos_snapshot ────────────────────────────────────────────────────────────── */

/*
 * vos_snapshot(handle, snap_lo_out, snap_hi_out)
 *
 * Serialise the live instance identified by handle into the blob store.
 * Transitions the instance state from RUNNING to SUSPENDED.
 * Writes the blob store slot index into *snap_lo_out / *snap_hi_out.
 */
vos_err_t vos_snapshot(vos_handle_t handle,
                        uint32_t *snap_lo_out, uint32_t *snap_hi_out)
{
    vos_instance_t *inst;
    int             rc;

    if (!snap_lo_out || !snap_hi_out)
        return VOS_ERR_INTERNAL;

    inst = vos_instance_get(handle);
    if (!inst)
        return VOS_ERR_INVALID_HANDLE;

    /* Suspend the guest while we capture state */
    inst->state = VOS_STATE_SUSPENDED;

    /* Serialise the instance into the blob store */
    rc = agentfs_put_blob(inst, (uint32_t)sizeof(*inst), snap_lo_out, snap_hi_out);
    if (rc != 0)
        return VOS_ERR_SNAPSHOT_FAILED;

    return VOS_ERR_OK;
}

/* ── vos_test_alloc_snap ─────────────────────────────────────────────────────── */

/*
 * vos_test_alloc_snap(data, len, idx_out)
 *
 * Test-only helper: directly insert a blob into the store without going through
 * vos_snapshot.  Useful for setting up restore preconditions.
 */
vos_err_t vos_test_alloc_snap(const void *data, uint32_t len, uint32_t *idx_out)
{
    uint32_t lo = 0, hi = 0;
    int      rc = agentfs_put_blob(data, len, &lo, &hi);
    if (rc != 0) return VOS_ERR_SNAPSHOT_FAILED;
    if (idx_out) *idx_out = lo;
    return VOS_ERR_OK;
}
