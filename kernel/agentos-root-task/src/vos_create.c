/*
 * vos_create.c — VOS_CREATE stub for initial root-task boot
 *
 * Phase 1 stub: provides link-compatible implementations of vos_create_init,
 * vos_create, and vos_instance_get.  vos_create returns VOS_ERR_OUT_OF_MEMORY
 * until full ELF loading and VCPU allocation are implemented.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "vos_create.h"

#define VOS_ERR_OUT_OF_MEMORY  UINT32_C(2)

static vos_instance_t g_slots[VOS_MAX_INSTANCES];
static uint32_t       g_initialised;

void vos_create_init(cap_tree_t *tree,
                     seL4_CPtr   root_cnode,
                     seL4_CPtr   asid_pool_cap,
                     seL4_Word   free_slot_base)
{
    (void)tree;
    (void)root_cnode;
    (void)asid_pool_cap;
    (void)free_slot_base;

    for (uint32_t i = 0u; i < VOS_MAX_INSTANCES; i++) {
        g_slots[i].handle = VOS_HANDLE_INVALID;
        g_slots[i].state  = VOS_STATE_DESTROYED;
    }
    g_initialised = 1u;
}

vos_err_t vos_create(const vos_spec_t *spec, vos_handle_t *handle_out)
{
    (void)spec;
    if (handle_out) {
        *handle_out = VOS_HANDLE_INVALID;
    }
    return VOS_ERR_OUT_OF_MEMORY;
}

vos_instance_t *vos_instance_get(vos_handle_t h)
{
    if (h >= VOS_MAX_INSTANCES || !g_initialised) {
        return (vos_instance_t *)0;
    }
    if (g_slots[h].state == VOS_STATE_DESTROYED ||
        g_slots[h].handle == VOS_HANDLE_INVALID) {
        return (vos_instance_t *)0;
    }
    return &g_slots[h];
}
