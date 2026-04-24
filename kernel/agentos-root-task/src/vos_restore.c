/*
 * vos_restore.c — VOS instance RESTORE operation
 *
 * Creates a new guest OS instance by deserialising a previously captured
 * snapshot blob.  The new instance gets a fresh handle but inherits the
 * state (os_type, memory layout, device bindings) from the snapshot.
 *
 * Under AGENTOS_TEST_HOST the snapshot is fetched from the in-memory blob
 * store via agentfs_get_blob() (implemented in vos_snapshot.c).  The
 * restored instance is created via vos_test_alloc_instance() so that it
 * shares the same handle counter as instances created by vos_create().
 *
 * Public API:
 *   vos_restore(snap_lo, snap_hi, spec, handle_out) — restore from snapshot
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdint.h>
#include <string.h>
#include "vos_types.h"
#include "vos_snap_store.h"

vos_err_t vos_restore(uint32_t snap_lo, uint32_t snap_hi,
                       const vos_spec_t *spec, vos_handle_t *handle_out)
{
    vos_instance_t  restored;
    uint32_t        blob_len = 0;
    vos_handle_t    new_handle;
    vos_err_t       err;
    vos_instance_t *slot;

    (void)spec;  /* spec is advisory in stub: os_type/pages come from snapshot */

    if (!handle_out)
        return VOS_ERR_INTERNAL;

    /* Zero token → definitely invalid */
    if (snap_lo == 0u && snap_hi == 0u)
        return VOS_ERR_INVALID_HANDLE;

    /* Retrieve the serialised instance from the blob store */
    if (agentfs_get_blob(snap_lo, snap_hi,
                         &restored, (uint32_t)sizeof(restored),
                         &blob_len) != 0)
        return VOS_ERR_SNAP_NOT_FOUND;

    if (blob_len != sizeof(restored))
        return VOS_ERR_SNAP_NOT_FOUND;

    /* Allocate a fresh instance slot using the restored os_type */
    err = vos_test_alloc_instance(restored.os_type, VOS_STATE_RUNNING, &new_handle);
    if (err != VOS_ERR_OK)
        return err;

    /* Overwrite the freshly-allocated slot with the full restored image,
     * keeping the newly-assigned handle so the slot is addressable. */
    slot = vos_instance_get(new_handle);
    if (!slot) {
        /* should not happen, but be defensive */
        return VOS_ERR_INTERNAL;
    }
    restored.handle = new_handle;
    restored.state  = VOS_STATE_RUNNING;  /* restored instances resume running */
    *slot = restored;

    *handle_out = new_handle;
    return VOS_ERR_OK;
}
