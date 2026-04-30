/*
 * test_ut_alloc.c — API tests for the root-task untyped allocator
 */

#ifdef AGENTOS_TEST_HOST

#include "framework.h"
#include "ut_alloc.h"

typedef struct {
    seL4_CPtr service;
    seL4_Word type;
    seL4_Word size_bits;
    seL4_CPtr root;
    seL4_Word node_index;
    seL4_Word node_depth;
    seL4_Word node_offset;
    seL4_Word num_objects;
} RetypeCall;

static RetypeCall g_retype_calls[16];
static uint32_t g_retype_count;
static seL4_Error g_retype_results[16];
static uint32_t g_retype_result_count;
static seL4_Error g_default_retype_result;

static void retype_reset(void)
{
    memset(g_retype_calls, 0, sizeof(g_retype_calls));
    memset(g_retype_results, 0, sizeof(g_retype_results));
    g_retype_count = 0;
    g_retype_result_count = 0;
    g_default_retype_result = seL4_NoError;
}

static void retype_push_result(seL4_Error err)
{
    if (g_retype_result_count < 16u) {
        g_retype_results[g_retype_result_count++] = err;
    }
}

seL4_Error seL4_Untyped_Retype(seL4_CPtr service,
                                seL4_Word type,
                                seL4_Word size_bits,
                                seL4_CPtr root,
                                seL4_Word node_index,
                                seL4_Word node_depth,
                                seL4_Word node_offset,
                                seL4_Word num_objects)
{
    if (g_retype_count < 16u) {
        g_retype_calls[g_retype_count] = (RetypeCall){
            .service = service,
            .type = type,
            .size_bits = size_bits,
            .root = root,
            .node_index = node_index,
            .node_depth = node_depth,
            .node_offset = node_offset,
            .num_objects = num_objects,
        };
    }

    uint32_t idx = g_retype_count++;
    if (idx < g_retype_result_count) {
        return g_retype_results[idx];
    }
    return g_default_retype_result;
}

#include "../../kernel/agentos-root-task/src/ut_alloc.c"

static seL4_BootInfo make_bi(seL4_SlotPos empty_start,
                              seL4_SlotPos empty_end,
                              seL4_SlotPos untyped_start,
                              uint32_t untyped_count)
{
    seL4_BootInfo bi;
    memset(&bi, 0, sizeof(bi));
    bi.empty.start = empty_start;
    bi.empty.end = empty_end;
    bi.untyped.start = untyped_start;
    bi.untyped.end = untyped_start + untyped_count;
    return bi;
}

static void test_init_null(void)
{
    retype_reset();
    ut_alloc_init((const seL4_BootInfo *)0);

    ASSERT_EQ(ut_free_slot_base(), 0u, "ut_alloc_init: NULL BootInfo clears slot base");
    ASSERT_EQ(ut_alloc_slot(), seL4_CapNull, "ut_alloc_slot: NULL BootInfo has no slots");
    ASSERT_EQ(ut_alloc(seL4_TCBObject, 0u, 2u, 10u, 64u),
              seL4_NotEnoughMemory, "ut_alloc: NULL BootInfo has no untypeds");
}

static void test_slot_cursor(void)
{
    seL4_BootInfo bi = make_bi(100u, 102u, 200u, 0u);
    ut_alloc_init(&bi);

    ASSERT_EQ(ut_free_slot_base(), 100u, "ut_free_slot_base: returns BootInfo empty.start");
    ASSERT_EQ(ut_alloc_slot(), 100u, "ut_alloc_slot: returns first free slot");
    ASSERT_EQ(ut_alloc_slot(), 101u, "ut_alloc_slot: advances cursor");
    ASSERT_EQ(ut_alloc_slot(), seL4_CapNull, "ut_alloc_slot: returns null when exhausted");
}

static void test_advance_slot_cursor_clamps(void)
{
    seL4_BootInfo bi = make_bi(20u, 25u, 200u, 0u);
    ut_alloc_init(&bi);
    ut_advance_slot_cursor(99u);

    ASSERT_EQ(ut_free_slot_base(), 20u, "ut_advance_slot_cursor: leaves base unchanged");
    ASSERT_EQ(ut_alloc_slot(), seL4_CapNull, "ut_advance_slot_cursor: clamps at empty.end");
}

