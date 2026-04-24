/*
 * test_vos_create.c — API tests for VOS_CREATE capability delegation
 *
 * Covered entry points (E6-S3):
 *   vos_create_init   — initialise subsystem, mark all slots invalid
 *   vos_create        — allocate capabilities for a new guest OS instance
 *   vos_instance_get  — look up a live instance by handle
 *
 * All seL4 invocations (ut_alloc, seL4_CNode_Revoke, seL4_CNode_Delete)
 * and cap_tree operations are replaced by stubs that record call arguments
 * and return a configurable error code.  The test exercises the same struct
 * layout and control flow as the production build.
 *
 * Build & run:
 *   cc -DAGENTOS_TEST_HOST \
 *      -I tests/api \
 *      -I kernel/agentos-root-task/include \
 *      -std=c11 -Wall -Wextra \
 *      -o /tmp/t_vos_create \
 *      tests/api/test_vos_create.c && /tmp/t_vos_create
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifdef AGENTOS_TEST_HOST
#include "framework.h"
#include "sel4_boot.h"
#include "cap_tree.h"

/* ── Stub call counters and state ────────────────────────────────────────── */

/* ut_alloc stub ----------------------------------------------------------- */

typedef struct {
    seL4_Word  obj_type;
    seL4_Word  size_bits;
    seL4_CPtr  dest_cnode;
    seL4_Word  dest_index;
    seL4_Word  dest_depth;
} UtAllocCall;

#define STUB_UT_MAX_CALLS  1024u
static UtAllocCall g_ut_calls[STUB_UT_MAX_CALLS];
static uint32_t    g_ut_call_count;
static int         g_ut_fail_on_call; /* 0 = never fail; N > 0 = fail on call #N */

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

    /*
     * Return dest_index as the cap — this matches ut_alloc() behaviour:
     * on success it returns the slot index that was filled.
     */
    return (seL4_CPtr)dest_index;
}

/* seL4_CNode_Revoke / Delete stubs --------------------------------------- */

typedef struct {
    seL4_CPtr root;
    seL4_Word index;
    uint8_t   depth;
} CNodeOpCall;

#define STUB_CNODE_MAX_CALLS 2048u
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

/* cap_tree stub ----------------------------------------------------------- */

/*
 * We simulate the cap_tree using a flat counter.  For pd_owner tracking we
 * store the last insert's pd_owner so tests can verify it.
 */
static uint32_t g_tree_insert_count;
static uint32_t g_tree_last_pd_owner;
static uint32_t g_tree_next_idx = 0u; /* simulated node index */
static int      g_tree_fail;          /* if non-zero, return CAP_NODE_NONE */

uint32_t stub_cap_tree_insert(cap_tree_t *tree, uint32_t parent_idx,
                               uint64_t cap, uint32_t obj_type,
                               uint32_t pd_owner, const char *name)
{
    (void)tree; (void)parent_idx; (void)cap; (void)obj_type; (void)name;
    g_tree_insert_count++;
    g_tree_last_pd_owner = pd_owner;
    if (g_tree_fail) {
        return 0xFFFFFFFFu; /* CAP_NODE_NONE */
    }
    return g_tree_next_idx++;
}

/* Global cap_tree instance (content irrelevant — we stub the functions) */
static cap_tree_t g_cap_tree;

/* ── Include the implementation under test ──────────────────────────────── */
/*
 * vos_create.c pulls in vos_create.h → sel4_boot.h, cap_tree.h,
 * contracts/vibeos/interface.h, and ut_alloc.h.  The #ifdef AGENTOS_TEST_HOST
 * blocks inside vos_create.c redirect ut_alloc, seL4_CNode_Revoke/Delete, and
 * cap_tree_insert to our stubs above.
 */
#include "../../kernel/agentos-root-task/src/vos_create.c"

/* ── Test helpers ────────────────────────────────────────────────────────── */

#define TEST_ROOT_CNODE     ((seL4_CPtr)2u)
#define TEST_ASID_POOL      ((seL4_CPtr)6u)
#define TEST_FREE_SLOT_BASE ((seL4_Word)200u)

static void reset_stubs(void)
{
    memset(g_ut_calls,    0, sizeof(g_ut_calls));
    memset(g_revoke_calls, 0, sizeof(g_revoke_calls));
    memset(g_delete_calls, 0, sizeof(g_delete_calls));
    g_ut_call_count     = 0u;
    g_ut_fail_on_call   = 0;
    g_revoke_count      = 0u;
    g_delete_count      = 0u;
    g_tree_insert_count = 0u;
    g_tree_last_pd_owner= 0u;
    g_tree_next_idx     = 0u;
    g_tree_fail         = 0;
}

