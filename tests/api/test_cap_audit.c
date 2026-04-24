/*
 * test_cap_audit.c — API tests for OP_CAP_AUDIT and OP_CAP_AUDIT_GUEST
 *
 * 20 TAP tests covering:
 *
 *   1.  cap_audit_entry_t is exactly 32 bytes
 *   2.  OP_CAP_AUDIT from non-controller badge → SEL4_ERR_FORBIDDEN
 *   3.  OP_CAP_AUDIT from controller badge → SEL4_ERR_OK
 *   4.  OP_CAP_AUDIT (all PDs) count equals total cap_acct_count()
 *   5.  OP_CAP_AUDIT with pd_id==0 returns all nodes (count > 0)
 *   6.  OP_CAP_AUDIT with pd_id==0xFF99 (unused) returns count==0
 *   7.  OP_CAP_AUDIT entry 0 has valid pd_id field
 *   8.  OP_CAP_AUDIT entry 0 has valid cslot field (cap value)
 *   9.  OP_CAP_AUDIT entry 0 has non-empty name
 *   10. OP_CAP_AUDIT rep.data[0..3] encodes count little-endian
 *   11. OP_CAP_AUDIT_GUEST from non-controller badge → SEL4_ERR_FORBIDDEN
 *   12. OP_CAP_AUDIT_GUEST with VOS_HANDLE_INVALID → SEL4_ERR_NOT_FOUND
 *   13. OP_CAP_AUDIT_GUEST with out-of-range handle → SEL4_ERR_NOT_FOUND
 *   14. OP_CAP_AUDIT_GUEST with valid handle (stub) → SEL4_ERR_OK
 *   15. OP_CAP_AUDIT_GUEST result count matches expected guest cap count
 *   16. audit buffer reset between calls (entries are zeroed)
 *   17. revocable==0 for root-task (pd_index==0) caps
 *   18. revocable==1 for non-root PD caps
 *   19. Multiple sequential OP_CAP_AUDIT calls produce same count
 *   20. cap_tree_verify_all_pds completes without crash
 *
 * Build & run:
 *   cc -DAGENTOS_TEST_HOST \
 *      -I tests/api \
 *      -I kernel/agentos-root-task/include \
 *      -I . \
 *      -std=c11 -Wall -Wextra -Wpedantic \
 *      -o /tmp/test_cap_audit \
 *      tests/api/test_cap_audit.c && /tmp/test_cap_audit
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifdef AGENTOS_TEST_HOST

#include "framework.h"

/*
 * ── Stub types and implementations ───────────────────────────────────────────
 *
 * cap_audit.c under AGENTOS_TEST_HOST expects the caller to provide:
 *   - cap_acct_entry_t type and cap_acct_count() / cap_acct_get()
 *   - vos_handle_t type, VOS_HANDLE_INVALID constant
 *   - vos_instance_t type and vos_instance_get()
 *
 * These must be defined BEFORE the #include of cap_audit.c so that the
 * implementation file sees them.  We also define the guard macros that
 * cap_audit.c's header-inclusion guards check, to prevent double-definition.
 */

/* ── cap_acct_entry_t stub ───────────────────────────────────────────────── */

/*
 * Match the field names cap_audit.c uses: .cap, .obj_type, .pd_index, .name
 * Use uintptr_t for cap so the (uint32_t)(uintptr_t) cast in write_audit_entry
 * is safe on both 32-bit and 64-bit hosts.
 */
typedef struct {
    uintptr_t  cap;
    uint32_t   obj_type;
    uint32_t   pd_index;
    char       name[16];
} cap_acct_entry_t;

/*
 * Stub table — 10 entries with known pd_index / cap values.
 *
 *   Indices 0-2: pd_index=0 (root task) — root-level caps
 *   Indices 3-4: pd_index=1 (nameserver)
 *   Indices 5-6: pd_index=2 (controller)
 *   Indices 7-9: pd_index=3 (VMM / guest handle==3)
 */
#define STUB_TABLE_SIZE 10u

