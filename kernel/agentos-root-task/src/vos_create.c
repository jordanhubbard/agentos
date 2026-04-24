/*
 * vos_create.c — VOS_CREATE capability delegation implementation
 *
 * Implements the allocation and cleanup paths for vibeOS guest OS instance
 * creation.  For each VOS_OP_CREATE call the following kernel objects are
 * allocated from the root task's untyped memory pool:
 *
 *   1. Guest CNode   (seL4_CapTableObject, size_bits=8 → 256 slots)
 *   2. VSpace        (seL4_ARM_VSpaceObject)
 *   3. VCPU          (seL4_ARM_VCPUObject)
 *   4. Notification  (seL4_NotificationObject)
 *   5. RAM frames    (seL4_ARM_SmallPageObject) × spec->memory_pages
 *
 * All allocated capabilities are recorded in the global cap_tree under a
 * per-guest subtree rooted at a node named "guest:<label>".
 *
 * Error handling:
 *   If any allocation fails we call vos_create_cleanup() which walks the
 *   allocation log backwards, calling seL4_CNode_Revoke + seL4_CNode_Delete
 *   on each capability before marking the slot unused.
 *
 * Host-test build (-DAGENTOS_TEST_HOST):
 *   ut_alloc(), seL4_CNode_Revoke(), and seL4_CNode_Delete() are replaced
 *   by stubs defined in the test file that includes this translation unit.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "vos_create.h"
#include "ut_alloc.h"

/* ── Additional seL4 object type constants not in sel4_boot.h ───────────── */
/*
 * These constants follow the seL4 AArch64 object type numbering.
 * The main worktree's sel4_boot.h is a minimal stub; we add what we need.
 */
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
/*
 * ARM hypervisor VCPU object.  Named seL4_ARM_VCPUObject in seL4 headers
 * from commit b43e08a onward.  Older trees call it seL4_VCPU (value 12).
 */
#ifndef seL4_ARM_VCPUObject
#  define seL4_ARM_VCPUObject      12u
#endif

/* ── seL4 CNode invocation stubs (host test build) ─────────────────────── */
#ifdef AGENTOS_TEST_HOST

/* Forward declarations — defined by the including test file */
extern seL4_Error stub_cnode_revoke(seL4_CPtr root, seL4_Word index, uint8_t depth);
extern seL4_Error stub_cnode_delete(seL4_CPtr root, seL4_Word index, uint8_t depth);
extern seL4_CPtr  stub_ut_alloc(seL4_Word obj_type, seL4_Word size_bits,
                                 seL4_CPtr dest_cnode, seL4_Word dest_index,
                                 seL4_Word dest_depth);

/* Map real names → stubs */
#define seL4_CNode_Revoke(root, idx, depth) \
    stub_cnode_revoke((root), (idx), (uint8_t)(depth))
#define seL4_CNode_Delete(root, idx, depth) \
    stub_cnode_delete((root), (idx), (uint8_t)(depth))

/* ut_alloc is provided by the test file as a stub function declared extern */
#undef  ut_alloc
#define ut_alloc(type, sbits, dcnode, didx, ddepth) \
    stub_ut_alloc((type), (sbits), (dcnode), (didx), (ddepth))

#else  /* real seL4 build */

/* seL4_CNode_Revoke / seL4_CNode_Delete are inline functions in sel4_boot.h */
/* ut_alloc is declared in ut_alloc.h */

#endif /* AGENTOS_TEST_HOST */

/* ── cap_tree stub (host test build) ────────────────────────────────────── */
#ifdef AGENTOS_TEST_HOST

/*
 * The test file must provide:
 *   extern uint32_t stub_cap_tree_insert(cap_tree_t *, uint32_t parent_idx,
 *                                        uint64_t cap, uint32_t obj_type,
 *                                        uint32_t pd_owner, const char *name);
 *   extern void     stub_cap_tree_remove(cap_tree_t *, uint32_t node_idx);
 */
