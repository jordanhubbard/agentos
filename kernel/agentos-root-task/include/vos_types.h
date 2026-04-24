/*
 * vos_types.h — VOS instance types shared between vos_create, vos_destroy,
 *               vos_snapshot, and vos_restore.
 *
 * This header mirrors the key definitions from contracts/vibeos/interface.h
 * so that the vos_*.c implementation files can be compiled with only
 *   -I kernel/agentos-root-task/include
 * without needing the top-level contracts/ directory in the include path.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── Opaque handle ───────────────────────────────────────────────────────────── */

typedef uint32_t vos_handle_t;
#define VOS_HANDLE_INVALID  UINT32_C(0xFFFFFFFF)

/* ── OS type ─────────────────────────────────────────────────────────────────── */

typedef enum __attribute__((packed)) {
    VOS_OS_LINUX   = 0,
    VOS_OS_FREEBSD = 1,
    VOS_OS_CUSTOM  = 2,
} vos_os_type_t;

/* ── Instance state ──────────────────────────────────────────────────────────── */

typedef enum __attribute__((packed)) {
    VOS_STATE_CREATING  = 0,
    VOS_STATE_RUNNING   = 1,
    VOS_STATE_SUSPENDED = 2,
    VOS_STATE_DESTROYED = 3,
} vos_state_t;

/* ── Error codes ─────────────────────────────────────────────────────────────── */

typedef uint32_t vos_err_t;

#define VOS_ERR_OK                UINT32_C(0)
#define VOS_ERR_INVALID_HANDLE    UINT32_C(1)
#define VOS_ERR_OUT_OF_MEMORY     UINT32_C(2)
#define VOS_ERR_PERMISSION_DENIED UINT32_C(3)
#define VOS_ERR_INVALID_SPEC      UINT32_C(4)
#define VOS_ERR_UNSUPPORTED_OS    UINT32_C(5)
#define VOS_ERR_SERVICE_UNAVAIL   UINT32_C(6)
#define VOS_ERR_ALREADY_EXISTS    UINT32_C(7)
#define VOS_ERR_SNAPSHOT_FAILED   UINT32_C(8)
#define VOS_ERR_MIGRATE_FAILED    UINT32_C(9)
#define VOS_ERR_NOT_SUPPORTED     UINT32_C(10)
#define VOS_ERR_SNAP_NOT_FOUND    UINT32_C(11)
#define VOS_ERR_INTERNAL          UINT32_C(99)

/* ── Spec and status structs ─────────────────────────────────────────────────── */

#define VOS_SPEC_MIN_PAGES  UINT32_C(256)
#define VOS_SPEC_MAX_PAGES  UINT32_C(524288)
#define VOS_MAX_INSTANCES   4u

typedef struct __attribute__((packed)) {
    vos_os_type_t os_type;
    uint8_t       vcpu_count;
    uint8_t       cpu_quota_pct;
    uint8_t       _pad0;
    uint32_t      memory_pages;
    uint32_t      cpu_affinity;
    uint32_t      boot_image_cap;
    uint32_t      config_len;
    uint64_t      config_blob;
    char          label[16];
} vos_spec_t;

typedef struct __attribute__((packed)) {
    vos_handle_t  handle;
    vos_state_t   state;
    vos_os_type_t os_type;
    uint8_t       cpu_quota_pct;
    uint8_t       vcpu_count;
    uint64_t      uptime_ms;
    uint32_t      memory_used_pages;
    uint32_t      memory_total_pages;
    uint64_t      run_ticks;
    uint64_t      preempt_count;
    uint32_t      bound_services;
    uint32_t      _pad;
    char          label[16];
} vos_status_t;

/* ── Instance table entry (internal) ────────────────────────────────────────── */

typedef struct {
    vos_handle_t  handle;     /* VOS_HANDLE_INVALID when slot is free */
    vos_state_t   state;
    vos_os_type_t os_type;
    uint8_t       vcpu_count;
    uint8_t       cpu_quota_pct;
    uint32_t      memory_pages;
    uint32_t      cpu_affinity;
    char          label[16];
} vos_instance_t;

/* ── Public API (vos_create.c) ───────────────────────────────────────────────── */

void       vos_create_init(void);
vos_err_t  vos_create(const vos_spec_t *spec, vos_handle_t *handle_out);
vos_err_t  vos_get_status(vos_handle_t handle, vos_status_t *status_out);
vos_err_t  vos_instance_set_state(vos_handle_t handle, vos_state_t state);
vos_err_t  vos_test_alloc_instance(vos_os_type_t os_type, vos_state_t state,
                                    vos_handle_t *handle_out);
vos_err_t  vos_test_free_instance(vos_handle_t handle);

/*
 * vos_instance_get() — returns pointer into the internal table; used by
 * vos_snapshot.c and vos_destroy.c to inspect/modify the live entry.
 * Returns NULL if handle not found.
 */
vos_instance_t *vos_instance_get(vos_handle_t handle);

/* ── Public API (vos_destroy.c) ──────────────────────────────────────────────── */

vos_err_t vos_destroy(vos_handle_t handle);

/* ── Public API (vos_snapshot.c) ────────────────────────────────────────────── */

vos_err_t vos_snapshot(vos_handle_t handle,
                        uint32_t *snap_lo_out, uint32_t *snap_hi_out);
void      vos_snapshot_init(void);
vos_err_t vos_test_alloc_snap(const void *data, uint32_t len,
                               uint32_t *idx_out);

/* ── Public API (vos_restore.c) ──────────────────────────────────────────────── */

vos_err_t vos_restore(uint32_t snap_lo, uint32_t snap_hi,
                       const vos_spec_t *spec, vos_handle_t *handle_out);