static cap_acct_entry_t g_stub_table[STUB_TABLE_SIZE] = {
    { 1u,   10u, 0u, "root-cnode"  },
    { 2u,   11u, 0u, "root-vspace" },
    { 3u,    1u, 0u, "root-tcb"    },
    { 100u,  1u, 1u, "ns-tcb"      },
    { 101u,  2u, 1u, "ns-ep"       },
    { 200u,  1u, 2u, "ctrl-tcb"    },
    { 201u,  2u, 2u, "ctrl-ep"     },
    { 300u,  1u, 3u, "vmm-tcb"     },
    { 301u,  2u, 3u, "vmm-ep"      },
    { 302u,  3u, 3u, "vmm-vcpu"    },
};

uint32_t cap_acct_count(void)
{
    return STUB_TABLE_SIZE;
}

const cap_acct_entry_t *cap_acct_get(uint32_t index)
{
    if (index >= STUB_TABLE_SIZE) return (const cap_acct_entry_t *)0;
    return &g_stub_table[index];
}

/* ── vos_handle_t / vos_instance_t stub ─────────────────────────────────── */

typedef uint32_t vos_handle_t;

#define VOS_HANDLE_INVALID UINT32_C(0xFFFFFFFF)
#define VOS_MAX_INSTANCES  UINT32_C(4)   /* vibeos contract value */

/*
 * Minimal vos_instance_t — cap_audit.c only uses .handle in the type
 * returned from vos_instance_get().  Provide a minimal struct that
 * satisfies the cast to (const vos_instance_t *).
 */
typedef struct {
    vos_handle_t handle;
    uint32_t     cap_subtree_root;
} vos_instance_t;

/* One live guest at slot 3 (pd_index==3 in stub table) */
static vos_instance_t g_stub_inst = { .handle = 3u, .cap_subtree_root = 0u };

vos_instance_t *vos_instance_get(vos_handle_t h)
{
    if (h == VOS_HANDLE_INVALID || h >= VOS_MAX_INSTANCES) {
        return (vos_instance_t *)0;
    }
    if (h == g_stub_inst.handle) return &g_stub_inst;
    return (vos_instance_t *)0;
}

/* ── Pull in the unit under test ─────────────────────────────────────────── */

#include "../../kernel/agentos-root-task/src/cap_audit.c"

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/*
 * Decode a little-endian uint32_t from the first 4 bytes of rep.data[].
 */
static uint32_t rep_count(const sel4_msg_t *rep)
{
    return (uint32_t)rep->data[0]
         | ((uint32_t)rep->data[1] << 8u)
         | ((uint32_t)rep->data[2] << 16u)
         | ((uint32_t)rep->data[3] << 24u);
}

/* Build a controller badge: bits[47:32] == CONTROLLER_CLIENT_ID == 0 */
static sel4_badge_t ctrl_badge(void)
{
    return (sel4_badge_t)0u;
}

/* Build a non-controller badge: bits[47:32] == 7 */
static sel4_badge_t stranger_badge(void)
{
    return ((sel4_badge_t)7u) << 32u;
}

/* Build a request message with opcode and one uint32_t argument */
static sel4_msg_t make_req_u32(uint32_t opcode, uint32_t arg)
{
    sel4_msg_t req;
    req.opcode  = opcode;
    req.length  = 4u;
    for (uint32_t i = 0u; i < SEL4_MSG_DATA_BYTES; i++) req.data[i] = 0u;
    req.data[0] = (uint8_t)(arg & 0xFFu);
    req.data[1] = (uint8_t)((arg >> 8u)  & 0xFFu);
    req.data[2] = (uint8_t)((arg >> 16u) & 0xFFu);
    req.data[3] = (uint8_t)((arg >> 24u) & 0xFFu);
    return req;
}

/* ── Tests ───────────────────────────────────────────────────────────────── */

/* Test 1: cap_audit_entry_t is exactly 32 bytes */
static void test_entry_size(void)
{
    ASSERT_EQ(sizeof(cap_audit_entry_t), 32u,
              "cap_audit_entry_t is exactly 32 bytes");
}