static void test_alloc_uses_first_normal_untyped(void)
{
    retype_reset();
    seL4_BootInfo bi = make_bi(100u, 110u, 300u, 1u);
    bi.untypedList[0].sizeBits = 12u;
    bi.untypedList[0].isDevice = 0u;
    ut_alloc_init(&bi);

    seL4_Error err = ut_alloc(seL4_TCBObject, 0u, 77u, 88u, 64u);

    ASSERT_EQ(err, seL4_NoError, "ut_alloc: returns seL4_NoError on successful retype");
    ASSERT_EQ(g_retype_count, 1u, "ut_alloc: calls seL4_Untyped_Retype once");
    ASSERT_EQ(g_retype_calls[0].service, 300u, "ut_alloc: uses first normal untyped cap");
    ASSERT_EQ(g_retype_calls[0].type, seL4_TCBObject, "ut_alloc: passes object type");
    ASSERT_EQ(g_retype_calls[0].size_bits, 0u, "ut_alloc: passes size_bits");
    ASSERT_EQ(g_retype_calls[0].root, 77u, "ut_alloc: passes destination CNode");
    ASSERT_EQ(g_retype_calls[0].node_offset, 88u, "ut_alloc: passes destination slot");
    ASSERT_EQ(g_retype_calls[0].num_objects, 1u, "ut_alloc: retypes one object");
}

static void test_alloc_skips_device_untyped(void)
{
    retype_reset();
    seL4_BootInfo bi = make_bi(100u, 110u, 400u, 2u);
    bi.untypedList[0].sizeBits = 12u;
    bi.untypedList[0].isDevice = 1u;
    bi.untypedList[0].paddr = 0x10000000u;
    bi.untypedList[1].sizeBits = 12u;
    bi.untypedList[1].isDevice = 0u;
    ut_alloc_init(&bi);

    ASSERT_EQ(ut_alloc(seL4_TCBObject, 0u, 2u, 9u, 64u), seL4_NoError,
              "ut_alloc: succeeds with normal untyped after device entry");
    ASSERT_EQ(g_retype_calls[0].service, 401u,
              "ut_alloc: skips device untyped caps for normal allocation");
}

static void test_alloc_retries_after_nomem(void)
{
    retype_reset();
    retype_push_result(seL4_NotEnoughMemory);
    retype_push_result(seL4_NoError);

    seL4_BootInfo bi = make_bi(100u, 110u, 500u, 2u);
    bi.untypedList[0].sizeBits = 12u;
    bi.untypedList[1].sizeBits = 12u;
    ut_alloc_init(&bi);

    ASSERT_EQ(ut_alloc(seL4_TCBObject, 0u, 2u, 9u, 64u), seL4_NoError,
              "ut_alloc: retries after seL4_NotEnoughMemory");
    ASSERT_EQ(g_retype_count, 2u, "ut_alloc: attempted two untyped caps");
    ASSERT_EQ(g_retype_calls[0].service, 500u, "ut_alloc: first attempt uses first cap");
    ASSERT_EQ(g_retype_calls[1].service, 501u, "ut_alloc: retry uses next cap");
}

static void test_alloc_stops_on_invalid(void)
{
    retype_reset();
    retype_push_result(seL4_InvalidArgument);

    seL4_BootInfo bi = make_bi(100u, 110u, 600u, 2u);
    bi.untypedList[0].sizeBits = 12u;
    bi.untypedList[1].sizeBits = 12u;
    ut_alloc_init(&bi);

    ASSERT_EQ(ut_alloc(seL4_TCBObject, 0u, 2u, 9u, 64u), seL4_InvalidArgument,
              "ut_alloc: propagates non-memory retype error");
    ASSERT_EQ(g_retype_count, 1u, "ut_alloc: does not retry non-memory errors");
    ASSERT_EQ(g_retype_calls[0].service, 600u, "ut_alloc: first cap produced error");
}

static void test_alloc_cap_success(void)
{
    retype_reset();
    seL4_BootInfo bi = make_bi(70u, 72u, 700u, 1u);
    bi.untypedList[0].sizeBits = 12u;
    ut_alloc_init(&bi);

    seL4_CPtr cap = seL4_CapNull;
    seL4_Error err = ut_alloc_cap(seL4_EndpointObject, 0u, &cap);

    ASSERT_EQ(err, seL4_NoError, "ut_alloc_cap: returns success");
    ASSERT_EQ(cap, 70u, "ut_alloc_cap: returns allocated slot as cap");
    ASSERT_EQ(g_retype_calls[0].node_offset, 70u, "ut_alloc_cap: retypes into allocated slot");
    ASSERT_EQ(g_retype_calls[0].root, seL4_CapInitThreadCNode,
              "ut_alloc_cap: uses root task CNode");
    ASSERT_EQ(g_retype_calls[0].type, seL4_EndpointObject,
              "ut_alloc_cap: passes requested object type");
}

