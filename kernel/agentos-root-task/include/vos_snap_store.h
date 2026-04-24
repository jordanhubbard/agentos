/*
 * vos_snap_store.h — in-memory blob store shared between vos_snapshot and vos_restore
 *
 * Under AGENTOS_TEST_HOST this store lives in static arrays; on real hardware it
 * would be backed by AgentFS.  The snap_token is a 64-bit value (lo + hi) that
 * encodes the slot index used by agentfs_put_blob / agentfs_get_blob.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Maximum number of simultaneous snapshots in the test-host blob store. */
#define VOS_SNAP_STORE_MAX  16u

/*
 * snap_store_entry_t — one cell in the in-memory blob store.
 *
 * blob     — opaque byte payload (callee-owned copy of the snapshot)
 * blob_len — number of valid bytes in blob[]
 * used     — true when this slot is occupied
 */
#define VOS_SNAP_BLOB_MAX  4096u

typedef struct {
    uint8_t  blob[VOS_SNAP_BLOB_MAX];
    uint32_t blob_len;
    bool     used;
} snap_store_entry_t;

/* ── Functions implemented in vos_snapshot.c ────────────────────────────────── */

/*
 * vos_snapshot_init() — reset the in-memory blob store.  Must be called at
 * the start of each test run to ensure a clean slate.
 */
void vos_snapshot_init(void);

/*
 * agentfs_put_blob(data, len, snap_lo_out, snap_hi_out)
 *
 * Store `len` bytes from `data` in the next free slot.  On success writes the
 * slot index into *snap_lo_out (snap_hi_out is always set to 0 in the stub).
 * Returns 0 on success, -1 if no free slot.
 */
int agentfs_put_blob(const void *data, uint32_t len,
                     uint32_t *snap_lo_out, uint32_t *snap_hi_out);

/*
 * agentfs_get_blob(snap_lo, snap_hi, buf, buf_len, len_out)
 *
 * Retrieve the blob stored at slot snap_lo into buf (up to buf_len bytes).
 * On success writes the actual byte count into *len_out.
 * Returns 0 on success, -1 if the slot is empty or index is out of range.
 */
int agentfs_get_blob(uint32_t snap_lo, uint32_t snap_hi,
                     void *buf, uint32_t buf_len, uint32_t *len_out);

/*
 * vos_test_snap_store_get(idx) — return pointer to entry at idx (test use only).
 * vos_test_snap_store_put(idx, entry) — overwrite entry at idx (test use only).
 * vos_test_snap_store_reset() — equivalent to vos_snapshot_init() (test use only).
 */
snap_store_entry_t *vos_test_snap_store_get(uint32_t idx);
void                vos_test_snap_store_put(uint32_t idx, const snap_store_entry_t *e);
void                vos_test_snap_store_reset(void);
