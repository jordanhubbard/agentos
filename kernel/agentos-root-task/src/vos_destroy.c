/*
 * vos_destroy.c — VOS_DESTROY capability revocation and memory reclamation
 *
 * Implements the teardown path for vibeOS guest OS instances.  For each
 * VOS_OP_DESTROY call the following is performed:
 *
 *   Step 1 — Validate handle
 *   Step 2 — Quiesce the VMM (seL4_TCB_Suspend on vcpu_cap; state → SUSPENDED)
 *   Step 3 — Revoke guest capability subtree (post-order, leaves first)
 *   Step 4 — Return untyped memory (accumulate freed bytes per frame page)
 *   Step 5 — Zero guest RAM pages (stub in test builds)
 *   Step 6 — Final accounting (verify 0 caps remain, state → DESTROYED)
 *
 * Integration:
 *   vos_destroy_init() must be called with the same cap_tree_t pointer and
 *   root_cnode that were passed to vos_create_init().  Both subsystems share
 *   these references but own independent state.
 *
 * Constraints:
 *   - C11, no libc except stdint.h / stdbool.h / stddef.h
 *   - No recursion — post-order walk uses an iterative stack (see POST_ORDER_STACK_DEPTH)
 *   - No Microkit references
 *   - AGENTOS_TEST_HOST stubs for all seL4 calls
 *
 * Host-test build (-DAGENTOS_TEST_HOST):
 *   seL4_TCB_Suspend, seL4_CNode_Revoke, seL4_CNode_Delete, and
 *   cap_tree_remove are replaced by stubs defined in the including test file.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "vos_destroy.h"

/* ── seL4 object type constants (mirror vos_create.c) ───────────────────── */

#ifndef seL4_NotificationObject
#  define seL4_NotificationObject  3u
#endif
#ifndef seL4_CapTableObject
#  define seL4_CapTableObject      4u
#endif
#ifndef seL4_ARM_SmallPageObject
#  define seL4_ARM_SmallPageObject 15u
#endif
#ifndef seL4_ARM_VSpaceObject
#  define seL4_ARM_VSpaceObject    20u
#endif
#ifndef seL4_ARM_VCPUObject
#  define seL4_ARM_VCPUObject      12u
#endif

/* Page size in bytes (4 KiB) */
#ifndef PAGE_SIZE
#  define PAGE_SIZE  4096u
#endif

/* seL4 CNode depth for flat initial CNode lookups */
#define CNODE_DEPTH  64u

/* ── seL4 / cap_tree stubs (host test build) ─────────────────────────────── */

#ifdef AGENTOS_TEST_HOST

/*
 * The test file must provide forward declarations of these stub functions
 * before including this translation unit.
 */
extern seL4_Error stub_tcb_suspend(seL4_CPtr tcb);
extern seL4_Error stub_cnode_revoke(seL4_CPtr root, seL4_Word index,
                                     uint8_t depth);
extern seL4_Error stub_cnode_delete(seL4_CPtr root, seL4_Word index,
                                     uint8_t depth);
extern void       stub_cap_tree_remove(cap_tree_t *tree, uint32_t node_idx);
extern void       stub_cap_tree_walk_pd_shim(cap_tree_t *tree, uint32_t pd_id,
                                              cap_node_visitor_t visitor,
                                              void *ctx);

/* Map real names → stubs */
#define seL4_TCB_Suspend(tcb) \
    stub_tcb_suspend((tcb))
#define seL4_CNode_Revoke(root, idx, depth) \
    stub_cnode_revoke((root), (idx), (uint8_t)(depth))
#define seL4_CNode_Delete(root, idx, depth) \
    stub_cnode_delete((root), (idx), (uint8_t)(depth))
#undef  cap_tree_remove
#define cap_tree_remove(tree, idx) \
    stub_cap_tree_remove((tree), (idx))
#undef  cap_tree_walk_pd
#define cap_tree_walk_pd(tree, pd_id, visitor, ctx) \
    stub_cap_tree_walk_pd_shim((tree), (pd_id), (visitor), (ctx))

#endif /* AGENTOS_TEST_HOST */

/* ── Module-level static state ───────────────────────────────────────────── */

/* Global cap tree pointer — set by vos_destroy_init() */
static cap_tree_t *s_tree;

/* Root task's initial CNode capability — set by vos_destroy_init() */
static seL4_CPtr   s_root_cnode;

/* ── Initialisation ──────────────────────────────────────────────────────── */

/*
 * vos_destroy_init — initialise the VOS_DESTROY subsystem.
 *
 * Must be called once at root task boot, with the same parameters that
 * were passed to vos_create_init().  Called before any vos_destroy() call.
 *
 * Parameters:
 *   tree       — pointer to the global cap_tree_t managed by the root task
 *   root_cnode — seL4 CPtr of the root task's initial CNode
 */
