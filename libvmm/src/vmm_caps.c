/*
 * vmm_caps.c — seL4-native VMM capability table storage
 *
 * Defines the global VCPU capability table and the PD name symbol
 * that VMM PDs must define themselves.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <libvmm/vmm_caps.h>

/* Global VCPU capability table — indexed by vcpu_id. */
vmm_vcpu_t g_vmm_vcpus[VMM_MAX_VCPUS];