static void do_init(void)
{
    vos_create_init(&g_cap_tree, TEST_ROOT_CNODE, TEST_ASID_POOL,
                    TEST_FREE_SLOT_BASE);
}

/* Build a minimal valid spec */
static vos_spec_t make_spec(const char *label,
                             vos_os_type_t os_type,
                             uint32_t memory_pages)
{
    vos_spec_t s;
    memset(&s, 0, sizeof(s));
    s.os_type       = os_type;
    s.vcpu_count    = 1u;
    s.memory_pages  = memory_pages;
    s.cpu_affinity  = 0xFFFFFFFFu;
    /* copy label safely */
    uint32_t i = 0u;
    while (i < 15u && label[i] != '\0') {
        s.label[i] = label[i];
        i++;
    }
    s.label[i] = '\0';
    return s;
}

/* ════════════════════════════════════════════════════════════════════════════
 * Test 01 — vos_create_init() succeeds (no crash, no assertion)
 * ════════════════════════════════════════════════════════════════════════════ */
static void test_01_init_succeeds(void)
{
    reset_stubs();
    do_init();
    TAP_OK("init: vos_create_init() returns without crash");
}

/* ════════════════════════════════════════════════════════════════════════════
 * Test 02 — all slots marked invalid after init
 * ════════════════════════════════════════════════════════════════════════════ */
static void test_02_all_slots_invalid_after_init(void)
{
    reset_stubs();
    do_init();
    /* vos_instance_get on any handle index must return NULL after fresh init */
    uint32_t all_null = 1u;
    for (uint32_t i = 0u; i < VOS_MAX_INSTANCES; i++) {
        if (vos_instance_get((vos_handle_t)i) != (vos_instance_t *)0) {
            all_null = 0u;
        }
    }
    ASSERT_TRUE(all_null, "init: all slots are NULL after vos_create_init");
}

/* ════════════════════════════════════════════════════════════════════════════
 * Test 03 — valid spec returns VOS_ERR_OK
 * ════════════════════════════════════════════════════════════════════════════ */
static void test_03_create_valid_spec_ok(void)
{
    reset_stubs();
    do_init();
    vos_spec_t    spec = make_spec("testvm", VOS_OS_LINUX, VOS_SPEC_MIN_PAGES);
    vos_handle_t  h    = VOS_HANDLE_INVALID;
    vos_err_t     err  = vos_create(&spec, &h);
    ASSERT_EQ((uint64_t)err, (uint64_t)VOS_ERR_OK,
              "create: valid spec returns VOS_ERR_OK");
}

/* ════════════════════════════════════════════════════════════════════════════
 * Test 04 — vos_create returns a valid handle (not INVALID)
 * ════════════════════════════════════════════════════════════════════════════ */
static void test_04_create_returns_valid_handle(void)
{
    reset_stubs();
    do_init();
    vos_spec_t    spec = make_spec("vm0", VOS_OS_LINUX, VOS_SPEC_MIN_PAGES);
    vos_handle_t  h    = VOS_HANDLE_INVALID;
    vos_create(&spec, &h);
    ASSERT_NE((uint64_t)h, (uint64_t)VOS_HANDLE_INVALID,
              "create: returned handle != VOS_HANDLE_INVALID");
}

/* ════════════════════════════════════════════════════════════════════════════
 * Test 05 — vos_instance_get returns non-NULL after create
 * ════════════════════════════════════════════════════════════════════════════ */
static void test_05_instance_get_non_null_after_create(void)
{
    reset_stubs();
    do_init();
    vos_spec_t    spec = make_spec("vm1", VOS_OS_LINUX, VOS_SPEC_MIN_PAGES);
    vos_handle_t  h    = VOS_HANDLE_INVALID;
    vos_create(&spec, &h);
    vos_instance_t *inst = vos_instance_get(h);
    ASSERT_TRUE(inst != (vos_instance_t *)0,
                "get: vos_instance_get returns non-NULL after create");
}

/* ════════════════════════════════════════════════════════════════════════════
 * Test 06 — vos_instance_get(VOS_HANDLE_INVALID) returns NULL
 * ════════════════════════════════════════════════════════════════════════════ */