/* Test 2: non-controller badge → SEL4_ERR_FORBIDDEN */
static void test_non_ctrl_badge_denied(void)
{
    cap_audit_test_reset();
    sel4_msg_t req = make_req_u32(OP_CAP_AUDIT, 0u);
    sel4_msg_t rep;
    uint32_t rc = handle_cap_audit(stranger_badge(), &req, &rep, (void *)0);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_FORBIDDEN,
              "OP_CAP_AUDIT: non-controller badge → SEL4_ERR_FORBIDDEN");
}

/* Test 3: controller badge → SEL4_ERR_OK */
static void test_ctrl_badge_ok(void)
{
    cap_audit_test_reset();
    sel4_msg_t req = make_req_u32(OP_CAP_AUDIT, 0u);
    sel4_msg_t rep;
    uint32_t rc = handle_cap_audit(ctrl_badge(), &req, &rep, (void *)0);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK,
              "OP_CAP_AUDIT: controller badge → SEL4_ERR_OK");
}

/* Test 4: pd_id==0 count equals total cap_acct_count() */
static void test_audit_all_count_equals_total(void)
{
    cap_audit_test_reset();
    sel4_msg_t req = make_req_u32(OP_CAP_AUDIT, 0u);
    sel4_msg_t rep;
    handle_cap_audit(ctrl_badge(), &req, &rep, (void *)0);
    uint32_t count = rep_count(&rep);
    ASSERT_EQ(count, (uint64_t)cap_acct_count(),
              "OP_CAP_AUDIT pd_id=0: count equals cap_acct_count()");
}

/* Test 5: pd_id==0 returns all nodes (count > 0) */
static void test_audit_all_nonzero(void)
{
    cap_audit_test_reset();
    sel4_msg_t req = make_req_u32(OP_CAP_AUDIT, 0u);
    sel4_msg_t rep;
    handle_cap_audit(ctrl_badge(), &req, &rep, (void *)0);
    uint32_t count = rep_count(&rep);
    ASSERT_TRUE(count > 0u,
                "OP_CAP_AUDIT pd_id=0: count > 0");
}

/* Test 6: unused pd_id → count==0 */
static void test_audit_unused_pd_zero_count(void)
{
    cap_audit_test_reset();
    sel4_msg_t req = make_req_u32(OP_CAP_AUDIT, 0xFF99u);
    sel4_msg_t rep;
    handle_cap_audit(ctrl_badge(), &req, &rep, (void *)0);
    uint32_t count = rep_count(&rep);
    ASSERT_EQ(count, 0u,
              "OP_CAP_AUDIT pd_id=0xFF99 (unused): count==0");
}

/* Test 7: entry 0 has valid pd_id field */
static void test_entry0_pd_id(void)
{
    cap_audit_test_reset();
    sel4_msg_t req = make_req_u32(OP_CAP_AUDIT, 0u);
    sel4_msg_t rep;
    handle_cap_audit(ctrl_badge(), &req, &rep, (void *)0);
    const cap_audit_entry_t *e = cap_audit_test_get_entry(0u);
    /* First entry comes from stub table[0]: pd_index==0 */
    ASSERT_EQ(e ? e->pd_id : 0xFFFFu, 0u,
              "OP_CAP_AUDIT: entry[0].pd_id == 0 (root task)");
}

/* Test 8: entry 0 has valid cslot field (CPtr from stub table[0] == 1) */
static void test_entry0_cslot(void)
{
    cap_audit_test_reset();
    sel4_msg_t req = make_req_u32(OP_CAP_AUDIT, 0u);
    sel4_msg_t rep;
    handle_cap_audit(ctrl_badge(), &req, &rep, (void *)0);
    const cap_audit_entry_t *e = cap_audit_test_get_entry(0u);
    ASSERT_EQ(e ? e->cslot : (uint32_t)-1u, 1u,
              "OP_CAP_AUDIT: entry[0].cslot == 1 (root-cnode CPtr)");
}