extern uint32_t stub_cap_tree_insert(cap_tree_t *tree, uint32_t parent_idx,
                                      uint64_t cap, uint32_t obj_type,
                                      uint32_t pd_owner, const char *name);

#undef  cap_tree_insert
#define cap_tree_insert(tree, parent, cap, type, owner, name) \
    stub_cap_tree_insert((tree), (parent), (uint64_t)(cap), (type), (owner), (name))

#endif /* AGENTOS_TEST_HOST */

/* ── Module-level static state ──────────────────────────────────────────── */

/*
 * g_slots — the fixed-size array of guest instance slots.
 * Indexed by vos_handle_t (0..VOS_MAX_INSTANCES-1).
 */
static vos_instance_t g_slots[VOS_MAX_INSTANCES];

/* Global cap tree (owned by root task; pointer stored at init time) */
static cap_tree_t    *g_tree;

/* Root task's initial CNode capability */
static seL4_CPtr      g_root_cnode;

/* ASID pool cap (passed at init; used when mapping VSpaces) */
static seL4_CPtr      g_asid_pool_cap;

/*
 * g_free_slot — bump pointer for assigning CNode slot indices to new caps.
 * Each vos_create() call consumes:
 *   1 (CNode) + 1 (VSpace) + 1 (VCPU) + 1 (Notification) + memory_pages
 * slots, starting at g_free_slot and advancing atomically on success.
 * On cleanup the slots are deleted but the bump pointer is not wound back
 * (seL4_Untyped_Retype is not reversible; we simply leave the slots empty).
 */
static seL4_Word      g_free_slot;

/* ── String helpers (no libc) ───────────────────────────────────────────── */

