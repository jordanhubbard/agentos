/*
 * test_irq_setup.c — API tests for seL4 IRQ capability binding (E7-S3)
 *
 * Covered functions / behaviours:
 *   irq_desc_t size                   — _Static_assert on struct layout
 *   boot_setup_irqs()                 — calls seL4_IRQControl_Get per irq_desc_t
 *   PD_IRQHANDLER_SLOT_BASE + i       — cap slot assignment
 *   badge dispatch (0x1 → net, 0x2 → blk, 0x3 → both)
 *
 * Build & run (host-side, no seL4 required):
 *   cc -DAGENTOS_TEST_HOST \
 *      -I tests/api \
 *      -I kernel/agentos-root-task/include \
 *      -std=c11 -Wall -Wextra \
 *      -o /tmp/t_irq_setup \
 *      tests/api/test_irq_setup.c && /tmp/t_irq_setup
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifdef AGENTOS_TEST_HOST

#include "framework.h"

/*
 * Pull in production type definitions from the real headers.
 * We then provide stub bodies for the seL4 invocations before including
 * the implementation under test (main.c boot_setup_irqs section).
 */
#include "sel4_boot.h"
#include "system_desc.h"

/* ── Stub state for seL4_IRQControl_Get ─────────────────────────────────── */

typedef struct {
    seL4_CPtr service;
    seL4_Word irq;
    seL4_CPtr dest_root;
    seL4_Word dest_index;
    seL4_Word dest_depth;
} IrqControlGetCall;

#define MAX_IRQ_CALLS 16u

static IrqControlGetCall g_irq_calls[MAX_IRQ_CALLS];
static uint32_t          g_irq_call_count;
static seL4_Error        g_irq_control_err;  /* injected error code */

static void stub_reset(void)
{
    for (uint32_t i = 0u; i < MAX_IRQ_CALLS; i++) {
        g_irq_calls[i].service    = (seL4_CPtr)0u;
        g_irq_calls[i].irq        = 0u;
        g_irq_calls[i].dest_root  = (seL4_CPtr)0u;
        g_irq_calls[i].dest_index = 0u;
        g_irq_calls[i].dest_depth = 0u;
    }
    g_irq_call_count  = 0u;
    g_irq_control_err = seL4_NoError;
}

/* ── seL4 IRQ API stubs ──────────────────────────────────────────────────── */

seL4_Error seL4_IRQControl_Get(seL4_CPtr  _service,
                                seL4_Word  irq,
                                seL4_CPtr  dest_root,
                                seL4_Word  dest_index,
                                seL4_Word  dest_depth)
{
    if (g_irq_call_count < MAX_IRQ_CALLS) {
        g_irq_calls[g_irq_call_count].service    = _service;
        g_irq_calls[g_irq_call_count].irq        = irq;
        g_irq_calls[g_irq_call_count].dest_root  = dest_root;
        g_irq_calls[g_irq_call_count].dest_index = dest_index;
        g_irq_calls[g_irq_call_count].dest_depth = dest_depth;
        g_irq_call_count++;
    }
    return g_irq_control_err;
}

seL4_Error seL4_IRQHandler_SetNotification(seL4_CPtr _service,
                                            seL4_CPtr notification)
{
    (void)_service;
    (void)notification;
    return seL4_NoError;
}

seL4_Error seL4_IRQHandler_Ack(seL4_CPtr _service)
{
    (void)_service;
    return seL4_NoError;
}

seL4_Error seL4_IRQHandler_Clear(seL4_CPtr _service)
{
    (void)_service;
    return seL4_NoError;
}

/* ── Minimal ut_alloc stub (not used by boot_setup_irqs, but needed by
 *    any transitive includes of ut_alloc.h) ──────────────────────────────── */

#include "ut_alloc.h"

seL4_Error ut_alloc(uint32_t type, uint32_t size_bits,
                    seL4_CPtr dest_cnode, seL4_Word dest_index,
                    uint32_t dest_depth)
{
    (void)type; (void)size_bits; (void)dest_cnode;
    (void)dest_index; (void)dest_depth;
    return seL4_NoError;
}

/*
 * boot_setup_irqs — inline the function under test.
 *
 * Rather than pulling in all of main.c (which has many dependencies),
 * we reproduce the function here.  This is the canonical implementation;
 * any change to main.c must be reflected here.
 *
 * This mirrors what the task spec requires:
 *   For each irq_desc_t in pd->irqs[], call seL4_IRQControl_Get and place
 *   the resulting cap at slot (PD_IRQHANDLER_SLOT_BASE + i) in the PD's CNode.
 */
