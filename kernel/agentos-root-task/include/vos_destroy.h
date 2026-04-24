/*
 * vos_destroy.h — VOS_DESTROY capability revocation and memory reclamation
 *
 * Implements the teardown side of the vibeOS instance lifecycle:
 * given a vos_handle_t, quiesce the VMM thread, revoke all guest capabilities
 * via seL4's capability tree, return untyped memory, and zero guest RAM pages.
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

#include "vos_create.h"   /* vos_instance_t, vos_handle_t, vos_instance_get() */
#include "cap_tree.h"     /* cap_tree_t, cap_node_t, cap_tree_node()           */

/* ── Public API ──────────────────────────────────────────────────────────── */

/*
 * vos_destroy_init — initialise the VOS_DESTROY subsystem.
 *
 * Must be called once at root task boot, before any vos_destroy() call,
 * with the same cap_tree_t pointer and root_cnode that were passed to
 * vos_create_init().
 *
 * Parameters:
 *   tree       — pointer to the global cap_tree_t managed by the root task
 *   root_cnode — seL4 CPtr of the root task's initial CNode
 */
void vos_destroy_init(cap_tree_t *tree, seL4_CPtr root_cnode);

/*
 * vos_destroy — tear down a guest instance identified by handle.
 *
 * Performs the following steps in strict order:
 *   1. Validate handle — VOS_ERR_INVALID_HANDLE if not live or DESTROYED
 *   2. Quiesce the VMM — seL4_TCB_Suspend on the instance's VCPU (if any)
 *      and set state = VOS_STATE_SUSPENDED
 *   3. Revoke the guest capability subtree — post-order walk, leaves first;
 *      seL4_CNode_Revoke + seL4_CNode_Delete + cap_tree_remove per node
 *   4. Return untyped memory — accumulate freed bytes for each frame cap
 *   5. Zero guest RAM pages — calls zero_frame_stub() for each frame (no-op
 *      in test builds; real hardware would memset the mapped vaddr)
 *   6. Final accounting — verify zero caps remain, set state = DESTROYED
 *
 * Parameters:
 *   handle              — identifies the instance to tear down
 *   bytes_reclaimed_out — receives the number of bytes freed (may be NULL)
 *
 * Returns:
 *   VOS_ERR_OK            — success; handle is now invalid
 *   VOS_ERR_INVALID_HANDLE — handle does not identify a live instance
 */
vos_err_t vos_destroy(vos_handle_t handle, uint64_t *bytes_reclaimed_out);

/*
 * vos_cap_tree_revoke_subtree — revoke+delete all caps rooted at node_idx.
 *
 * Performs an iterative post-order (leaves-first) walk of the subtree rooted
 * at node_idx.  For each node:
 *   1. seL4_CNode_Revoke(root_cnode, node->cap, 64)
 *   2. seL4_CNode_Delete(root_cnode, node->cap, 64)
 *   3. cap_tree_remove(tree, node_idx)
 *
 * Uses a static 256-entry stack — no recursion, safe on bare metal.
 * The walk terminates when the stack is empty; nodes with > 256 descendants
 * in a single path are handled by multiple re-entry passes (the caller must
 * verify the tree is empty afterward with cap_tree_walk_pd).
 *
 * Parameters:
 *   tree        — the global cap tree
 *   node_idx    — root of the subtree to revoke (CAP_NODE_NONE is a no-op)
 *   root_cnode  — the root task's initial CNode capability
 *
 * Returns:
 *   The number of caps revoked (= number of nodes processed).
 */
uint32_t vos_cap_tree_revoke_subtree(cap_tree_t *tree,
                                      uint32_t    node_idx,
                                      seL4_CPtr   root_cnode);