/* Test 9: entry 0 has non-empty name */
static void test_entry0_name_nonempty(void)
{
    cap_audit_test_reset();
    sel4_msg_t req = make_req_u32(OP_CAP_AUDIT, 0u);
    sel4_msg_t rep;
    handle_cap_audit(ctrl_badge(), &req, &rep, (void *)0);
    const cap_audit_entry_t *e = cap_audit_test_get_entry(0u);
    ASSERT_TRUE(e && e->name[0] != '\0',
                "OP_CAP_AUDIT: entry[0].name is non-empty");
}

/* Test 10: rep.data[0..3] encodes count in little-endian */
static void test_rep_count_little_endian(void)
{
    cap_audit_test_reset();
    sel4_msg_t req = make_req_u32(OP_CAP_AUDIT, 0u);
    sel4_msg_t rep;
    handle_cap_audit(ctrl_badge(), &req, &rep, (void *)0);
    uint32_t from_rep  = rep_count(&rep);
    uint32_t from_acct = cap_acct_count();
    ASSERT_EQ(from_rep, (uint64_t)from_acct,
              "OP_CAP_AUDIT: rep.data[0..3] little-endian count matches total");
}

/* Test 11: non-controller badge on GUEST → SEL4_ERR_FORBIDDEN */
static void test_guest_non_ctrl_denied(void)
{
    cap_audit_test_reset();
    sel4_msg_t req = make_req_u32(OP_CAP_AUDIT_GUEST, 3u);
    sel4_msg_t rep;
    uint32_t rc = handle_cap_audit_guest(stranger_badge(), &req, &rep, (void *)0);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_FORBIDDEN,
              "OP_CAP_AUDIT_GUEST: non-controller badge → SEL4_ERR_FORBIDDEN");
}

/* Test 12: VOS_HANDLE_INVALID → SEL4_ERR_NOT_FOUND */
static void test_guest_invalid_handle(void)
{
    cap_audit_test_reset();
    sel4_msg_t req = make_req_u32(OP_CAP_AUDIT_GUEST, VOS_HANDLE_INVALID);
    sel4_msg_t rep;
    uint32_t rc = handle_cap_audit_guest(ctrl_badge(), &req, &rep, (void *)0);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_NOT_FOUND,
              "OP_CAP_AUDIT_GUEST: VOS_HANDLE_INVALID → SEL4_ERR_NOT_FOUND");
}

/* Test 13: out-of-range handle → SEL4_ERR_NOT_FOUND */
static void test_guest_out_of_range(void)
{
    cap_audit_test_reset();
    /* VOS_MAX_INSTANCES == 4; handle 99 is beyond range */
    sel4_msg_t req = make_req_u32(OP_CAP_AUDIT_GUEST, 99u);
    sel4_msg_t rep;
    uint32_t rc = handle_cap_audit_guest(ctrl_badge(), &req, &rep, (void *)0);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_NOT_FOUND,
              "OP_CAP_AUDIT_GUEST: out-of-range handle → SEL4_ERR_NOT_FOUND");
}

/* Test 14: valid handle → SEL4_ERR_OK */
static void test_guest_valid_handle_ok(void)
{
    cap_audit_test_reset();
    /* Stub instance at handle==3 */
    sel4_msg_t req = make_req_u32(OP_CAP_AUDIT_GUEST, 3u);
    sel4_msg_t rep;
    uint32_t rc = handle_cap_audit_guest(ctrl_badge(), &req, &rep, (void *)0);
    ASSERT_EQ(rc, (uint64_t)SEL4_ERR_OK,
              "OP_CAP_AUDIT_GUEST: valid handle==3 → SEL4_ERR_OK");
}

/* Test 15: guest count == 3 (stub entries 7,8,9 have pd_index==3) */
static void test_guest_count_in_rep(void)
{
    cap_audit_test_reset();
    sel4_msg_t req = make_req_u32(OP_CAP_AUDIT_GUEST, 3u);
    sel4_msg_t rep;
    handle_cap_audit_guest(ctrl_badge(), &req, &rep, (void *)0);
    uint32_t count = rep_count(&rep);
    ASSERT_EQ(count, 3u,
              "OP_CAP_AUDIT_GUEST handle==3: count == 3 (vmm caps in stub)");
}