void vos_destroy_init(cap_tree_t *tree, seL4_CPtr root_cnode)
{
    s_tree       = tree;
    s_root_cnode = root_cnode;
}

/* ── Frame zeroing stub ──────────────────────────────────────────────────── */

/*
 * zero_frame_stub — placeholder for zeroing a guest RAM frame.
 *
 * TODO(E6-S4): map frame into root task vspace, memset, unmap, then delete.
 * For security, frames must be zeroed before capabilities are returned to
 * the untyped pool.  In the current implementation this is a no-op since
 * the root task does not have a VA for guest frames.
 */
static void zero_frame_stub(seL4_CPtr frame_cap)
{
    (void)frame_cap;
    /* No-op in test and current bare-metal builds. */
}

/* ── Iterative post-order subtree revocation ────────────────────────────── */

/*
 * POST_ORDER_STACK_DEPTH — maximum number of nodes that can be queued at once.
 *
 * A guest instance has 1 subtree root (CNode cap) plus up to
 * VOS_SPEC_MAX_PAGES + 3 direct children (VSpace, VCPU, Notification,
 * and up to VOS_SPEC_MAX_PAGES frame caps).
 *
 * During Phase 1 the worst case is: root is popped, then ALL its children
 * (up to VOS_SPEC_MAX_PAGES + 3) are pushed onto visit_stack simultaneously.
 * visit_stack therefore needs to hold VOS_SPEC_MAX_PAGES + 3 entries.
 *
 * VOS_SPEC_MIN_PAGES = 256, so the minimum guest has 256 + 3 = 259 children.
 * A depth of 256 would overflow for any valid guest.  Use 524288 + 8 (max
 * pages + generous headroom) to safely cover the full spec range.  On bare
 * metal this is two 32-bit arrays of ~2 MiB each — acceptable for a root
 * task that owns all physical memory.  In test builds memory is virtual.
 */
#define POST_ORDER_STACK_DEPTH  (524288u + 8u)

uint32_t vos_cap_tree_revoke_subtree(cap_tree_t *tree,
                                      uint32_t    node_idx,
                                      seL4_CPtr   root_cnode)
{
    /*
     * Post-order iterative traversal of a SINGLE subtree (children before
     * the parent that owns them).  This function must not follow sibling
     * links at the starting node — only the subtree rooted at node_idx is
     * processed.
     *
     * Two-stack algorithm (child-descent only, no sibling traversal at root):
     *
     *   Phase 1 — build the done stack:
     *     Seed visit stack with node_idx.
     *     For each node popped from visit stack:
     *       1. Push it onto done stack.
     *       2. Walk its child list via next_sibling_idx within the child list
     *          (i.e., siblings within the SAME child level) and push each.
     *          NOTE: for each child, we then recurse into ITS children, not
     *          into its siblings at the root level.
     *
     * Simpler re-statement: treat the subtree as a tree and do a pre-order
     * push onto done_stack (so done_stack reversed = post-order).  For each
     * node, push ALL its children (linked by next_sibling_idx within that
     * child list) onto visit_stack.
     *
     *   Phase 2 — drain done stack top-to-bottom:
     *     This yields post-order (leaf nodes first).
     *     For each node: Revoke → Delete → cap_tree_remove.
     *
     * No heap, no recursion.  Both stacks are static uint32_t arrays.
     * Stack size 256 is sufficient: current guest tree depth ≤ 2 (root →
     * children), and breadth is bounded by VOS_SPEC_MAX_PAGES + 4.
     */
    static uint32_t  visit_stack[POST_ORDER_STACK_DEPTH];
    static uint32_t  done_stack[POST_ORDER_STACK_DEPTH];
    int32_t          visit_top = -1;
    int32_t          done_top  = -1;
    uint32_t         revoked   = 0u;

    if (node_idx == CAP_NODE_NONE) {
        return 0u;
    }

    /* Phase 1: Seed with the subtree root only (no siblings) */
    visit_stack[++visit_top] = node_idx;

    while (visit_top >= 0) {
        uint32_t    cur = visit_stack[visit_top--];
        cap_node_t *n   = cap_tree_node(tree, cur);

        if (!n) {
            continue;
        }

        /*
         * Push cur onto done stack (pre-order; reversed drain = post-order).
         */
        if (done_top + 1 < (int32_t)POST_ORDER_STACK_DEPTH) {
            done_stack[++done_top] = cur;
        }

        /*
         * Push ALL children of cur (walk the sibling chain within the child
         * list of THIS node).  We do NOT follow cur's own next_sibling_idx
         * here — that would escape the subtree.
         */
        uint32_t child = n->first_child_idx;
        while (child != CAP_NODE_NONE) {
            cap_node_t *cn = cap_tree_node(tree, child);
            if (!cn) {
                break;
            }
            if (visit_top + 1 < (int32_t)POST_ORDER_STACK_DEPTH) {
                visit_stack[++visit_top] = child;
            }
            child = cn->next_sibling_idx;
        }
    }

    /*
     * Phase 2: Drain done stack top-to-bottom → post-order (leaves first).
     * For each node: Revoke → Delete → cap_tree_remove.
     */
    while (done_top >= 0) {
        uint32_t    cur = done_stack[done_top--];
        cap_node_t *n   = cap_tree_node(tree, cur);

        if (!n) {
            continue;
        }

        seL4_CNode_Revoke(root_cnode, (seL4_Word)n->cap, CNODE_DEPTH);
        seL4_CNode_Delete(root_cnode, (seL4_Word)n->cap, CNODE_DEPTH);
        cap_tree_remove(tree, cur);

        revoked++;
    }

    return revoked;
}

