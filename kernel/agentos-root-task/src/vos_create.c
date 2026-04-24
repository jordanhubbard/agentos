/*
 * vos_create.c — VOS instance table and CREATE / STATUS operations
 *
 * Implements the in-memory guest OS instance table used by the vibeOS lifecycle
 * API.  Under AGENTOS_TEST_HOST all seL4 / Microkit primitives are absent; the
 * table lives in static storage and the boot sequence is synchronous.
 *
 * Public functions (all callable from the lifecycle test):
 *   vos_create_init()         — reset instance table (call before each test)
 *   vos_create()              — allocate + initialise a new slot
 *   vos_instance_get()        — look up an existing slot by handle
 *   vos_instance_set_state()  — update state field of a live slot
 *   vos_get_status()          — populate vos_status_t for a live handle
 *   vos_test_alloc_instance() — test-only raw slot allocation
 *   vos_test_free_instance()  — test-only raw slot release
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "vos_types.h"

/* ── Instance table ──────────────────────────────────────────────────────────── */

static vos_instance_t g_instances[VOS_MAX_INSTANCES];
static uint32_t       g_next_handle = 1u;

/* ── Initialisation ──────────────────────────────────────────────────────────── */

void vos_create_init(void)
{
    uint32_t i;
    for (i = 0; i < VOS_MAX_INSTANCES; i++) {
        memset(&g_instances[i], 0, sizeof(g_instances[i]));
        g_instances[i].handle = VOS_HANDLE_INVALID;
    }
    g_next_handle = 1u;
}

/* ── Internal helpers ────────────────────────────────────────────────────────── */

static vos_instance_t *find_by_handle(vos_handle_t h)
{
    uint32_t i;
    if (h == VOS_HANDLE_INVALID) return NULL;
    for (i = 0; i < VOS_MAX_INSTANCES; i++)
        if (g_instances[i].handle == h) return &g_instances[i];
    return NULL;
}

static vos_instance_t *find_free_slot(void)
{
    uint32_t i;
    for (i = 0; i < VOS_MAX_INSTANCES; i++)
        if (g_instances[i].handle == VOS_HANDLE_INVALID) return &g_instances[i];
    return NULL;
}

/* ── Public: look up instance ────────────────────────────────────────────────── */

vos_instance_t *vos_instance_get(vos_handle_t h)
{
    return find_by_handle(h);
}

/* ── Public: CREATE ──────────────────────────────────────────────────────────── */

vos_err_t vos_create(const vos_spec_t *spec, vos_handle_t *handle_out)
{
    vos_instance_t *slot;
    vos_handle_t    h;

    if (!spec || !handle_out)
        return VOS_ERR_INTERNAL;

    if (spec->memory_pages < VOS_SPEC_MIN_PAGES ||
        spec->memory_pages > VOS_SPEC_MAX_PAGES)
        return VOS_ERR_INVALID_SPEC;

    if (spec->os_type != VOS_OS_LINUX  &&
        spec->os_type != VOS_OS_FREEBSD &&
        spec->os_type != VOS_OS_CUSTOM)
        return VOS_ERR_UNSUPPORTED_OS;

    slot = find_free_slot();
    if (!slot)
        return VOS_ERR_OUT_OF_MEMORY;

    h = g_next_handle++;
    if (g_next_handle == VOS_HANDLE_INVALID)
        g_next_handle = 1u;

    slot->handle        = h;
    slot->state         = VOS_STATE_RUNNING;  /* boot completes synchronously in stub */
    slot->os_type       = spec->os_type;
    slot->vcpu_count    = spec->vcpu_count ? spec->vcpu_count : 1u;
    slot->cpu_quota_pct = spec->cpu_quota_pct;
    slot->memory_pages  = spec->memory_pages;
    slot->cpu_affinity  = spec->cpu_affinity ? spec->cpu_affinity : 0xFFFFFFFFu;
    memset(slot->label, 0, sizeof(slot->label));
    memcpy(slot->label, spec->label, sizeof(slot->label));

    *handle_out = h;
    return VOS_ERR_OK;
}

/* ── Public: STATUS ──────────────────────────────────────────────────────────── */

vos_err_t vos_get_status(vos_handle_t handle, vos_status_t *status_out)
{
    vos_instance_t *inst;

    if (!status_out)
        return VOS_ERR_INTERNAL;

    inst = find_by_handle(handle);
    if (!inst)
        return VOS_ERR_INVALID_HANDLE;

    memset(status_out, 0, sizeof(*status_out));
    status_out->handle             = handle;
    status_out->state              = inst->state;
    status_out->os_type            = inst->os_type;
    status_out->cpu_quota_pct      = inst->cpu_quota_pct;
    status_out->vcpu_count         = inst->vcpu_count;
    status_out->memory_total_pages = inst->memory_pages;
    memcpy(status_out->label, inst->label, sizeof(status_out->label));
    return VOS_ERR_OK;
}

/* ── Public: set state ───────────────────────────────────────────────────────── */

vos_err_t vos_instance_set_state(vos_handle_t handle, vos_state_t state)
{
    vos_instance_t *inst = find_by_handle(handle);
    if (!inst) return VOS_ERR_INVALID_HANDLE;
    inst->state = state;
    return VOS_ERR_OK;
}

/* ── Test-only: raw slot management ─────────────────────────────────────────── */

vos_err_t vos_test_alloc_instance(vos_os_type_t os_type, vos_state_t state,
                                   vos_handle_t *handle_out)
{
    vos_instance_t *slot;
    vos_handle_t    h;

    slot = find_free_slot();
    if (!slot) return VOS_ERR_OUT_OF_MEMORY;

    h = g_next_handle++;
    if (g_next_handle == VOS_HANDLE_INVALID) g_next_handle = 1u;

    slot->handle        = h;
    slot->state         = state;
    slot->os_type       = os_type;
    slot->vcpu_count    = 1u;
    slot->cpu_quota_pct = 50u;
    slot->memory_pages  = VOS_SPEC_MIN_PAGES;
    slot->cpu_affinity  = 0xFFFFFFFFu;
    memset(slot->label, 0, sizeof(slot->label));

    if (handle_out) *handle_out = h;
    return VOS_ERR_OK;
}

vos_err_t vos_test_free_instance(vos_handle_t handle)
{
    vos_instance_t *inst = find_by_handle(handle);
    if (!inst) return VOS_ERR_INVALID_HANDLE;
    memset(inst, 0, sizeof(*inst));
    inst->handle = VOS_HANDLE_INVALID;
    return VOS_ERR_OK;
}