/* Test 16: reset zeroes audit buffer entries */
static void test_reset_zeroes_buffer(void)
{
    sel4_msg_t req = make_req_u32(OP_CAP_AUDIT, 0u);
    sel4_msg_t rep;
    handle_cap_audit(ctrl_badge(), &req, &rep, (void *)0);
    cap_audit_test_reset();
    const cap_audit_entry_t *e = cap_audit_test_get_entry(0u);
    ASSERT_TRUE(e && e->pd_id == 0u && e->cslot == 0u && e->name[0] == '\0',
                "cap_audit_test_reset: entry[0] is fully zeroed");
}

/* Test 17: revocable==0 for root-task (pd_index==0) caps */
static void test_root_caps_not_revocable(void)
{
    cap_audit_test_reset();
    sel4_msg_t req = make_req_u32(OP_CAP_AUDIT, 0u);
    sel4_msg_t rep;
    handle_cap_audit(ctrl_badge(), &req, &rep, (void *)0);
    /* First 3 audit entries correspond to stub entries 0,1,2 (pd_index==0) */
    const cap_audit_entry_t *e0 = cap_audit_test_get_entry(0u);
    const cap_audit_entry_t *e1 = cap_audit_test_get_entry(1u);
    const cap_audit_entry_t *e2 = cap_audit_test_get_entry(2u);
    ASSERT_TRUE(e0 && e0->revocable == 0u &&
                e1 && e1->revocable == 0u &&
                e2 && e2->revocable == 0u,
                "OP_CAP_AUDIT: pd_index==0 entries have revocable==0");
}

/* Test 18: revocable==1 for non-root PD caps */
static void test_pd_caps_revocable(void)
{
    cap_audit_test_reset();
    sel4_msg_t req = make_req_u32(OP_CAP_AUDIT, 0u);
    sel4_msg_t rep;
    handle_cap_audit(ctrl_badge(), &req, &rep, (void *)0);
    /* Entry index 3 in audit buffer corresponds to stub[3] (ns-tcb, pd_index==1) */
    const cap_audit_entry_t *e3 = cap_audit_test_get_entry(3u);
    ASSERT_TRUE(e3 && e3->revocable == 1u,
                "OP_CAP_AUDIT: pd_index>0 entries have revocable==1");
}

/* Test 19: sequential calls return same count */
static void test_sequential_calls_same_count(void)
{
    cap_audit_test_reset();
    sel4_msg_t req = make_req_u32(OP_CAP_AUDIT, 0u);
    sel4_msg_t rep1, rep2;
    handle_cap_audit(ctrl_badge(), &req, &rep1, (void *)0);
    cap_audit_test_reset();
    handle_cap_audit(ctrl_badge(), &req, &rep2, (void *)0);
    ASSERT_EQ(rep_count(&rep1), (uint64_t)rep_count(&rep2),
              "OP_CAP_AUDIT: sequential calls return same count");
}

/* Test 20: cap_tree_verify_all_pds completes without crash */
static void test_verify_all_pds_no_crash(void)
{
    cap_tree_verify_all_pds();
    TAP_OK("cap_tree_verify_all_pds: completes without crash");
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    TAP_PLAN(20);

    test_entry_size();
    test_non_ctrl_badge_denied();
    test_ctrl_badge_ok();
    test_audit_all_count_equals_total();
    test_audit_all_nonzero();
    test_audit_unused_pd_zero_count();
    test_entry0_pd_id();
    test_entry0_cslot();
    test_entry0_name_nonempty();
    test_rep_count_little_endian();
    test_guest_non_ctrl_denied();
    test_guest_invalid_handle();
    test_guest_out_of_range();
    test_guest_valid_handle_ok();
    test_guest_count_in_rep();
    test_reset_zeroes_buffer();
    test_root_caps_not_revocable();
    test_pd_caps_revocable();
    test_sequential_calls_same_count();
    test_verify_all_pds_no_crash();

    return tap_exit();
}

#else /* !AGENTOS_TEST_HOST */
typedef int _agentos_cap_audit_test_dummy;
#endif /* AGENTOS_TEST_HOST */
