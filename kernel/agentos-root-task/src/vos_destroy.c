/*
 * vos_destroy.c — VOS instance DESTROY operation
 *
 * Tears down a live guest OS instance and frees its slot in the instance table.
 * Under AGENTOS_TEST_HOST there are no real seL4 capabilities to revoke;
 * the slot is simply zeroed and marked free.
 *
 * Public API:
 *   vos_destroy(handle) — release the instance; returns VOS_ERR_INVALID_HANDLE
 *                          if the handle is not live.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdint.h>
#include <string.h>
#include "vos_types.h"

vos_err_t vos_destroy(vos_handle_t handle)
{
    vos_instance_t *inst = vos_instance_get(handle);
    if (!inst)
        return VOS_ERR_INVALID_HANDLE;

    /*
     * In production: revoke capabilities, stop vCPU(s), unmap memory.
     * Under AGENTOS_TEST_HOST: zero the slot to mark it free.
     */
    memset(inst, 0, sizeof(*inst));
    inst->handle = VOS_HANDLE_INVALID;

    return VOS_ERR_OK;
}
