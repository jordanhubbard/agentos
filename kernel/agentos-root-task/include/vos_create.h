/*
 * vos_create.h — VOS_CREATE capability delegation subsystem
 *
 * Implements the allocation side of the vibeOS instance lifecycle:
 * given a vos_spec_t, carve out a bounded, non-escalatable capability
 * set (CNode, VSpace, VCPU, Notification, RAM frames) and return a
 * vos_handle_t that opaquely identifies the new guest instance.
 *
 * This module is called exclusively from the vibeOS IPC dispatch path
 * in the root task's main event loop.  It must not block.
 *
 * All storage is static — no heap, no stdlib allocators.
 *
 * Version: 1
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "sel4_boot.h"
#include "cap_tree.h"
#include "../../contracts/vibeos/interface.h"

/* ── Capability layout constants ─────────────────────────────────────────── */

/*
 * Guest CNode slot assignments (within each guest's private 256-slot CNode).
 *
 *   Slot 0                  — VSpace cap copy
 *   Slot 1                  — VCPU cap copy
 *   Slot 2                  — Notification cap copy
 *   Slots 3..2+memory_pages — RAM frame caps (one per 4 KiB page)
 *
 * The guest CNode has size_bits = 8 → 256 slots.
 */
#define GUEST_CNODE_SIZE_BITS    8u            /* 2^8 = 256 slots */
#define GUEST_CNODE_SLOT_VSPACE  0u
#define GUEST_CNODE_SLOT_VCPU    1u
#define GUEST_CNODE_SLOT_NTFN    2u
#define GUEST_CNODE_SLOT_FRAMES  3u            /* first frame slot */

/* ── vos_instance_t — one live guest instance ────────────────────────────── */

/*
 * An opaque per-instance record maintained by this subsystem.
 * One slot in g_slots[VOS_MAX_INSTANCES]; unused slots have
 * state == VOS_STATE_DESTROYED and handle == VOS_HANDLE_INVALID.
 *
 * Fields:
 *   handle           — the vos_handle_t returned to the caller (== slot index)
 *   state            — current lifecycle state
 *   os_type          — guest OS type from vos_spec_t
 *   vcpu_count       — vCPU count from vos_spec_t (currently always 1)
 *   memory_pages     — number of 4 KiB RAM frames allocated
 *   cap_subtree_root — index in g_cap_tree of the root node for this guest
 *   vcpu_cap         — seL4 CPtr of the VCPU capability in root CSpace
 *   vspace_cap       — seL4 CPtr of the VSpace capability in root CSpace
 *   guest_cnode_cap  — seL4 CPtr of the guest CNode capability in root CSpace
 *   ntfn_cap         — seL4 CPtr of the Notification capability in root CSpace
 *   label            — NUL-terminated label (from vos_spec_t, max 15 chars)
 */
typedef struct {
    vos_handle_t    handle;
    vos_state_t     state;
    vos_os_type_t   os_type;
    uint8_t         vcpu_count;
    uint32_t        memory_pages;
    uint32_t        cap_subtree_root;   /* index in g_cap_tree */
    seL4_CPtr       vcpu_cap;
    seL4_CPtr       vspace_cap;
    seL4_CPtr       guest_cnode_cap;
    seL4_CPtr       ntfn_cap;
    char            label[16];
} vos_instance_t;

_Static_assert(sizeof(vos_instance_t) >= sizeof(vos_handle_t),
               "vos_instance_t must contain at least a handle field");

/* ── Subsystem API ───────────────────────────────────────────────────────── */

/*
 * vos_create_init — initialise the VOS_CREATE subsystem.
 *
 * Called once during root task boot, before any vos_create() call.
 *
 * Parameters:
 *   tree           — pointer to the global cap_tree_t managed by the root task
 *   root_cnode     — seL4 CPtr of the root task's initial CNode
 *   asid_pool_cap  — seL4 CPtr of the ASID pool to assign to new VSpaces
 *   free_slot_base — first free CNode slot available for dynamic allocation
 *
 * All slots in g_slots[] are marked invalid.  The module stores the
 * parameters for use in subsequent vos_create() calls.
 */
void vos_create_init(cap_tree_t *tree,
                     seL4_CPtr   root_cnode,
                     seL4_CPtr   asid_pool_cap,
                     seL4_Word   free_slot_base);

/*
 * vos_create — allocate capabilities for a new guest OS instance.
 *
 * Validates spec, finds a free slot, retypes kernel objects from untyped
 * memory, records every capability in the cap_tree under a new per-guest
 * subtree, and writes the new instance handle into *handle_out.
 *
 * On any failure the function revokes and deletes all capabilities allocated
 * so far (best-effort cleanup) before returning the error code.  No partial
 * state is left in g_slots[].
 *
 * Parameters:
 *   spec        — caller-provided specification (must not be NULL)
 *   handle_out  — receives the new vos_handle_t on success (must not be NULL)
 *
 * Returns:
 *   VOS_ERR_OK            — success; *handle_out is valid
 *   VOS_ERR_INVALID_SPEC  — spec field out of range or os_type unknown
 *   VOS_ERR_OUT_OF_MEMORY — no free instance slot or ut_alloc() failed
 */
vos_err_t vos_create(const vos_spec_t *spec, vos_handle_t *handle_out);

/*
 * vos_instance_get — look up a live instance by handle.
 *
 * Returns a pointer to the vos_instance_t for handle h, or NULL if:
 *   - h >= VOS_MAX_INSTANCES
 *   - h == VOS_HANDLE_INVALID
 *   - the slot's state is VOS_STATE_DESTROYED
 *
 * The pointer is into static storage; the caller must not free() it.
 */
vos_instance_t *vos_instance_get(vos_handle_t h);