static void test_alloc_cap_slot_exhausted(void)
{
    retype_reset();
    seL4_BootInfo bi = make_bi(80u, 80u, 800u, 1u);
    bi.untypedList[0].sizeBits = 12u;
    ut_alloc_init(&bi);

    seL4_CPtr cap = 123u;
    ASSERT_EQ(ut_alloc_cap(seL4_TCBObject, 0u, &cap), seL4_NotEnoughMemory,
              "ut_alloc_cap: reports slot exhaustion");
    ASSERT_EQ(cap, seL4_CapNull, "ut_alloc_cap: clears cap_out on slot exhaustion");
    ASSERT_EQ(g_retype_count, 0u, "ut_alloc_cap: does not retype without a slot");
}

static void test_device_frame_success(void)
{
    retype_reset();
    seL4_BootInfo bi = make_bi(90u, 95u, 900u, 1u);
    bi.untypedList[0].isDevice = 1u;
    bi.untypedList[0].paddr = 0x10000000u;
    bi.untypedList[0].sizeBits = 12u;
    ut_alloc_init(&bi);

    seL4_Error err = ut_alloc_device_frame(0x10000000u, 44u, 12u);

    ASSERT_EQ(err, seL4_NoError, "ut_alloc_device_frame: maps covered device paddr");
    ASSERT_EQ(g_retype_calls[0].service, 900u, "ut_alloc_device_frame: uses covering device untyped");
    ASSERT_EQ(g_retype_calls[0].type, seL4_ARM_SmallPageObject,
              "ut_alloc_device_frame: retypes a small page");
    ASSERT_EQ(g_retype_calls[0].root, seL4_CapInitThreadCNode,
              "ut_alloc_device_frame: traverses from root CNode");
    ASSERT_EQ(g_retype_calls[0].node_index, 44u,
              "ut_alloc_device_frame: traverses into PD CNode");
    ASSERT_EQ(g_retype_calls[0].node_depth, 64u,
              "ut_alloc_device_frame: uses full-depth CSpace traversal");
    ASSERT_EQ(g_retype_calls[0].node_offset, 12u,
              "ut_alloc_device_frame: places frame in target slot");
}

static void test_device_frame_not_found(void)
{
    retype_reset();
    seL4_BootInfo bi = make_bi(90u, 95u, 1000u, 1u);
    bi.untypedList[0].isDevice = 1u;
    bi.untypedList[0].paddr = 0x20000000u;
    bi.untypedList[0].sizeBits = 12u;
    ut_alloc_init(&bi);

    ASSERT_EQ(ut_alloc_device_frame(0x10000000u, 44u, 12u), seL4_InvalidArgument,
              "ut_alloc_device_frame: rejects uncovered paddr");
    ASSERT_EQ(g_retype_count, 0u, "ut_alloc_device_frame: does not retype uncovered paddr");
}

static void test_device_cap_typed_validation(void)
{
    retype_reset();
    seL4_BootInfo bi = make_bi(90u, 95u, 1100u, 1u);
    bi.untypedList[0].isDevice = 1u;
    bi.untypedList[0].paddr = 0x30000000u;
    bi.untypedList[0].sizeBits = 21u;
    ut_alloc_init(&bi);

    ASSERT_EQ(ut_alloc_device_cap_typed(0x30000000u, seL4_ARM_LargePageObject,
                                        seL4_ARCH_LargePageBits,
                                        (seL4_CPtr *)0),
              seL4_InvalidArgument,
              "ut_alloc_device_cap_typed: rejects NULL cap_out");

    seL4_CPtr cap = 123u;
    ASSERT_EQ(ut_alloc_device_cap_typed(0x30001000u, seL4_ARM_LargePageObject,
                                        seL4_ARCH_LargePageBits, &cap),
              seL4_InvalidArgument,
              "ut_alloc_device_cap_typed: rejects unaligned paddr");
    ASSERT_EQ(cap, seL4_CapNull,
              "ut_alloc_device_cap_typed: clears cap_out before validation");
}

int main(void)
{
    TAP_PLAN(46);

    test_init_null();
    test_slot_cursor();
    test_advance_slot_cursor_clamps();
    test_alloc_uses_first_normal_untyped();
    test_alloc_skips_device_untyped();
    test_alloc_retries_after_nomem();
    test_alloc_stops_on_invalid();
    test_alloc_cap_success();
    test_alloc_cap_slot_exhausted();
    test_device_frame_success();
    test_device_frame_not_found();
    test_device_cap_typed_validation();

    return tap_exit();
}

#else
int main(void) { return 0; }
#endif
