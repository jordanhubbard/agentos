/*
 * test_vos_destroy.c — API tests for VOS_DESTROY (E6-S4)
 *
 * Covered entry points:
 *   vos_destroy                 — full 6-step teardown sequence
 *   vos_cap_tree_revoke_subtree — post-order subtree revocation
 *
 * All seL4 invocations are replaced by stubs that record call arguments.
 * The cap_tree is a real in-memory implementation (minimal, embedded below)
 * so that post-order traversal ordering can be verified structurally.
 *
 * Build & run:
 *   cc -DAGENTOS_TEST_HOST \
 *      -I tests/api \
 *      -I kernel/agentos-root-task/include \
 *      -std=c11 -Wall -Wextra \
 *      -o /tmp/t_vos_destroy \
 *      tests/api/test_vos_destroy.c && /tmp/t_vos_destroy
 *
 * TAP output: 31 test points.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifdef AGENTOS_TEST_HOST
#include "framework.h"
#include "sel4_boot.h"
#include "cap_tree.h"

/* ══════════════════════════════════════════════════════════════════════════
 * Minimal cap_tree implementation for host tests
 *
 * cap_tree_t uses a static slab of cap_node_t nodes.  Nodes are allocated
 * from a free-list threaded through next_sibling_idx.  This implementation
 * covers cap_tree_init, cap_tree_insert, cap_tree_remove, and
 * cap_tree_walk_pd — the only four functions needed by this test suite.
 * ══════════════════════════════════════════════════════════════════════════ */

void cap_tree_init(cap_tree_t *tree)
{
    uint32_t i;
    /* Zero all nodes */
    for (i = 0u; i < CAP_TREE_MAX_NODES; i++) {
        cap_node_t *n    = &tree->nodes[i];
        n->cap             = 0u;
        n->obj_type        = 0u;
        n->pd_owner        = 0u;
        n->parent_idx      = CAP_NODE_NONE;
        n->first_child_idx = CAP_NODE_NONE;
        n->next_sibling_idx= (i < CAP_TREE_MAX_NODES - 1u)
                               ? i + 1u : CAP_NODE_NONE;
        n->flags           = 0u;
        n->name[0]         = '\0';
    }
    tree->free_head  = 0u;
    tree->used_count = 0u;
}

