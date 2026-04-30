/*
 * test_ep_alloc.c — API tests for the root-task endpoint allocator
 */

#ifdef AGENTOS_TEST_HOST

#include "framework.h"
#include "ep_alloc.h"

typedef struct {
    uint32_t type;
    uint32_t size_bits;
    seL4_CPtr dest_cnode;
    seL4_Word dest_index;
    uint32_t dest_depth;
} UtAllocCall;

typedef struct {
    seL4_CPtr service;
    seL4_Word dest_index;
    uint8_t dest_depth;
    seL4_CPtr src_root;
    seL4_Word src_index;
    uint8_t src_depth;
    seL4_Word rights;
    seL4_Word badge;
} MintCall;

static UtAllocCall g_ut_calls[128];
static uint32_t g_ut_count;
static seL4_Error g_ut_result;
static MintCall g_mint;
static uint32_t g_mint_count;
static seL4_Error g_mint_result;

static void stubs_reset(void)
{
    memset(g_ut_calls, 0, sizeof(g_ut_calls));
    memset(&g_mint, 0, sizeof(g_mint));
    g_ut_count = 0;
    g_ut_result = seL4_NoError;
    g_mint_count = 0;
    g_mint_result = seL4_NoError;
}

seL4_Error ut_alloc(uint32_t type,
                    uint32_t size_bits,
                    seL4_CPtr dest_cnode,
                    seL4_Word dest_index,
                    uint32_t dest_depth)
{
    if (g_ut_count < 128u) {
        g_ut_calls[g_ut_count] = (UtAllocCall){
            .type = type,
            .size_bits = size_bits,
            .dest_cnode = dest_cnode,
            .dest_index = dest_index,
            .dest_depth = dest_depth,
        };
    }
    g_ut_count++;
    return g_ut_result;
}

seL4_Error seL4_CNode_Mint(seL4_CPtr service,
                            seL4_Word dest_index,
                            uint8_t dest_depth,
                            seL4_CPtr src_root,
                            seL4_Word src_index,
                            uint8_t src_depth,
                            seL4_Word rights,
                            seL4_Word badge)
{
    g_mint = (MintCall){
        .service = service,
        .dest_index = dest_index,
        .dest_depth = dest_depth,
        .src_root = src_root,
        .src_index = src_index,
        .src_depth = src_depth,
        .rights = rights,
        .badge = badge,
    };
    g_mint_count++;
    return g_mint_result;
}

#include "../../kernel/agentos-root-task/src/ep_alloc.c"

static void test_alloc_first_endpoint(void)
{
    stubs_reset();
    ep_alloc_init(11u, 50u, 4u);

    seL4_CPtr ep = ep_alloc();

    ASSERT_EQ(ep, 50u, "ep_alloc: returns first pool slot");
    ASSERT_EQ(g_ut_count, 1u, "ep_alloc: calls ut_alloc once");
    ASSERT_EQ(g_ut_calls[0].type, seL4_EndpointObject,
              "ep_alloc: allocates endpoint object");
    ASSERT_EQ(g_ut_calls[0].size_bits, 0u, "ep_alloc: uses fixed object size_bits");
    ASSERT_EQ(g_ut_calls[0].dest_cnode, 11u, "ep_alloc: uses configured root CNode");
    ASSERT_EQ(g_ut_calls[0].dest_depth, 64u, "ep_alloc: uses full destination depth");
}

static void test_alloc_increments_and_exhausts(void)
{
    stubs_reset();
    ep_alloc_init(11u, 60u, 2u);

    ASSERT_EQ(ep_alloc(), 60u, "ep_alloc: first allocation uses base slot");
    ASSERT_EQ(ep_alloc(), 61u, "ep_alloc: second allocation advances slot");
    ASSERT_EQ(ep_alloc(), seL4_CapNull, "ep_alloc: returns null when pool exhausted");
    ASSERT_EQ(g_ut_count, 2u, "ep_alloc: does not call ut_alloc after exhaustion");
}

static void test_alloc_failure_preserves_slot(void)
{
    stubs_reset();
    ep_alloc_init(11u, 70u, 2u);

    g_ut_result = seL4_NotEnoughMemory;
    ASSERT_EQ(ep_alloc(), seL4_CapNull, "ep_alloc: returns null on ut_alloc failure");

    g_ut_result = seL4_NoError;
    ASSERT_EQ(ep_alloc(), 70u, "ep_alloc: retries failed slot on later success");
    ASSERT_EQ(g_ut_count, 2u, "ep_alloc: called ut_alloc for failed and retried allocation");
    ASSERT_EQ(g_ut_calls[1].dest_index, 70u, "ep_alloc: retry uses same slot");
}

static void test_mint_badge_args(void)
{
    stubs_reset();
    ep_alloc_init(99u, 80u, 1u);

    ASSERT_EQ(ep_mint_badge(80u, 0xABCDu, 22u, 33u, 6u), seL4_NoError,
              "ep_mint_badge: returns CNode_Mint result");
    ASSERT_EQ(g_mint_count, 1u, "ep_mint_badge: calls CNode_Mint once");
    ASSERT_EQ(g_mint.service, 22u, "ep_mint_badge: destination CNode");
    ASSERT_EQ(g_mint.dest_index, 33u, "ep_mint_badge: destination slot");
    ASSERT_EQ(g_mint.dest_depth, 6u, "ep_mint_badge: destination depth");
    ASSERT_EQ(g_mint.src_root, 99u, "ep_mint_badge: source root is allocator CNode");
    ASSERT_EQ(g_mint.src_index, 80u, "ep_mint_badge: source endpoint cap");
    ASSERT_EQ(g_mint.badge, 0xABCDu, "ep_mint_badge: propagates badge");
}

static void test_service_cache(void)
{
    stubs_reset();
    ep_alloc_init(11u, 90u, 4u);

    ASSERT_EQ(ep_find_by_service_id(123u), seL4_CapNull,
              "ep_find_by_service_id: unknown service returns null");

    seL4_CPtr first = ep_alloc_for_service(123u);
    seL4_CPtr second = ep_alloc_for_service(123u);
    seL4_CPtr other = ep_alloc_for_service(124u);

    ASSERT_EQ(first, 90u, "ep_alloc_for_service: allocates service endpoint");
    ASSERT_EQ(second, first, "ep_alloc_for_service: returns cached endpoint");
    ASSERT_EQ(other, 91u, "ep_alloc_for_service: different service gets next endpoint");
    ASSERT_EQ(g_ut_count, 2u, "ep_alloc_for_service: cached lookup avoids allocation");
    ASSERT_EQ(ep_find_by_service_id(123u), first,
              "ep_find_by_service_id: returns registered endpoint");
}

int main(void)
{
    TAP_PLAN(28);

    test_alloc_first_endpoint();
    test_alloc_increments_and_exhausts();
    test_alloc_failure_preserves_slot();
    test_mint_badge_args();
    test_service_cache();

    return tap_exit();
}

#else
int main(void) { return 0; }
#endif