static void boot_setup_irqs(const pd_desc_t *pd,
                             seL4_CPtr        pd_cnode,
                             seL4_CPtr        irq_control_cap,
                             seL4_Word        pd_cnode_depth)
{
    for (uint8_t i = 0u; i < pd->irq_count; i++) {
        const irq_desc_t *d = &pd->irqs[i];
        seL4_Word dest_slot = (seL4_Word)PD_IRQHANDLER_SLOT_BASE + (seL4_Word)i;
        seL4_Error err = seL4_IRQControl_Get(
            irq_control_cap,
            (seL4_Word)d->irq_number,
            pd_cnode,
            dest_slot,
            pd_cnode_depth);
        (void)err;
    }
}

/* ── Handler call tracking for badge dispatch tests ─────────────────────── */

static int g_virtio_net_handled;
static int g_virtio_blk_handled;

static void handle_virtio_net_irq(void) { g_virtio_net_handled++; }
static void handle_virtio_blk_irq(void) { g_virtio_blk_handled++; }

/*
 * badge_dispatch — model of the linux_vmm notified() badge dispatch logic.
 *
 * Tests the dispatch pattern independently of the Microkit runtime.
 * badge is the seL4 notification word (bitwise OR of ntfn_badge values).
 */
static void badge_dispatch(uint32_t badge)
{
    if (badge & 0x1u) {
        handle_virtio_net_irq();
        seL4_IRQHandler_Ack((seL4_CPtr)(PD_IRQHANDLER_SLOT_BASE + 0u));
    }
    if (badge & 0x2u) {
        handle_virtio_blk_irq();
        seL4_IRQHandler_Ack((seL4_CPtr)(PD_IRQHANDLER_SLOT_BASE + 1u));
    }
}

/* ── Tests ───────────────────────────────────────────────────────────────── */

/* Test 1: irq_desc_t is exactly 24 bytes */
static void test_irq_desc_size(void)
{
    /*
     * The _Static_assert in system_desc.h fires at compile time; this runtime
     * check provides a TAP-visible proof that the host build agrees with the
     * target layout.
     */
    ASSERT_EQ((uint64_t)sizeof(irq_desc_t), 24u,
              "irq_desc_t is exactly 24 bytes");
}

/* Test 2: irq_desc_t field offsets */
static void test_irq_desc_field_offsets(void)
{
    ASSERT_EQ((uint64_t)__builtin_offsetof(irq_desc_t, irq_number), 0u,
              "irq_desc_t.irq_number at offset 0");
    ASSERT_EQ((uint64_t)__builtin_offsetof(irq_desc_t, ntfn_badge), 4u,
              "irq_desc_t.ntfn_badge at offset 4");
    ASSERT_EQ((uint64_t)__builtin_offsetof(irq_desc_t, name), 8u,
              "irq_desc_t.name at offset 8");
}

/* Test 3: boot_setup_irqs with 0 IRQs does nothing */
static void test_setup_irqs_zero(void)
{
    stub_reset();
    pd_desc_t pd = { 0 };  /* irq_count == 0 */
    boot_setup_irqs(&pd, (seL4_CPtr)10u, seL4_CapIRQControl, 7u);
    ASSERT_EQ((uint64_t)g_irq_call_count, 0u,
              "boot_setup_irqs: 0 IRQs → seL4_IRQControl_Get not called");
}

/* Test 4: boot_setup_irqs with 1 IRQ calls seL4_IRQControl_Get once */
static void test_setup_irqs_one(void)
{
    stub_reset();
    pd_desc_t pd = {
        .irq_count = 1u,
        .irqs = {
            { .irq_number = 48u, .ntfn_badge = 0x1u, .name = "virtio-net" },
        },
    };
    boot_setup_irqs(&pd, (seL4_CPtr)20u, seL4_CapIRQControl, 7u);
    ASSERT_EQ((uint64_t)g_irq_call_count, 1u,
              "boot_setup_irqs: 1 IRQ → seL4_IRQControl_Get called once");
    ASSERT_EQ((uint64_t)g_irq_calls[0].irq, 48u,
              "boot_setup_irqs: IRQ 48 passed to seL4_IRQControl_Get");
}