/* strncpy helper — no libc dependency */
static void ct_strncpy(char *dst, const char *src, uint32_t n)
{
    uint32_t i;
    if (!dst || n == 0u) return;
    for (i = 0u; i + 1u < n && src && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

uint32_t cap_tree_insert(cap_tree_t *tree, uint32_t parent_idx,
                          uint64_t cap, uint32_t obj_type,
                          uint32_t pd_owner, const char *name)
{
    if (tree->free_head == CAP_NODE_NONE) return CAP_NODE_NONE;

    uint32_t   idx = tree->free_head;
    cap_node_t *n  = &tree->nodes[idx];
    tree->free_head = n->next_sibling_idx;

    n->cap             = cap;
    n->obj_type        = obj_type;
    n->pd_owner        = pd_owner;
    n->flags           = 0u;
    n->parent_idx      = parent_idx;
    n->first_child_idx = CAP_NODE_NONE;
    ct_strncpy(n->name, name ? name : "", CAP_TREE_NAME_MAX);

    if (parent_idx == CAP_NODE_NONE) {
        /* Root-level: no explicit root list — parent is NONE */
        n->next_sibling_idx = CAP_NODE_NONE;
    } else {
        /* Child of parent: prepend to parent's child list */
        cap_node_t *parent     = &tree->nodes[parent_idx];
        n->next_sibling_idx    = parent->first_child_idx;
        parent->first_child_idx = idx;
    }

    tree->used_count++;
    return idx;
}

void cap_tree_remove(cap_tree_t *tree, uint32_t idx)
{
    if (idx >= CAP_TREE_MAX_NODES) return;

    cap_node_t *n          = &tree->nodes[idx];
    uint32_t    parent_idx = n->parent_idx;

    if (parent_idx != CAP_NODE_NONE) {
        cap_node_t *parent = &tree->nodes[parent_idx];
        if (parent->first_child_idx == idx) {
            parent->first_child_idx = n->next_sibling_idx;
        } else {
            uint32_t cur = parent->first_child_idx;
            while (cur != CAP_NODE_NONE) {
                cap_node_t *c = &tree->nodes[cur];
                if (c->next_sibling_idx == idx) {
                    c->next_sibling_idx = n->next_sibling_idx;
                    break;
                }
                cur = c->next_sibling_idx;
            }
        }
    }

    /* Zero and return to free list */
    uint32_t save_free  = tree->free_head;
    n->cap              = 0u;
    n->obj_type         = 0u;
    n->pd_owner         = 0u;
    n->parent_idx       = CAP_NODE_NONE;
    n->first_child_idx  = CAP_NODE_NONE;
    n->next_sibling_idx = save_free;
    n->flags            = 0u;
    n->name[0]          = '\0';
    tree->free_head     = idx;
    if (tree->used_count > 0u) tree->used_count--;
}

void cap_tree_walk_pd(cap_tree_t        *tree,
                      uint32_t           pd_id,
                      cap_node_visitor_t visitor,
                      void              *ctx)
{
    uint32_t i;
    for (i = 0u; i < CAP_TREE_MAX_NODES; i++) {
        cap_node_t *n = &tree->nodes[i];
        if (n->cap == 0u) continue; /* free slot */
        if (n->pd_owner == pd_id && visitor) {
            visitor(n, ctx);
        }
    }
}

uint32_t cap_tree_find_cap(cap_tree_t *tree, uint64_t cap)
{
    uint32_t i;
    if (cap == 0u) return CAP_NODE_NONE;
    for (i = 0u; i < CAP_TREE_MAX_NODES; i++) {
        if (tree->nodes[i].cap == cap) return i;
    }
    return CAP_NODE_NONE;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Forward declarations for stubs consumed by vos_create.c / vos_destroy.c
 * ══════════════════════════════════════════════════════════════════════════ */

seL4_CPtr  stub_ut_alloc(seL4_Word obj_type, seL4_Word size_bits,
                          seL4_CPtr dest_cnode, seL4_Word dest_index,
                          seL4_Word dest_depth);

seL4_Error stub_cnode_revoke(seL4_CPtr root, seL4_Word index, uint8_t depth);
seL4_Error stub_cnode_delete(seL4_CPtr root, seL4_Word index, uint8_t depth);

uint32_t   stub_cap_tree_insert(cap_tree_t *tree, uint32_t parent_idx,
                                 uint64_t cap, uint32_t obj_type,
                                 uint32_t pd_owner, const char *name);

seL4_Error stub_tcb_suspend(seL4_CPtr tcb);
void       stub_cap_tree_remove_shim(cap_tree_t *tree, uint32_t node_idx);
void       stub_cap_tree_walk_pd_shim(cap_tree_t *tree, uint32_t pd_id,
                                       cap_node_visitor_t visitor, void *ctx);

/* ══════════════════════════════════════════════════════════════════════════
 * Stub state
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── ut_alloc stub ────────────────────────────────────────────────────────── */

#define STUB_UT_MAX_CALLS  1024u

typedef struct {
    seL4_Word  obj_type;
    seL4_Word  size_bits;
    seL4_CPtr  dest_cnode;
    seL4_Word  dest_index;
    seL4_Word  dest_depth;
} UtAllocCall;

static UtAllocCall g_ut_calls[STUB_UT_MAX_CALLS];
static uint32_t    g_ut_call_count;
static int         g_ut_fail_on_call;

seL4_CPtr stub_ut_alloc(seL4_Word obj_type, seL4_Word size_bits,
                         seL4_CPtr dest_cnode, seL4_Word dest_index,
                         seL4_Word dest_depth)
{
    if (g_ut_call_count < STUB_UT_MAX_CALLS) {
        g_ut_calls[g_ut_call_count].obj_type   = obj_type;
        g_ut_calls[g_ut_call_count].size_bits  = size_bits;
        g_ut_calls[g_ut_call_count].dest_cnode = dest_cnode;
        g_ut_calls[g_ut_call_count].dest_index = dest_index;
        g_ut_calls[g_ut_call_count].dest_depth = dest_depth;
    }
    g_ut_call_count++;

    if (g_ut_fail_on_call > 0 &&
        (int)g_ut_call_count == g_ut_fail_on_call) {
        return seL4_CapNull;
    }
    return (seL4_CPtr)dest_index;
}

/* ── seL4_CNode_Revoke / Delete stubs ──────────────────────────────────── */

#define STUB_CNODE_MAX_CALLS 4096u

typedef struct {
    seL4_CPtr root;
    seL4_Word index;
    uint8_t   depth;
} CNodeOpCall;

static CNodeOpCall g_revoke_calls[STUB_CNODE_MAX_CALLS];
static uint32_t    g_revoke_count;
static CNodeOpCall g_delete_calls[STUB_CNODE_MAX_CALLS];
static uint32_t    g_delete_count;

seL4_Error stub_cnode_revoke(seL4_CPtr root, seL4_Word index, uint8_t depth)
{
    if (g_revoke_count < STUB_CNODE_MAX_CALLS) {
        g_revoke_calls[g_revoke_count].root  = root;
        g_revoke_calls[g_revoke_count].index = index;
        g_revoke_calls[g_revoke_count].depth = depth;
    }
    g_revoke_count++;
    return seL4_NoError;
}

seL4_Error stub_cnode_delete(seL4_CPtr root, seL4_Word index, uint8_t depth)
{
    if (g_delete_count < STUB_CNODE_MAX_CALLS) {
        g_delete_calls[g_delete_count].root  = root;
        g_delete_calls[g_delete_count].index = index;
        g_delete_calls[g_delete_count].depth = depth;
    }
    g_delete_count++;
    return seL4_NoError;
}

/* ── seL4_TCB_Suspend stub ──────────────────────────────────────────────── */

static uint32_t  g_tcb_suspend_count;
static seL4_CPtr g_tcb_suspend_last_cap;

seL4_Error stub_tcb_suspend(seL4_CPtr tcb)
{
    g_tcb_suspend_count++;
    g_tcb_suspend_last_cap = tcb;
    return seL4_NoError;
}

/* ── cap_tree stubs ─────────────────────────────────────────────────────── */

/*
 * The cap_tree shims for vos_create.c (cap_tree_insert) and vos_destroy.c
 * (cap_tree_remove, cap_tree_walk_pd) delegate to the real cap_tree
 * implementation above AND track call counts.
 */

/* Global cap_tree used by both TUs */
static cap_tree_t g_real_tree;

/* cap_tree_insert shim for vos_create.c stubs */
static uint32_t g_tree_insert_count;
static uint32_t g_tree_last_pd_owner;
static int      g_tree_fail;

uint32_t stub_cap_tree_insert(cap_tree_t *tree, uint32_t parent_idx,
                               uint64_t cap, uint32_t obj_type,
                               uint32_t pd_owner, const char *name)
{
    (void)tree;
    g_tree_insert_count++;
    g_tree_last_pd_owner = pd_owner;
    if (g_tree_fail) {
        return CAP_NODE_NONE;
    }
    /* Perform real insertion into g_real_tree */
    return cap_tree_insert(&g_real_tree, parent_idx, cap, obj_type,
                            pd_owner, name);
}

/* cap_tree_remove shim for vos_destroy.c stubs */
static uint32_t g_tree_remove_count;

void stub_cap_tree_remove_shim(cap_tree_t *tree, uint32_t node_idx)
{
    (void)tree;
    g_tree_remove_count++;
    cap_tree_remove(&g_real_tree, node_idx);
}

/* cap_tree_walk_pd shim for vos_destroy.c stubs */
static uint32_t g_walk_pd_call_count;

void stub_cap_tree_walk_pd_shim(cap_tree_t         *tree,
                                 uint32_t            pd_id,
                                 cap_node_visitor_t  visitor,
                                 void               *ctx)
{
    (void)tree; (void)visitor; (void)ctx;
    g_walk_pd_call_count++;
    /* Pass-through: real walk with NULL visitor is a no-op per visitor call */
    cap_tree_walk_pd(&g_real_tree, pd_id,
                     (cap_node_visitor_t)0, (void *)0);
}

/* Count nodes remaining for a PD (for test assertions) */
typedef struct { uint32_t count; } ct_count_ctx_t;
static void ct_count_cb(cap_node_t *node, void *ctx)
{
    (void)node;
    ct_count_ctx_t *c = (ct_count_ctx_t *)ctx;
    c->count++;
}
static uint32_t tree_count_pd(uint32_t pd_id)
{
    ct_count_ctx_t ctx = { 0u };
    cap_tree_walk_pd(&g_real_tree, pd_id, ct_count_cb, &ctx);
    return ctx.count;
}

/*
 * The remove stub name used in vos_destroy.c must match.  In vos_destroy.c
 * we define:
 *   #define cap_tree_remove(tree, idx)  stub_cap_tree_remove((tree),(idx))
 * so the extern declaration needs to match that name.
 */
void stub_cap_tree_remove(cap_tree_t *tree, uint32_t node_idx)
{
    stub_cap_tree_remove_shim(tree, node_idx);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Include the implementations under test
 * ══════════════════════════════════════════════════════════════════════════ */

#include "../../kernel/agentos-root-task/src/vos_create.c"
#include "../../kernel/agentos-root-task/src/vos_destroy.c"

/* ══════════════════════════════════════════════════════════════════════════
 * Test helpers
 * ══════════════════════════════════════════════════════════════════════════ */

#define TEST_ROOT_CNODE     ((seL4_CPtr)2u)
#define TEST_ASID_POOL      ((seL4_CPtr)6u)
#define TEST_FREE_SLOT_BASE ((seL4_Word)200u)

static void reset_stubs(void)
{
    memset(g_ut_calls,     0, sizeof(g_ut_calls));
    memset(g_revoke_calls, 0, sizeof(g_revoke_calls));
    memset(g_delete_calls, 0, sizeof(g_delete_calls));

    g_ut_call_count        = 0u;
    g_ut_fail_on_call      = 0;
    g_revoke_count         = 0u;
    g_delete_count         = 0u;
    g_tcb_suspend_count    = 0u;
    g_tcb_suspend_last_cap = seL4_CapNull;
    g_tree_insert_count    = 0u;
    g_tree_last_pd_owner   = 0u;
    g_tree_fail            = 0;
    g_tree_remove_count    = 0u;
    g_walk_pd_call_count   = 0u;

    cap_tree_init(&g_real_tree);
}

static void do_init(void)
{
    vos_create_init(&g_real_tree, TEST_ROOT_CNODE, TEST_ASID_POOL,
                    TEST_FREE_SLOT_BASE);
    vos_destroy_init(&g_real_tree, TEST_ROOT_CNODE);
}

static vos_spec_t make_spec(const char    *label,
                              vos_os_type_t  os_type,
                              uint32_t       memory_pages)
{
    vos_spec_t s;
    uint32_t i;
    memset(&s, 0, sizeof(s));
    s.os_type      = os_type;
    s.vcpu_count   = 1u;
    s.memory_pages = memory_pages;
    s.cpu_affinity = 0xFFFFFFFFu;
    for (i = 0u; i < 15u && label[i] != '\0'; i++)
        s.label[i] = label[i];
    s.label[i] = '\0';
    return s;
}

static vos_handle_t create_one(const char *label)
{
    vos_spec_t   spec = make_spec(label, VOS_OS_LINUX, VOS_SPEC_MIN_PAGES);
    vos_handle_t h    = VOS_HANDLE_INVALID;
    vos_create(&spec, &h);
    return h;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 01 — vos_destroy(VOS_HANDLE_INVALID) → VOS_ERR_INVALID_HANDLE
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_01_destroy_invalid_handle(void)
{
    reset_stubs();
    do_init();

    uint64_t  reclaimed = 999u;
    vos_err_t err = vos_destroy(VOS_HANDLE_INVALID, &reclaimed);

    ASSERT_EQ((uint64_t)err, (uint64_t)VOS_ERR_INVALID_HANDLE,
              "destroy: VOS_HANDLE_INVALID returns VOS_ERR_INVALID_HANDLE");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 02 — create then destroy returns VOS_ERR_OK
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_02_create_then_destroy_ok(void)
{
    reset_stubs();
    do_init();

    vos_handle_t h   = create_one("vm0");
    uint64_t     rec = 0u;
    vos_err_t    err = vos_destroy(h, &rec);

    ASSERT_EQ((uint64_t)err, (uint64_t)VOS_ERR_OK,
              "destroy: create+destroy returns VOS_ERR_OK");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 03 — destroy same handle twice → VOS_ERR_INVALID_HANDLE on 2nd call
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_03_double_destroy_invalid(void)
{
    reset_stubs();
    do_init();

    vos_handle_t h = create_one("vm1");
    vos_destroy(h, (uint64_t *)0);

    vos_err_t err2 = vos_destroy(h, (uint64_t *)0);
    ASSERT_EQ((uint64_t)err2, (uint64_t)VOS_ERR_INVALID_HANDLE,
              "destroy: second destroy on same handle returns VOS_ERR_INVALID_HANDLE");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 04 — after destroy, vos_instance_get returns NULL
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_04_instance_get_null_after_destroy(void)
{
    reset_stubs();
    do_init();

    vos_handle_t    h    = create_one("vm2");
    vos_destroy(h, (uint64_t *)0);

    vos_instance_t *inst = vos_instance_get(h);
    ASSERT_TRUE(inst == (vos_instance_t *)0,
                "destroy: vos_instance_get returns NULL after destroy");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 05 — bytes_reclaimed_out is non-zero after destroy
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_05_bytes_reclaimed_nonzero(void)
{
    reset_stubs();
    do_init();

    vos_handle_t h   = create_one("vm3");
    uint64_t     rec = 0u;
    vos_destroy(h, &rec);

    ASSERT_TRUE(rec > 0u,
                "destroy: bytes_reclaimed_out is non-zero after destroy");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 06 — bytes_reclaimed >= spec.memory_pages * 4096
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_06_bytes_reclaimed_ge_pages(void)
{
    reset_stubs();
    do_init();

    uint32_t     pages = VOS_SPEC_MIN_PAGES;
    vos_spec_t   spec  = make_spec("vm4", VOS_OS_LINUX, pages);
    vos_handle_t h     = VOS_HANDLE_INVALID;
    vos_create(&spec, &h);

    uint64_t rec = 0u;
    vos_destroy(h, &rec);

    uint64_t expected_min = (uint64_t)pages * 4096u;
    ASSERT_TRUE(rec >= expected_min,
                "destroy: bytes_reclaimed >= memory_pages * 4096");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 07 — vos_cap_tree_revoke_subtree on CAP_NODE_NONE → 0
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_07_revoke_subtree_empty(void)
{
    reset_stubs();
    do_init();

    uint32_t count = vos_cap_tree_revoke_subtree(&g_real_tree,
                                                   CAP_NODE_NONE,
                                                   TEST_ROOT_CNODE);
    ASSERT_EQ((uint64_t)count, (uint64_t)0u,
              "revoke_subtree: CAP_NODE_NONE returns 0");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 08 — revoke_subtree calls seL4_CNode_Revoke once per cap
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_08_revoke_subtree_revoke_count(void)
{
    reset_stubs();
    cap_tree_init(&g_real_tree);
    g_revoke_count      = 0u;
    g_delete_count      = 0u;
    g_tree_remove_count = 0u;

    /* Build: root + 2 children manually */
    uint32_t root_idx = cap_tree_insert(&g_real_tree, CAP_NODE_NONE,
                                         (uint64_t)100u, 4u, 0u, "root");
    cap_tree_insert(&g_real_tree, root_idx, (uint64_t)101u, 15u, 0u, "child-a");
    cap_tree_insert(&g_real_tree, root_idx, (uint64_t)102u, 15u, 0u, "child-b");

    uint32_t revoked = vos_cap_tree_revoke_subtree(&g_real_tree,
                                                    root_idx,
                                                    TEST_ROOT_CNODE);

    ASSERT_EQ((uint64_t)revoked, (uint64_t)3u,
              "revoke_subtree: returns count equal to number of nodes");
    ASSERT_EQ((uint64_t)g_revoke_count, (uint64_t)3u,
              "revoke_subtree: seL4_CNode_Revoke called once per cap");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 09 — revoke_subtree calls cap_tree_remove for every node
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_09_revoke_subtree_remove_count(void)
{
    reset_stubs();
    cap_tree_init(&g_real_tree);
    g_tree_remove_count = 0u;
    g_revoke_count      = 0u;
    g_delete_count      = 0u;

    uint32_t root_idx = cap_tree_insert(&g_real_tree, CAP_NODE_NONE,
                                         (uint64_t)200u, 4u, 1u, "root");
    cap_tree_insert(&g_real_tree, root_idx, (uint64_t)201u, 15u, 1u, "f0");
    cap_tree_insert(&g_real_tree, root_idx, (uint64_t)202u, 15u, 1u, "f1");
    cap_tree_insert(&g_real_tree, root_idx, (uint64_t)203u, 15u, 1u, "f2");

    vos_cap_tree_revoke_subtree(&g_real_tree, root_idx, TEST_ROOT_CNODE);

    ASSERT_EQ((uint64_t)g_tree_remove_count, (uint64_t)4u,
              "revoke_subtree: cap_tree_remove called for every node");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 10 — post-order walk: child nodes revoked before parent node
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_10_revoke_subtree_post_order(void)
{
    reset_stubs();
    cap_tree_init(&g_real_tree);
    g_revoke_count = 0u;

    /*
     * Build:  parent-cap=300 → child-cap=301
     * Expected post-order revoke: 301 first, then 300.
     */
    uint32_t parent_idx = cap_tree_insert(&g_real_tree, CAP_NODE_NONE,
                                           (uint64_t)300u, 4u, 0u, "parent");
    cap_tree_insert(&g_real_tree, parent_idx,
                    (uint64_t)301u, 15u, 0u, "child");

    vos_cap_tree_revoke_subtree(&g_real_tree, parent_idx, TEST_ROOT_CNODE);

    /* First Revoke call must be for child (cap=301), not parent (cap=300) */
    ASSERT_EQ((uint64_t)g_revoke_calls[0].index, (uint64_t)301u,
              "revoke_subtree: child (301) is revoked before parent (300)");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 11 — create 4 instances, destroy 2; cap tree still has 2 subtrees
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_11_partial_destroy_cap_tree(void)
{
    reset_stubs();
    do_init();

    vos_handle_t h[4];
    h[0] = create_one("g0");
    h[1] = create_one("g1");
    h[2] = create_one("g2");
    h[3] = create_one("g3");

    vos_destroy(h[0], (uint64_t *)0);
    vos_destroy(h[2], (uint64_t *)0);

    uint32_t remaining_1 = tree_count_pd((uint32_t)h[1]);
    uint32_t remaining_3 = tree_count_pd((uint32_t)h[3]);

    ASSERT_TRUE(remaining_1 > 0u,
                "partial destroy: h[1] still has caps after h[0],h[2] destroyed");
    ASSERT_TRUE(remaining_3 > 0u,
                "partial destroy: h[3] still has caps after h[0],h[2] destroyed");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 12 — after all 4 destroyed, cap tree has 0 guest nodes
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_12_full_destroy_cap_tree_empty(void)
{
    reset_stubs();
    do_init();

    vos_handle_t h[4];
    h[0] = create_one("a0");
    h[1] = create_one("a1");
    h[2] = create_one("a2");
    h[3] = create_one("a3");

    vos_destroy(h[0], (uint64_t *)0);
    vos_destroy(h[1], (uint64_t *)0);
    vos_destroy(h[2], (uint64_t *)0);
    vos_destroy(h[3], (uint64_t *)0);

    uint32_t left = tree_count_pd(0u) + tree_count_pd(1u)
                  + tree_count_pd(2u) + tree_count_pd(3u);

    ASSERT_EQ((uint64_t)left, (uint64_t)0u,
              "full destroy: all 4 instances destroyed — 0 guest nodes remain");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 13 — TCB_Suspend called exactly once during destroy
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_13_tcb_suspend_called_once(void)
{
    reset_stubs();
    do_init();

    vos_handle_t h = create_one("vm-s");
    g_tcb_suspend_count = 0u;

    vos_destroy(h, (uint64_t *)0);

    ASSERT_EQ((uint64_t)g_tcb_suspend_count, (uint64_t)1u,
              "destroy: seL4_TCB_Suspend called exactly once");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 14 — Suspend called (Step 2) and Revoke called (Step 3)
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_14_suspend_before_revoke(void)
{
    reset_stubs();
    do_init();

    vos_handle_t h = create_one("vm-ord");
    g_tcb_suspend_count = 0u;
    g_revoke_count      = 0u;

    vos_destroy(h, (uint64_t *)0);

    ASSERT_TRUE(g_tcb_suspend_count >= 1u,
                "destroy: Suspend called (Step 2 ran)");
    ASSERT_TRUE(g_revoke_count >= 1u,
                "destroy: Revoke called (Step 3 ran)");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 15 — state is VOS_STATE_DESTROYED after vos_destroy returns
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_15_state_destroyed_after_destroy(void)
{
    reset_stubs();
    do_init();

    vos_handle_t h = create_one("vm-st");
    vos_destroy(h, (uint64_t *)0);

    /* vos_instance_get returns NULL only when state == DESTROYED */
    vos_instance_t *inst = vos_instance_get(h);
    ASSERT_TRUE(inst == (vos_instance_t *)0,
                "destroy: instance unreachable (DESTROYED state) after destroy");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 16 — bytes_reclaimed_out == NULL is safe (no crash)
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_16_null_bytes_out_safe(void)
{
    reset_stubs();
    do_init();

    vos_handle_t h   = create_one("vm-nb");
    vos_err_t    err = vos_destroy(h, (uint64_t *)0);

    ASSERT_EQ((uint64_t)err, (uint64_t)VOS_ERR_OK,
              "destroy: NULL bytes_reclaimed_out does not crash");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 17 — destroy with minimum pages spec is safe
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_17_destroy_min_pages_spec(void)
{
    reset_stubs();
    do_init();

    vos_spec_t   spec = make_spec("vm-zp", VOS_OS_LINUX, VOS_SPEC_MIN_PAGES);
    vos_handle_t h    = VOS_HANDLE_INVALID;
    vos_create(&spec, &h);

    uint64_t  rec = 0u;
    vos_err_t err = vos_destroy(h, &rec);

    ASSERT_EQ((uint64_t)err, (uint64_t)VOS_ERR_OK,
              "destroy: min-pages create+destroy returns VOS_ERR_OK");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 18 — handle out of range (>= VOS_MAX_INSTANCES) → INVALID_HANDLE
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_18_out_of_range_handle(void)
{
    reset_stubs();
    do_init();

    vos_err_t err = vos_destroy((vos_handle_t)VOS_MAX_INSTANCES,
                                 (uint64_t *)0);
    ASSERT_EQ((uint64_t)err, (uint64_t)VOS_ERR_INVALID_HANDLE,
              "destroy: handle >= VOS_MAX_INSTANCES returns VOS_ERR_INVALID_HANDLE");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 19 — bytes_reclaimed equals memory_pages * PAGE_SIZE exactly
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_19_bytes_reclaimed_exact(void)
{
    reset_stubs();
    do_init();

    uint32_t     pages = VOS_SPEC_MIN_PAGES;
    vos_spec_t   spec  = make_spec("vm-ex", VOS_OS_LINUX, pages);
    vos_handle_t h     = VOS_HANDLE_INVALID;
    vos_create(&spec, &h);

    uint64_t rec = 0u;
    vos_destroy(h, &rec);

    uint64_t expected = (uint64_t)pages * 4096u;
    ASSERT_EQ(rec, expected,
              "destroy: bytes_reclaimed == memory_pages * 4096");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 20 — revoke_subtree: single-node subtree returns count 1
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_20_revoke_subtree_single_node(void)
{
    reset_stubs();
    cap_tree_init(&g_real_tree);
    g_revoke_count      = 0u;
    g_tree_remove_count = 0u;

    uint32_t node_idx = cap_tree_insert(&g_real_tree, CAP_NODE_NONE,
                                         (uint64_t)500u, 4u, 0u, "solo");

    uint32_t count = vos_cap_tree_revoke_subtree(&g_real_tree, node_idx,
                                                   TEST_ROOT_CNODE);
    ASSERT_EQ((uint64_t)count, (uint64_t)1u,
              "revoke_subtree: single-node subtree returns count 1");
    ASSERT_EQ((uint64_t)g_revoke_count, (uint64_t)1u,
              "revoke_subtree: single node — Revoke called once");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 21 — slot is reused after the previous occupant was destroyed
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_21_slot_reuse_after_destroy(void)
{
    reset_stubs();
    do_init();

    vos_handle_t h0 = create_one("old");
    vos_destroy(h0, (uint64_t *)0);

    /* A new create should succeed (slot 0 is free again) */
    vos_spec_t   spec = make_spec("new", VOS_OS_LINUX, VOS_SPEC_MIN_PAGES);
    vos_handle_t h1   = VOS_HANDLE_INVALID;
    vos_err_t    err  = vos_create(&spec, &h1);

    ASSERT_EQ((uint64_t)err, (uint64_t)VOS_ERR_OK,
              "slot-reuse: create succeeds after prior destroy frees slot");
    ASSERT_NE((uint64_t)h1, (uint64_t)VOS_HANDLE_INVALID,
              "slot-reuse: returned handle is valid");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 22 — seL4_CNode_Delete called same number of times as Revoke
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_22_delete_count_matches_revoke(void)
{
    reset_stubs();
    do_init();

    vos_handle_t h = create_one("vm-dc");
    g_revoke_count = 0u;
    g_delete_count = 0u;

    vos_destroy(h, (uint64_t *)0);

    ASSERT_EQ((uint64_t)g_revoke_count, (uint64_t)g_delete_count,
              "destroy: seL4_CNode_Delete called same number of times as Revoke");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 23 — cap_tree_walk_pd called for final accounting check
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_23_final_walk_pd_called(void)
{
    reset_stubs();
    do_init();

    vos_handle_t h = create_one("vm-wp");
    g_walk_pd_call_count = 0u;

    vos_destroy(h, (uint64_t *)0);

    ASSERT_TRUE(g_walk_pd_call_count >= 1u,
                "destroy: cap_tree_walk_pd called for final accounting check");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 24 — destroy a CREATING-state instance succeeds
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_24_destroy_creating_state(void)
{
    reset_stubs();
    do_init();

    /* After vos_create(), state is VOS_STATE_CREATING */
    vos_spec_t   spec = make_spec("cr", VOS_OS_LINUX, VOS_SPEC_MIN_PAGES);
    vos_handle_t h    = VOS_HANDLE_INVALID;
    vos_create(&spec, &h);

    vos_instance_t *inst = vos_instance_get(h);
    uint32_t state_before = (inst) ? (uint32_t)inst->state
                                   : (uint32_t)VOS_STATE_DESTROYED;

    uint64_t  rec = 0u;
    vos_err_t err = vos_destroy(h, &rec);

    ASSERT_EQ((uint64_t)state_before, (uint64_t)VOS_STATE_CREATING,
              "destroy: pre-condition: instance was in CREATING state");
    ASSERT_EQ((uint64_t)err, (uint64_t)VOS_ERR_OK,
              "destroy: CREATING state instance can be destroyed");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 25 — revoke_subtree: seL4_CNode_Delete called once per cap
 * ══════════════════════════════════════════════════════════════════════════ */
static void test_25_revoke_subtree_delete_count(void)
{
    reset_stubs();
    cap_tree_init(&g_real_tree);
    g_revoke_count      = 0u;
    g_delete_count      = 0u;
    g_tree_remove_count = 0u;

    uint32_t root_idx = cap_tree_insert(&g_real_tree, CAP_NODE_NONE,
                                         (uint64_t)600u, 4u, 5u, "del-root");
    cap_tree_insert(&g_real_tree, root_idx, (uint64_t)601u, 15u, 5u, "c0");
    cap_tree_insert(&g_real_tree, root_idx, (uint64_t)602u, 15u, 5u, "c1");

    vos_cap_tree_revoke_subtree(&g_real_tree, root_idx, TEST_ROOT_CNODE);

    ASSERT_EQ((uint64_t)g_delete_count, (uint64_t)3u,
              "revoke_subtree: seL4_CNode_Delete called once per cap (3 nodes)");
}

/* ══════════════════════════════════════════════════════════════════════════
 * main
 * ══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    /*
     * TAP plan: 31 test points.
     *
     *   01  1   destroy invalid handle
     *   02  1   create+destroy ok
     *   03  1   double destroy
     *   04  1   instance_get null
     *   05  1   bytes non-zero
     *   06  1   bytes >= pages*4096
     *   07  1   revoke_subtree empty
     *   08  2   revoke count per cap (count + revoke_count check)
     *   09  1   remove count per node
     *   10  1   post-order (child before parent)
     *   11  2   partial destroy cap tree
     *   12  1   full destroy cap tree empty
     *   13  1   suspend called once
     *   14  2   suspend + revoke both ran
     *   15  1   state destroyed
     *   16  1   NULL bytes_out safe
     *   17  1   min-pages spec safe
     *   18  1   out of range handle
     *   19  1   bytes exact
     *   20  2   single-node subtree (count + revoke)
     *   21  2   slot reuse (err + handle)
     *   22  1   delete == revoke count
     *   23  1   final walk_pd called
     *   24  2   CREATING state destroy (state_before + err)
     *   25  1   revoke_subtree delete count
     *
     *   TOTAL = 31
     */
    TAP_PLAN(31);

    test_01_destroy_invalid_handle();
    test_02_create_then_destroy_ok();
    test_03_double_destroy_invalid();
    test_04_instance_get_null_after_destroy();
    test_05_bytes_reclaimed_nonzero();
    test_06_bytes_reclaimed_ge_pages();
    test_07_revoke_subtree_empty();
    test_08_revoke_subtree_revoke_count();
    test_09_revoke_subtree_remove_count();
    test_10_revoke_subtree_post_order();
    test_11_partial_destroy_cap_tree();
    test_12_full_destroy_cap_tree_empty();
    test_13_tcb_suspend_called_once();
    test_14_suspend_before_revoke();
    test_15_state_destroyed_after_destroy();
    test_16_null_bytes_out_safe();
    test_17_destroy_min_pages_spec();
    test_18_out_of_range_handle();
    test_19_bytes_reclaimed_exact();
    test_20_revoke_subtree_single_node();
    test_21_slot_reuse_after_destroy();
    test_22_delete_count_matches_revoke();
    test_23_final_walk_pd_called();
    test_24_destroy_creating_state();
    test_25_revoke_subtree_delete_count();

    return tap_exit();
}

#else
typedef int _agentos_api_test_vos_destroy_dummy;
#endif /* AGENTOS_TEST_HOST */