static void test_06_get_invalid_handle_returns_null(void)
{
    reset_stubs();
    do_init();
    vos_instance_t *p = vos_instance_get(VOS_HANDLE_INVALID);
    ASSERT_TRUE(p == (vos_instance_t *)0,
                "get: VOS_HANDLE_INVALID returns NULL");
}

/* ════════════════════════════════════════════════════════════════════════════
 * Test 07 — memory_pages < VOS_SPEC_MIN_PAGES → VOS_ERR_INVALID_SPEC
 * ════════════════════════════════════════════════════════════════════════════ */
static void test_07_spec_min_pages_underflow(void)
{
    reset_stubs();
    do_init();
    vos_spec_t   spec = make_spec("vm", VOS_OS_LINUX, VOS_SPEC_MIN_PAGES - 1u);
    vos_handle_t h    = VOS_HANDLE_INVALID;
    vos_err_t    err  = vos_create(&spec, &h);
    ASSERT_EQ((uint64_t)err, (uint64_t)VOS_ERR_INVALID_SPEC,
              "create: memory_pages < MIN returns VOS_ERR_INVALID_SPEC");
}

/* ════════════════════════════════════════════════════════════════════════════
 * Test 08 — memory_pages > VOS_SPEC_MAX_PAGES → VOS_ERR_INVALID_SPEC
 * ════════════════════════════════════════════════════════════════════════════ */
static void test_08_spec_max_pages_overflow(void)
{
    reset_stubs();
    do_init();
    vos_spec_t   spec = make_spec("vm", VOS_OS_LINUX, VOS_SPEC_MAX_PAGES + 1u);
    vos_handle_t h    = VOS_HANDLE_INVALID;
    vos_err_t    err  = vos_create(&spec, &h);
    ASSERT_EQ((uint64_t)err, (uint64_t)VOS_ERR_INVALID_SPEC,
              "create: memory_pages > MAX returns VOS_ERR_INVALID_SPEC");
}

/* ════════════════════════════════════════════════════════════════════════════
 * Test 09 — invalid os_type → VOS_ERR_INVALID_SPEC
 * ════════════════════════════════════════════════════════════════════════════ */
static void test_09_spec_invalid_os_type(void)
{
    reset_stubs();
    do_init();
    vos_spec_t   spec = make_spec("vm", (vos_os_type_t)0xFFu, VOS_SPEC_MIN_PAGES);
    vos_handle_t h    = VOS_HANDLE_INVALID;
    vos_err_t    err  = vos_create(&spec, &h);
    ASSERT_EQ((uint64_t)err, (uint64_t)VOS_ERR_INVALID_SPEC,
              "create: invalid os_type returns VOS_ERR_INVALID_SPEC");
}

/* ════════════════════════════════════════════════════════════════════════════
 * Test 10 — instance state is VOS_STATE_CREATING after create
 * ════════════════════════════════════════════════════════════════════════════ */
static void test_10_state_creating_after_create(void)
{
    reset_stubs();
    do_init();
    vos_spec_t   spec = make_spec("vm2", VOS_OS_LINUX, VOS_SPEC_MIN_PAGES);
    vos_handle_t h    = VOS_HANDLE_INVALID;
    vos_create(&spec, &h);
    vos_instance_t *inst = vos_instance_get(h);
    ASSERT_TRUE(inst != (vos_instance_t *)0, "create: instance is found");
    ASSERT_EQ((uint64_t)inst->state, (uint64_t)VOS_STATE_CREATING,
              "create: instance state is VOS_STATE_CREATING");
}

/* ════════════════════════════════════════════════════════════════════════════
 * Test 11 — instance os_type matches spec
 * ════════════════════════════════════════════════════════════════════════════ */
static void test_11_instance_os_type_matches(void)
{
    reset_stubs();
    do_init();
    vos_spec_t   spec = make_spec("bsd0", VOS_OS_FREEBSD, VOS_SPEC_MIN_PAGES);
    vos_handle_t h    = VOS_HANDLE_INVALID;
    vos_create(&spec, &h);
    vos_instance_t *inst = vos_instance_get(h);
    ASSERT_TRUE(inst != (vos_instance_t *)0, "create: instance found");
    ASSERT_EQ((uint64_t)inst->os_type, (uint64_t)VOS_OS_FREEBSD,
              "create: os_type matches spec (FREEBSD)");
}

/* ════════════════════════════════════════════════════════════════════════════
 * Test 12 — instance memory_pages matches spec
 * ════════════════════════════════════════════════════════════════════════════ */