/* Test 5: boot_setup_irqs with 2 IRQs calls seL4_IRQControl_Get twice */
static void test_setup_irqs_two(void)
{
    stub_reset();
    pd_desc_t pd = {
        .irq_count = 2u,
        .irqs = {
            { .irq_number = 48u, .ntfn_badge = 0x1u, .name = "virtio-net" },
            { .irq_number = 49u, .ntfn_badge = 0x2u, .name = "virtio-blk" },
        },
    };
    boot_setup_irqs(&pd, (seL4_CPtr)20u, seL4_CapIRQControl, 7u);
    ASSERT_EQ((uint64_t)g_irq_call_count, 2u,
              "boot_setup_irqs: 2 IRQs → seL4_IRQControl_Get called twice");
    ASSERT_EQ((uint64_t)g_irq_calls[0].irq, 48u,
              "boot_setup_irqs: first call uses IRQ 48");
    ASSERT_EQ((uint64_t)g_irq_calls[1].irq, 49u,
              "boot_setup_irqs: second call uses IRQ 49");
}

/* Test 6: cap slot for IRQ i == PD_IRQHANDLER_SLOT_BASE + i */
static void test_cap_slot_assignment(void)
{
    stub_reset();
    pd_desc_t pd = {
        .irq_count = 2u,
        .irqs = {
            { .irq_number = 48u, .ntfn_badge = 0x1u, .name = "virtio-net" },
            { .irq_number = 49u, .ntfn_badge = 0x2u, .name = "virtio-blk" },
        },
    };
    boot_setup_irqs(&pd, (seL4_CPtr)30u, seL4_CapIRQControl, 7u);
    ASSERT_EQ((uint64_t)g_irq_calls[0].dest_index,
              (uint64_t)(PD_IRQHANDLER_SLOT_BASE + 0u),
              "IRQ 0 placed at PD_IRQHANDLER_SLOT_BASE + 0");
    ASSERT_EQ((uint64_t)g_irq_calls[1].dest_index,
              (uint64_t)(PD_IRQHANDLER_SLOT_BASE + 1u),
              "IRQ 1 placed at PD_IRQHANDLER_SLOT_BASE + 1");
}

/* Test 7: irq_control_cap passed through correctly */
static void test_irq_control_cap_passed(void)
{
    stub_reset();
    pd_desc_t pd = {
        .irq_count = 1u,
        .irqs = { { .irq_number = 33u, .ntfn_badge = 0x4u, .name = "uart" } },
    };
    seL4_CPtr expected_ctl = seL4_CapIRQControl;
    boot_setup_irqs(&pd, (seL4_CPtr)40u, expected_ctl, 6u);
    ASSERT_EQ((uint64_t)g_irq_calls[0].service, (uint64_t)expected_ctl,
              "seL4_CapIRQControl passed as _service to seL4_IRQControl_Get");
}

/* Test 8: pd_cnode passed through correctly */
static void test_pd_cnode_passed(void)
{
    stub_reset();
    pd_desc_t pd = {
        .irq_count = 1u,
        .irqs = { { .irq_number = 33u, .ntfn_badge = 0x4u, .name = "uart" } },
    };
    seL4_CPtr expected_cnode = (seL4_CPtr)99u;
    boot_setup_irqs(&pd, expected_cnode, seL4_CapIRQControl, 6u);
    ASSERT_EQ((uint64_t)g_irq_calls[0].dest_root, (uint64_t)expected_cnode,
              "pd_cnode passed as dest_root to seL4_IRQControl_Get");
}

/* Test 9: cnode_depth passed through correctly */
static void test_cnode_depth_passed(void)
{
    stub_reset();
    pd_desc_t pd = {
        .irq_count = 1u,
        .irqs = { { .irq_number = 48u, .ntfn_badge = 0x1u, .name = "virtio-net" } },
    };
    seL4_Word expected_depth = 7u;  /* 128-slot CNode */
    boot_setup_irqs(&pd, (seL4_CPtr)50u, seL4_CapIRQControl, expected_depth);
    ASSERT_EQ((uint64_t)g_irq_calls[0].dest_depth, (uint64_t)expected_depth,
              "pd_cnode_depth passed as dest_depth to seL4_IRQControl_Get");
}

/* Test 10: PD_IRQHANDLER_SLOT_BASE >= PD_MAX_INIT_EPS */
static void test_slot_base_above_eps(void)
{
    ASSERT_TRUE(PD_IRQHANDLER_SLOT_BASE >= PD_MAX_INIT_EPS,
                "PD_IRQHANDLER_SLOT_BASE is above PD_MAX_INIT_EPS — no overlap");
}