static void str_copy_n(char *dst, const char *src, uint32_t n)
{
    uint32_t i;
    for (i = 0u; i + 1u < n && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

static void str_copy_cat(char *dst, const char *prefix, const char *src, uint32_t n)
{
    /* Write prefix first */
    uint32_t pi = 0u;
    while (pi + 1u < n && prefix[pi] != '\0') {
        dst[pi] = prefix[pi];
        pi++;
    }
    /* Append src */
    uint32_t si = 0u;
    while (pi + 1u < n && src[si] != '\0') {
        dst[pi++] = src[si++];
    }
    dst[pi] = '\0';
}

/* ── Allocation log — tracks caps in order of allocation ────────────────── */
/*
 * vos_create() allocates up to 4 + VOS_SPEC_MAX_PAGES caps.
 * We bound the log at 4 + VOS_SPEC_MAX_PAGES; the frame loop writes
 * entries 4..3+memory_pages.
 *
 * MAX_ALLOC_LOG = 4 (CNode, VSpace, VCPU, Ntfn) + VOS_SPEC_MAX_PAGES frames.
 * That would be huge if fully pre-allocated.  Instead we store a compact
 * descriptor for each allocation so cleanup can regenerate the slot values.
 */

/*
 * alloc_log_entry_t — records one seL4_Untyped_Retype result for cleanup.
 *
 * On failure we walk backwards through this log, calling Revoke + Delete.
 */
typedef struct {
    seL4_CPtr cap;      /* slot index returned by ut_alloc() */
    uint8_t   valid;    /* 1 if this entry was successfully allocated */
} alloc_log_entry_t;

/*
 * VOS_ALLOC_FIXED — number of fixed kernel objects per instance (not frames).
 *   0: guest CNode
 *   1: VSpace
 *   2: VCPU
 *   3: Notification
 */
#define VOS_ALLOC_FIXED  4u

/*
 * Maximum frame allocations per instance.  Frame cleanup uses a separate
 * compact representation — we record only the base slot and count.
 */

/* ── Internal cleanup ───────────────────────────────────────────────────── */

/*
 * vos_create_cleanup — revoke and delete all caps in the allocation log.
 *
 * Called on the error path in vos_create().  Walks the log of successfully
 * allocated caps in reverse order, calling seL4_CNode_Revoke then
 * seL4_CNode_Delete on each.
 *
 * Also cleans up frame caps using the base slot + count approach.
 *
 * Note: seL4_Untyped_Retype is not reversible; we cannot return the
 * untyped memory.  We simply mark the slot as freed in the cap_tree
 * and leave the bump pointer advanced (the slot stays empty in seL4).
 */
static void vos_create_cleanup(const alloc_log_entry_t *log,
                                 uint32_t log_count,
                                 seL4_CPtr frame_base_slot,
                                 uint32_t  frames_allocated)
{
    int32_t i;

    /* Clean up frame caps first (highest slots — allocated last) */
    for (i = (int32_t)frames_allocated - 1; i >= 0; i--) {
        seL4_CPtr cap = frame_base_slot + (seL4_Word)i;
        seL4_CNode_Revoke(g_root_cnode, cap, 64u);
        seL4_CNode_Delete(g_root_cnode, cap, 64u);
    }

    /* Clean up fixed objects in reverse order */
    for (i = (int32_t)log_count - 1; i >= 0; i--) {
        if (log[i].valid) {
            seL4_CNode_Revoke(g_root_cnode, log[i].cap, 64u);
            seL4_CNode_Delete(g_root_cnode, log[i].cap, 64u);
        }
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void vos_create_init(cap_tree_t *tree,
                     seL4_CPtr   root_cnode,
                     seL4_CPtr   asid_pool_cap,
                     seL4_Word   free_slot_base)
{
    uint32_t i;

    g_tree          = tree;
    g_root_cnode    = root_cnode;
    g_asid_pool_cap = asid_pool_cap;
    g_free_slot     = free_slot_base;

    /* Mark all slots as unused */
    for (i = 0u; i < VOS_MAX_INSTANCES; i++) {
        g_slots[i].handle           = VOS_HANDLE_INVALID;
        g_slots[i].state            = VOS_STATE_DESTROYED;
        g_slots[i].os_type          = VOS_OS_LINUX;
        g_slots[i].vcpu_count       = 0u;
        g_slots[i].memory_pages     = 0u;
        g_slots[i].cap_subtree_root = CAP_NODE_NONE;
        g_slots[i].vcpu_cap         = seL4_CapNull;
        g_slots[i].vspace_cap       = seL4_CapNull;
        g_slots[i].guest_cnode_cap  = seL4_CapNull;
        g_slots[i].ntfn_cap         = seL4_CapNull;
        g_slots[i].label[0]         = '\0';
    }
}

vos_err_t vos_create(const vos_spec_t *spec, vos_handle_t *handle_out)
{
    uint32_t           slot;
    uint32_t           found = VOS_MAX_INSTANCES;
    seL4_CPtr          cnode_cap, vspace_cap, vcpu_cap, ntfn_cap;
    seL4_Word          frame_base;
    uint32_t           frames_ok;
    alloc_log_entry_t  log[VOS_ALLOC_FIXED];
    uint32_t           log_count = 0u;
    uint32_t           subtree_root;
    char               node_name[64];

    if (!spec || !handle_out) {
        return VOS_ERR_INTERNAL;
    }

    /* ── Validate spec ───────────────────────────────────────────────────── */

    if (spec->memory_pages < VOS_SPEC_MIN_PAGES ||
        spec->memory_pages > VOS_SPEC_MAX_PAGES) {
        return VOS_ERR_INVALID_SPEC;
    }

    /* Validate os_type — only known enum values are accepted */
    switch ((uint32_t)spec->os_type) {
    case (uint32_t)VOS_OS_LINUX:
    case (uint32_t)VOS_OS_FREEBSD:
    case (uint32_t)VOS_OS_CUSTOM:
        break;
    default:
        return VOS_ERR_INVALID_SPEC;
    }

    /* ── Find a free instance slot ───────────────────────────────────────── */

    for (slot = 0u; slot < VOS_MAX_INSTANCES; slot++) {
        if (g_slots[slot].state == VOS_STATE_DESTROYED &&
            g_slots[slot].handle == VOS_HANDLE_INVALID) {
            found = slot;
            break;
        }
    }

    if (found == VOS_MAX_INSTANCES) {
        return VOS_ERR_OUT_OF_MEMORY;
    }

    /* ── Assign CNode slots for this instance's caps ─────────────────────── */

    /*
     * We carve four consecutive CNode slots from the bump pointer:
     *   g_free_slot + 0 → guest CNode itself
     *   g_free_slot + 1 → VSpace
     *   g_free_slot + 2 → VCPU
     *   g_free_slot + 3 → Notification
     *   g_free_slot + 4 .. g_free_slot + 3 + memory_pages → RAM frames
     */
    seL4_Word base          = g_free_slot;
    seL4_Word slot_cnode    = base + 0u;
    seL4_Word slot_vspace   = base + 1u;
    seL4_Word slot_vcpu     = base + 2u;
    seL4_Word slot_ntfn     = base + 3u;
    frame_base              = base + 4u;

    /* Advance the bump pointer past all slots we intend to use */
    g_free_slot = frame_base + (seL4_Word)spec->memory_pages;

    /* Initialise log */
    for (uint32_t i = 0u; i < VOS_ALLOC_FIXED; i++) {
        log[i].cap   = seL4_CapNull;
        log[i].valid = 0u;
    }

    /* ── Allocate guest CNode ────────────────────────────────────────────── */

    cnode_cap = ut_alloc(seL4_CapTableObject,
                         GUEST_CNODE_SIZE_BITS,
                         g_root_cnode,
                         slot_cnode,
                         64u);
    if (cnode_cap == seL4_CapNull) {
        vos_create_cleanup(log, log_count, frame_base, 0u);
        g_free_slot = base; /* best-effort: rewind bump pointer */
        return VOS_ERR_OUT_OF_MEMORY;
    }
    log[log_count].cap   = cnode_cap;
    log[log_count].valid = 1u;
    log_count++;

    /* ── Allocate VSpace ─────────────────────────────────────────────────── */

    vspace_cap = ut_alloc(seL4_ARM_VSpaceObject,
                          0u,
                          g_root_cnode,
                          slot_vspace,
                          64u);
    if (vspace_cap == seL4_CapNull) {
        vos_create_cleanup(log, log_count, frame_base, 0u);
        g_free_slot = base;
        return VOS_ERR_OUT_OF_MEMORY;
    }
    log[log_count].cap   = vspace_cap;
    log[log_count].valid = 1u;
    log_count++;

    /* ── Allocate VCPU ───────────────────────────────────────────────────── */

    vcpu_cap = ut_alloc(seL4_ARM_VCPUObject,
                        0u,
                        g_root_cnode,
                        slot_vcpu,
                        64u);
    if (vcpu_cap == seL4_CapNull) {
        vos_create_cleanup(log, log_count, frame_base, 0u);
        g_free_slot = base;
        return VOS_ERR_OUT_OF_MEMORY;
    }
    log[log_count].cap   = vcpu_cap;
    log[log_count].valid = 1u;
    log_count++;

    /* ── Allocate Notification ───────────────────────────────────────────── */

    ntfn_cap = ut_alloc(seL4_NotificationObject,
                        0u,
                        g_root_cnode,
                        slot_ntfn,
                        64u);
    if (ntfn_cap == seL4_CapNull) {
        vos_create_cleanup(log, log_count, frame_base, 0u);
        g_free_slot = base;
        return VOS_ERR_OUT_OF_MEMORY;
    }
    log[log_count].cap   = ntfn_cap;
    log[log_count].valid = 1u;
    log_count++;

    /* ── Allocate RAM frames ─────────────────────────────────────────────── */

    frames_ok = 0u;
    for (uint32_t f = 0u; f < spec->memory_pages; f++) {
        seL4_CPtr frame = ut_alloc(seL4_ARM_SmallPageObject,
                                    0u,
                                    g_root_cnode,
                                    frame_base + (seL4_Word)f,
                                    64u);
        if (frame == seL4_CapNull) {
            vos_create_cleanup(log, log_count, frame_base, frames_ok);
            g_free_slot = base;
            return VOS_ERR_OUT_OF_MEMORY;
        }
        frames_ok++;
    }

    /* ── Record capabilities in the cap_tree ─────────────────────────────── */

    /*
     * Create a subtree root node named "guest:<label>".
     * All caps for this instance are children of this root node.
     * pd_owner is set to the slot index (used as guest PD index).
     */
    str_copy_cat(node_name, "guest:", spec->label, sizeof(node_name));

    subtree_root = cap_tree_insert(g_tree,
                                    CAP_NODE_NONE,
                                    (uint64_t)cnode_cap,
                                    (uint32_t)seL4_CapTableObject,
                                    (uint32_t)found,
                                    node_name);

    if (subtree_root == CAP_NODE_NONE) {
        /* Cap tree exhausted — clean up and fail */
        vos_create_cleanup(log, log_count, frame_base, frames_ok);
        g_free_slot = base;
        return VOS_ERR_OUT_OF_MEMORY;
    }

    /* Insert VSpace as child of subtree root */
    cap_tree_insert(g_tree, subtree_root,
                    (uint64_t)vspace_cap,
                    (uint32_t)seL4_ARM_VSpaceObject,
                    (uint32_t)found,
                    "vspace");

    /* Insert VCPU as child of subtree root */
    cap_tree_insert(g_tree, subtree_root,
                    (uint64_t)vcpu_cap,
                    (uint32_t)seL4_ARM_VCPUObject,
                    (uint32_t)found,
                    "vcpu");

    /* Insert Notification as child of subtree root */
    cap_tree_insert(g_tree, subtree_root,
                    (uint64_t)ntfn_cap,
                    (uint32_t)seL4_NotificationObject,
                    (uint32_t)found,
                    "ntfn");

    /* Insert RAM frame caps as children of subtree root */
    for (uint32_t f = 0u; f < spec->memory_pages; f++) {
        cap_tree_insert(g_tree, subtree_root,
                        (uint64_t)(frame_base + (seL4_Word)f),
                        (uint32_t)seL4_ARM_SmallPageObject,
                        (uint32_t)found,
                        "frame");
    }

    /* ── Populate the instance slot ──────────────────────────────────────── */

    g_slots[found].handle           = (vos_handle_t)found;
    g_slots[found].state            = VOS_STATE_CREATING;
    g_slots[found].os_type          = spec->os_type;
    g_slots[found].vcpu_count       = spec->vcpu_count;
    g_slots[found].memory_pages     = spec->memory_pages;
    g_slots[found].cap_subtree_root = subtree_root;
    g_slots[found].vcpu_cap         = vcpu_cap;
    g_slots[found].vspace_cap       = vspace_cap;
    g_slots[found].guest_cnode_cap  = cnode_cap;
    g_slots[found].ntfn_cap         = ntfn_cap;
    str_copy_n(g_slots[found].label, spec->label, sizeof(g_slots[found].label));

    *handle_out = (vos_handle_t)found;
    return VOS_ERR_OK;
}

vos_instance_t *vos_instance_get(vos_handle_t h)
{
    if (h == VOS_HANDLE_INVALID || h >= (vos_handle_t)VOS_MAX_INSTANCES) {
        return (vos_instance_t *)0;
    }

    if (g_slots[h].state == VOS_STATE_DESTROYED ||
        g_slots[h].handle == VOS_HANDLE_INVALID) {
        return (vos_instance_t *)0;
    }

    return &g_slots[h];
}
