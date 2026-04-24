/*
 * vos_snap_store.h — Shared in-memory snapshot store for AGENTOS_TEST_HOST.
 *
 * On real seL4 hardware, snapshot blobs are persisted to AgentFS using IPC.
 * When building under AGENTOS_TEST_HOST (host-side unit tests), both
 * vos_snapshot.c (put) and vos_restore.c (get) use this shared static store
 * so that round-trip tests work without any seL4 infrastructure.
 *
 * Token encoding:
 *   snap_lo = slot_index + 1   (1-based so that 0 means "no snapshot")
 *   snap_hi = ~snap_lo         (bitwise complement for integrity check)
 *
 * The store holds up to VOS_SNAP_STORE_MAX blobs, each up to
 * VOS_SNAP_STORE_MAX_BLOB_BYTES bytes.
 *
 * Usage:
 *   vos_test_snap_store_put(buf, size)   → returns (snap_lo, snap_hi) via out-params
 *   vos_test_snap_store_get(snap_lo, snap_hi, buf, size) → bytes copied or -1
 *
 * Both functions are declared here and defined in vos_snapshot.c (which is
 * always compiled alongside vos_restore.c for tests).
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/* Maximum number of stored snapshots (covers VOS_MAX_INSTANCES × a few each) */
#define VOS_SNAP_STORE_MAX           8u

/* Maximum blob size in the host stub store (4 MiB per entry) */
#define VOS_SNAP_STORE_MAX_BLOB_BYTES (4u * 1024u * 1024u)

#ifdef AGENTOS_TEST_HOST

/*
 * vos_test_snap_store_put — store a blob and return the token.
 *
 * Copies up to VOS_SNAP_STORE_MAX_BLOB_BYTES bytes from buf into the next
 * free slot.  On success writes the token into *out_snap_lo and *out_snap_hi
 * and returns 0.  Returns -1 if the store is full or the blob is too large.
 */
int vos_test_snap_store_put(const void *buf, uint32_t size,
                             uint32_t *out_snap_lo, uint32_t *out_snap_hi);

/*
 * vos_test_snap_store_get — retrieve a blob by token.
 *
 * Copies up to buf_size bytes into buf.  Returns the number of bytes copied
 * on success, or -1 if the token is not found.
 */
int vos_test_snap_store_get(uint32_t snap_lo, uint32_t snap_hi,
                             void *buf, uint32_t buf_size);

/*
 * vos_test_snap_store_reset — clear all stored blobs (call between tests).
 */
void vos_test_snap_store_reset(void);

#endif /* AGENTOS_TEST_HOST */