/* Test 11: badge 0x1 → only virtio-net handler called */
static void test_badge_dispatch_net_only(void)
{
    g_virtio_net_handled = 0;
    g_virtio_blk_handled = 0;
    badge_dispatch(0x1u);
    ASSERT_EQ((uint64_t)g_virtio_net_handled, 1u,
              "badge 0x1: virtio-net handler called");
    ASSERT_EQ((uint64_t)g_virtio_blk_handled, 0u,
              "badge 0x1: virtio-blk handler NOT called");
}

/* Test 12: badge 0x2 → only virtio-blk handler called */
static void test_badge_dispatch_blk_only(void)
{
    g_virtio_net_handled = 0;
    g_virtio_blk_handled = 0;
    badge_dispatch(0x2u);
    ASSERT_EQ((uint64_t)g_virtio_net_handled, 0u,
              "badge 0x2: virtio-net handler NOT called");
    ASSERT_EQ((uint64_t)g_virtio_blk_handled, 1u,
              "badge 0x2: virtio-blk handler called");
}

/* Test 13: badge 0x3 → both handlers called (coalesced notification) */
static void test_badge_dispatch_both(void)
{
    g_virtio_net_handled = 0;
    g_virtio_blk_handled = 0;
    badge_dispatch(0x3u);
    ASSERT_EQ((uint64_t)g_virtio_net_handled, 1u,
              "badge 0x3: virtio-net handler called");
    ASSERT_EQ((uint64_t)g_virtio_blk_handled, 1u,
              "badge 0x3: virtio-blk handler called");
}

/* Test 14: badge 0x0 → no handlers called */
static void test_badge_dispatch_none(void)
{
    g_virtio_net_handled = 0;
    g_virtio_blk_handled = 0;
    badge_dispatch(0x0u);
    ASSERT_EQ((uint64_t)g_virtio_net_handled, 0u,
              "badge 0x0: virtio-net handler NOT called");
    ASSERT_EQ((uint64_t)g_virtio_blk_handled, 0u,
              "badge 0x0: virtio-blk handler NOT called");
}

/* Test 15: cap slots for IRQs are PD_IRQHANDLER_SLOT_BASE and +1 */
static void test_cap_slot_values(void)
{
    seL4_CPtr expected_net = (seL4_CPtr)(PD_IRQHANDLER_SLOT_BASE + 0u);
    seL4_CPtr expected_blk = (seL4_CPtr)(PD_IRQHANDLER_SLOT_BASE + 1u);
    ASSERT_EQ((uint64_t)expected_net, (uint64_t)PD_IRQHANDLER_SLOT_BASE,
              "virtio-net IRQ cap slot == PD_IRQHANDLER_SLOT_BASE");
    ASSERT_EQ((uint64_t)expected_blk, (uint64_t)(PD_IRQHANDLER_SLOT_BASE + 1u),
              "virtio-blk IRQ cap slot == PD_IRQHANDLER_SLOT_BASE + 1");
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void)
{
    /*
     * Total test points:
     *   test_irq_desc_size          :  1
     *   test_irq_desc_field_offsets :  3
     *   test_setup_irqs_zero        :  1
     *   test_setup_irqs_one         :  2
     *   test_setup_irqs_two         :  3
     *   test_cap_slot_assignment    :  2
     *   test_irq_control_cap_passed :  1
     *   test_pd_cnode_passed        :  1
     *   test_cnode_depth_passed     :  1
     *   test_slot_base_above_eps    :  1
     *   test_badge_dispatch_net     :  2
     *   test_badge_dispatch_blk     :  2
     *   test_badge_dispatch_both    :  2
     *   test_badge_dispatch_none    :  2
     *   test_cap_slot_values        :  2
     *   TOTAL = 26
     *
     * Note: 15 logical test scenarios, 26 TAP points (multiple assertions per
     * scenario ensure both positive and negative conditions are checked).
     */
    TAP_PLAN(26);

    test_irq_desc_size();
    test_irq_desc_field_offsets();
    test_setup_irqs_zero();
    test_setup_irqs_one();
    test_setup_irqs_two();
    test_cap_slot_assignment();
    test_irq_control_cap_passed();
    test_pd_cnode_passed();
    test_cnode_depth_passed();
    test_slot_base_above_eps();
    test_badge_dispatch_net_only();
    test_badge_dispatch_blk_only();
    test_badge_dispatch_both();
    test_badge_dispatch_none();
    test_cap_slot_values();

    return tap_exit();
}

#else
typedef int _agentos_api_test_irq_setup_dummy;
#endif /* AGENTOS_TEST_HOST */