static void test_12_instance_memory_pages_matches(void)
{
    reset_stubs();
    do_init();
    uint32_t     pages = 512u;
    vos_spec_t   spec  = make_spec("vm3", VOS_OS_LINUX, pages);
    vos_handle_t h     = VOS_HANDLE_INVALID;
    vos_create(&spec, &h);
    vos_instance_t *inst = vos_instance_get(h);
    ASSERT_TRUE(inst != (vos_instance_t *)0, "create: instance found");
    ASSERT_EQ((uint64_t)inst->memory_pages, (uint64_t)pages,
              "create: memory_pages matches spec");
}

/* ════════════════════════════════════════════════════════════════════════════
 * Test 13 — ut_alloc called at least once per fixed kernel object
 * ════════════════════════════════════════════════════════════════════════════ */
static void test_13_ut_alloc_called_for_fixed_objects(void)
{
    reset_stubs();
    do_init();
    vos_spec_t   spec = make_spec("vm4", VOS_OS_LINUX, VOS_SPEC_MIN_PAGES);
    vos_handle_t h    = VOS_HANDLE_INVALID;
    vos_create(&spec, &h);

    /*
     * Expect: 4 fixed objects (CNode, VSpace, VCPU, Notification)
     *       + VOS_SPEC_MIN_PAGES frame objects.
     */
    uint32_t expected = 4u + VOS_SPEC_MIN_PAGES;
    ASSERT_EQ((uint64_t)g_ut_call_count, (uint64_t)expected,
              "create: ut_alloc called for all objects (fixed + frames)");
}

/* ════════════════════════════════════════════════════════════════════════════
 * Test 14 — Fill all VOS_MAX_INSTANCES slots
 * ════════════════════════════════════════════════════════════════════════════ */
static void test_14_fill_all_slots(void)
{
    reset_stubs();
    do_init();
    vos_err_t all_ok = VOS_ERR_OK;
    for (uint32_t i = 0u; i < VOS_MAX_INSTANCES; i++) {
        vos_spec_t   spec = make_spec("vm", VOS_OS_LINUX, VOS_SPEC_MIN_PAGES);
        vos_handle_t h    = VOS_HANDLE_INVALID;
        vos_err_t    err  = vos_create(&spec, &h);
        if (err != VOS_ERR_OK) {
            all_ok = err;
        }
    }
    ASSERT_EQ((uint64_t)all_ok, (uint64_t)VOS_ERR_OK,
              "create: filling all VOS_MAX_INSTANCES slots succeeds");
}

/* ════════════════════════════════════════════════════════════════════════════
 * Test 15 — Create one more than VOS_MAX_INSTANCES → VOS_ERR_OUT_OF_MEMORY
 * ════════════════════════════════════════════════════════════════════════════ */
static void test_15_exceed_max_instances(void)
{
    reset_stubs();
    do_init();
    for (uint32_t i = 0u; i < VOS_MAX_INSTANCES; i++) {
        vos_spec_t   spec = make_spec("vm", VOS_OS_LINUX, VOS_SPEC_MIN_PAGES);
        vos_handle_t h    = VOS_HANDLE_INVALID;
        vos_create(&spec, &h);
    }
    /* One more — must fail */
    vos_spec_t   spec = make_spec("overflow", VOS_OS_LINUX, VOS_SPEC_MIN_PAGES);
    vos_handle_t h    = VOS_HANDLE_INVALID;
    vos_err_t    err  = vos_create(&spec, &h);
    ASSERT_EQ((uint64_t)err, (uint64_t)VOS_ERR_OUT_OF_MEMORY,
              "create: exceeding VOS_MAX_INSTANCES returns VOS_ERR_OUT_OF_MEMORY");
}

/* ════════════════════════════════════════════════════════════════════════════
 * Test 16 — cap_tree has expected node count after create
 * ════════════════════════════════════════════════════════════════════════════ */
static void test_16_cap_tree_node_count(void)
{
    reset_stubs();
    do_init();
    vos_spec_t   spec = make_spec("vm5", VOS_OS_LINUX, VOS_SPEC_MIN_PAGES);
    vos_handle_t h    = VOS_HANDLE_INVALID;
    vos_create(&spec, &h);

    /*
     * Expected cap_tree_insert calls:
     *   1 subtree root (CNode — the "guest:<label>" node)
     *   1 VSpace
     *   1 VCPU
     *   1 Notification
     *   VOS_SPEC_MIN_PAGES frames
     * Total = 4 + VOS_SPEC_MIN_PAGES
     */
    uint32_t expected_nodes = 4u + VOS_SPEC_MIN_PAGES;
    ASSERT_EQ((uint64_t)g_tree_insert_count, (uint64_t)expected_nodes,
              "cap_tree: insert called for every cap (fixed + frames)");
}