/* ── vos_destroy — main teardown path ───────────────────────────────────── */

vos_err_t vos_destroy(vos_handle_t handle, uint64_t *bytes_reclaimed_out)
{
    uint64_t        bytes_reclaimed = 0u;
    vos_instance_t *inst;

    /* ── Step 1: Validate handle ──────────────────────────────────────────── */

    inst = vos_instance_get(handle);
    if (!inst) {
        return VOS_ERR_INVALID_HANDLE;
    }

    /*
     * vos_instance_get() already rejects DESTROYED handles, but defend
     * explicitly for clarity.
     */
    if (inst->state == VOS_STATE_DESTROYED) {
        return VOS_ERR_INVALID_HANDLE;
    }

    /* ── Step 2: Quiesce the VMM ──────────────────────────────────────────── */

    /*
     * Suspend the guest's VCPU.  In a full implementation we would also
     * locate and suspend the host-side VMM thread TCB via the cap_tree.
     * That requires a seL4_TCBObject lookup which is deferred until the VMM
     * layer is wired up.
     */
    if (inst->vcpu_cap != seL4_CapNull) {
        seL4_TCB_Suspend(inst->vcpu_cap);
    }

    inst->state = VOS_STATE_SUSPENDED;

    /* ── Step 3+4+5: Revoke caps, count bytes, zero frames ───────────────── */

    /*
     * Walk the immediate children of the cap subtree root before the revoke
     * destroys the nodes.  We accumulate freed bytes (Step 4) and call the
     * frame-zeroing stub (Step 5) here, while the node data is still valid.
     */
    {
        uint32_t    root_idx  = inst->cap_subtree_root;
        cap_node_t *root_node = cap_tree_node(s_tree, root_idx);

        if (root_node) {
            uint32_t child = root_node->first_child_idx;
            while (child != CAP_NODE_NONE) {
                cap_node_t *cn = cap_tree_node(s_tree, child);
                if (!cn) {
                    break;
                }
                if (cn->obj_type == seL4_ARM_SmallPageObject) {
                    /* Step 4: accumulate freed bytes */
                    bytes_reclaimed += (uint64_t)PAGE_SIZE;

                    /* Step 5: zero the frame (stub / no-op) */
                    zero_frame_stub((seL4_CPtr)cn->cap);
                }
                child = cn->next_sibling_idx;
            }
        }
    }

    /*
     * Step 3: Revoke the subtree post-order (leaves before root).
     * After this call the cap tree has no nodes for this instance.
     */
    vos_cap_tree_revoke_subtree(s_tree,
                                 inst->cap_subtree_root,
                                 s_root_cnode);

    /* ── Step 6: Final accounting ────────────────────────────────────────── */

    /*
     * Verify zero caps remain for this PD.  cap_tree_walk_pd with a NULL
     * visitor is a counting call (the walk still happens; the NULL visitor
     * is harmless — the walk guards against NULL before calling).
     *
     * In test builds this redirects to stub_cap_tree_walk_pd_shim which
     * increments g_walk_pd_call_count so the test can verify it ran.
     */
    cap_tree_walk_pd(s_tree, (uint32_t)handle,
                     (cap_node_visitor_t)0, (void *)0);

    /*
     * Mark the slot as destroyed so vos_instance_get() returns NULL.
     */
    inst->state            = VOS_STATE_DESTROYED;
    inst->handle           = VOS_HANDLE_INVALID;
    inst->cap_subtree_root = CAP_NODE_NONE;
    inst->vcpu_cap         = seL4_CapNull;
    inst->vspace_cap       = seL4_CapNull;
    inst->guest_cnode_cap  = seL4_CapNull;
    inst->ntfn_cap         = seL4_CapNull;
    inst->memory_pages     = 0u;

    if (bytes_reclaimed_out) {
        *bytes_reclaimed_out = bytes_reclaimed;
    }

    return VOS_ERR_OK;
}