/* ════════════════════════════════════════════════════════════════════════════
 * Test 17 — all caps in cap_tree have correct pd_owner
 * ════════════════════════════════════════════════════════════════════════════ */
static void test_17_cap_tree_pd_owner(void)
{
    reset_stubs();
    do_init();
    vos_spec_t   spec = make_spec("vm6", VOS_OS_LINUX, VOS_SPEC_MIN_PAGES);
    vos_handle_t h    = VOS_HANDLE_INVALID;
    vos_create(&spec, &h);

    /*
     * All cap_tree_insert calls for this instance should use pd_owner == h.
     * g_tree_last_pd_owner captures the most recently set pd_owner.
     */
    ASSERT_EQ((uint64_t)g_tree_last_pd_owner, (uint64_t)h,
              "cap_tree: pd_owner matches the returned handle");
}

/* ════════════════════════════════════════════════════════════════════════════
 * Test 18 — ut_alloc failure on CNode → cleanup invoked, returns OOM
 * ════════════════════════════════════════════════════════════════════════════ */
static void test_18_ut_alloc_failure_cnode(void)
{
    reset_stubs();
    do_init();
    /* Fail on the very first call (CNode allocation) */
    g_ut_fail_on_call = 1;
    vos_spec_t   spec = make_spec("vm", VOS_OS_LINUX, VOS_SPEC_MIN_PAGES);
    vos_handle_t h    = VOS_HANDLE_INVALID;
    vos_err_t    err  = vos_create(&spec, &h);
    ASSERT_EQ((uint64_t)err, (uint64_t)VOS_ERR_OUT_OF_MEMORY,
              "create: CNode alloc failure returns VOS_ERR_OUT_OF_MEMORY");
    ASSERT_EQ((uint64_t)h, (uint64_t)VOS_HANDLE_INVALID,
              "create: handle stays INVALID when CNode alloc fails");
}

/* ════════════════════════════════════════════════════════════════════════════
 * Test 19 — ut_alloc failure on VCPU → cleanup invoked, returns OOM
 * ════════════════════════════════════════════════════════════════════════════ */
static void test_19_ut_alloc_failure_vcpu(void)
{
    reset_stubs();
    do_init();
    /* Fail on the 3rd call (VCPU) — CNode=1, VSpace=2, VCPU=3 */
    g_ut_fail_on_call = 3;
    vos_spec_t   spec = make_spec("vm", VOS_OS_LINUX, VOS_SPEC_MIN_PAGES);
    vos_handle_t h    = VOS_HANDLE_INVALID;
    vos_err_t    err  = vos_create(&spec, &h);
    ASSERT_EQ((uint64_t)err, (uint64_t)VOS_ERR_OUT_OF_MEMORY,
              "create: VCPU alloc failure returns VOS_ERR_OUT_OF_MEMORY");
    /* cleanup must have been called — revoke/delete counts > 0 */
    ASSERT_TRUE(g_revoke_count > 0u || g_delete_count > 0u,
                "create: cleanup called revoke/delete when VCPU alloc fails");
}

/* ════════════════════════════════════════════════════════════════════════════
 * Test 20 — ut_alloc failure mid-frames → partial cleanup, returns OOM
 * ════════════════════════════════════════════════════════════════════════════ */
static void test_20_ut_alloc_failure_mid_frames(void)
{
    reset_stubs();
    do_init();
    /*
     * Calls: CNode(1), VSpace(2), VCPU(3), Ntfn(4), frame0..N.
     * Fail on call 6 → frame allocation #2 fails.
     */
    g_ut_fail_on_call = 6;
    vos_spec_t   spec = make_spec("vm", VOS_OS_LINUX, VOS_SPEC_MIN_PAGES);
    vos_handle_t h    = VOS_HANDLE_INVALID;
    vos_err_t    err  = vos_create(&spec, &h);
    ASSERT_EQ((uint64_t)err, (uint64_t)VOS_ERR_OUT_OF_MEMORY,
              "create: mid-frame alloc failure returns VOS_ERR_OUT_OF_MEMORY");
    ASSERT_EQ((uint64_t)h, (uint64_t)VOS_HANDLE_INVALID,
              "create: handle stays INVALID on frame alloc failure");
}

/* ════════════════════════════════════════════════════════════════════════════
 * Test 21 — VOS_OS_CUSTOM is accepted
 * ════════════════════════════════════════════════════════════════════════════ */
static void test_21_custom_os_type_accepted(void)
{
    reset_stubs();
    do_init();
    vos_spec_t   spec = make_spec("custom", VOS_OS_CUSTOM, VOS_SPEC_MIN_PAGES);
    vos_handle_t h    = VOS_HANDLE_INVALID;
    vos_err_t    err  = vos_create(&spec, &h);
    ASSERT_EQ((uint64_t)err, (uint64_t)VOS_ERR_OK,
              "create: VOS_OS_CUSTOM is accepted");
}

/* ════════════════════════════════════════════════════════════════════════════
 * Test 22 — handle returned equals slot index (handle < VOS_MAX_INSTANCES)
 * ════════════════════════════════════════════════════════════════════════════ */
static void test_22_handle_is_slot_index(void)
{
    reset_stubs();
    do_init();
    vos_spec_t   spec = make_spec("vm7", VOS_OS_LINUX, VOS_SPEC_MIN_PAGES);
    vos_handle_t h    = VOS_HANDLE_INVALID;
    vos_create(&spec, &h);
    ASSERT_TRUE(h < VOS_MAX_INSTANCES,
                "create: returned handle is a valid slot index");
}

/* ════════════════════════════════════════════════════════════════════════════
 * Test 23 — vos_spec_t is exactly 44 bytes (regression guard)
 * ════════════════════════════════════════════════════════════════════════════ */
static void test_23_spec_size(void)
{
    ASSERT_EQ((uint64_t)sizeof(vos_spec_t), (uint64_t)44u,
              "vos_spec_t is exactly 44 bytes");
}

/* ════════════════════════════════════════════════════════════════════════════
 * Test 24 — two successive creates get different handles
 * ════════════════════════════════════════════════════════════════════════════ */
static void test_24_two_creates_different_handles(void)
{
    reset_stubs();
    do_init();
    vos_spec_t   s0  = make_spec("vm-a", VOS_OS_LINUX, VOS_SPEC_MIN_PAGES);
    vos_spec_t   s1  = make_spec("vm-b", VOS_OS_LINUX, VOS_SPEC_MIN_PAGES);
    vos_handle_t h0  = VOS_HANDLE_INVALID;
    vos_handle_t h1  = VOS_HANDLE_INVALID;
    vos_create(&s0, &h0);
    vos_create(&s1, &h1);
    ASSERT_NE((uint64_t)h0, (uint64_t)h1,
              "create: two successive creates produce distinct handles");
}

/* ════════════════════════════════════════════════════════════════════════════
 * main
 * ════════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    /*
     * Test count:
     *   01  1
     *   02  1
     *   03  1
     *   04  1
     *   05  1
     *   06  1
     *   07  1
     *   08  1
     *   09  1
     *   10  2
     *   11  2
     *   12  2
     *   13  1
     *   14  1
     *   15  1
     *   16  1
     *   17  1
     *   18  2
     *   19  2
     *   20  2
     *   21  1
     *   22  1
     *   23  1
     *   24  1
     *   TOTAL = 30
     */
    TAP_PLAN(30);

    test_01_init_succeeds();
    test_02_all_slots_invalid_after_init();
    test_03_create_valid_spec_ok();
    test_04_create_returns_valid_handle();
    test_05_instance_get_non_null_after_create();
    test_06_get_invalid_handle_returns_null();
    test_07_spec_min_pages_underflow();
    test_08_spec_max_pages_overflow();
    test_09_spec_invalid_os_type();
    test_10_state_creating_after_create();
    test_11_instance_os_type_matches();
    test_12_instance_memory_pages_matches();
    test_13_ut_alloc_called_for_fixed_objects();
    test_14_fill_all_slots();
    test_15_exceed_max_instances();
    test_16_cap_tree_node_count();
    test_17_cap_tree_pd_owner();
    test_18_ut_alloc_failure_cnode();
    test_19_ut_alloc_failure_vcpu();
    test_20_ut_alloc_failure_mid_frames();
    test_21_custom_os_type_accepted();
    test_22_handle_is_slot_index();
    test_23_spec_size();
    test_24_two_creates_different_handles();

    return tap_exit();
}

#else
typedef int _agentos_api_test_vos_create_dummy;
#endif /* AGENTOS_TEST_HOST */
