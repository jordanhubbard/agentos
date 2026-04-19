/*
 * agentOS Cap-Policy Ring-0 Service Registry — Unit Tests
 *
 * Tests the non-reinvention enforcement registry:
 *   cap_policy_register_ring0_service()   — register service for a function class
 *   cap_policy_find_ring0_service()       — query for existing service
 *   cap_policy_unregister_ring0_service() — remove entry
 *
 * Build:  cc -o /tmp/test_cap_policy_ring0_registry \
 *             tests/test_cap_policy_ring0_registry.c \
 *             -I tests \
 *             -I kernel/agentos-root-task/include \
 *             -DAGENTOS_TEST_HOST
 * Run:    /tmp/test_cap_policy_ring0_registry
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

/* ── Function class constants (mirrors cap_policy.h) ────────────────── */
#define CAP_POLICY_FUNC_CLASS_SERIAL   0x01u
#define CAP_POLICY_FUNC_CLASS_NET      0x02u
#define CAP_POLICY_FUNC_CLASS_BLOCK    0x03u
#define CAP_POLICY_FUNC_CLASS_USB      0x04u
#define CAP_POLICY_FUNC_CLASS_FB       0x05u
#define CAP_POLICY_FUNC_CLASS_MAX      0x05u

/* ── Inline copy of the ring-0 service registry from cap_policy.c ───── */

typedef struct {
    uint32_t pd_handle;
    uint32_t channel_id;
    bool     registered;
} ring0_svc_entry_t;

static ring0_svc_entry_t g_ring0_registry[CAP_POLICY_FUNC_CLASS_MAX + 1];

static int cap_policy_register_ring0_service(uint32_t func_class, uint32_t pd_handle, uint32_t channel_id)
{
    if (func_class < 1 || func_class > CAP_POLICY_FUNC_CLASS_MAX)
        return -1;
    if (g_ring0_registry[func_class].registered)
        return -1;
    g_ring0_registry[func_class].pd_handle  = pd_handle;
    g_ring0_registry[func_class].channel_id = channel_id;
    g_ring0_registry[func_class].registered = true;
    return 0;
}

static int cap_policy_find_ring0_service(uint32_t func_class, uint32_t *out_pd_handle, uint32_t *out_channel_id)
{
    if (func_class < 1 || func_class > CAP_POLICY_FUNC_CLASS_MAX)
        return 0;
    if (!g_ring0_registry[func_class].registered)
        return 0;
    if (out_pd_handle)  *out_pd_handle  = g_ring0_registry[func_class].pd_handle;
    if (out_channel_id) *out_channel_id = g_ring0_registry[func_class].channel_id;
    return 1;
}

static void cap_policy_unregister_ring0_service(uint32_t func_class)
{
    if (func_class < 1 || func_class > CAP_POLICY_FUNC_CLASS_MAX)
        return;
    g_ring0_registry[func_class].pd_handle  = 0;
    g_ring0_registry[func_class].channel_id = 0;
    g_ring0_registry[func_class].registered = false;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test framework
 * ══════════════════════════════════════════════════════════════════════════ */

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name)      printf("\n=== TEST: %s ===\n", (name))
#define ASSERT_TRUE(expr, msg)  do { \
    if (!(expr)) { printf("  FAIL: %s\n", (msg)); tests_failed++; } \
    else         { printf("  PASS: %s\n", (msg)); tests_passed++; } \
} while(0)
#define ASSERT_EQ(a, b, msg)  do { \
    if ((int64_t)(a) != (int64_t)(b)) { \
        printf("  FAIL: %s (got %lld expected %lld)\n", (msg), \
               (long long)(a), (long long)(b)); tests_failed++; \
    } else { printf("  PASS: %s\n", (msg)); tests_passed++; } \
} while(0)

/* ══════════════════════════════════════════════════════════════════════════
 * Tests: register
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_register_valid_classes(void)
{
    TEST("register_valid_classes");

    ASSERT_EQ(cap_policy_register_ring0_service(CAP_POLICY_FUNC_CLASS_SERIAL, 0x10, 67), 0,
              "register serial: ok");
    ASSERT_EQ(cap_policy_register_ring0_service(CAP_POLICY_FUNC_CLASS_NET, 0x20, 68), 0,
              "register net: ok");
    ASSERT_EQ(cap_policy_register_ring0_service(CAP_POLICY_FUNC_CLASS_BLOCK, 0x30, 69), 0,
              "register block: ok");
    ASSERT_EQ(cap_policy_register_ring0_service(CAP_POLICY_FUNC_CLASS_USB, 0x40, 70), 0,
              "register usb: ok");
    ASSERT_EQ(cap_policy_register_ring0_service(CAP_POLICY_FUNC_CLASS_FB, 0x50, 71), 0,
              "register fb: ok");

    cap_policy_unregister_ring0_service(CAP_POLICY_FUNC_CLASS_SERIAL);
    cap_policy_unregister_ring0_service(CAP_POLICY_FUNC_CLASS_NET);
    cap_policy_unregister_ring0_service(CAP_POLICY_FUNC_CLASS_BLOCK);
    cap_policy_unregister_ring0_service(CAP_POLICY_FUNC_CLASS_USB);
    cap_policy_unregister_ring0_service(CAP_POLICY_FUNC_CLASS_FB);
}

static void test_register_out_of_range(void)
{
    TEST("register_out_of_range_rejected");

    ASSERT_EQ(cap_policy_register_ring0_service(0, 0x10, 1), -1,
              "func_class 0 is invalid");
    ASSERT_EQ(cap_policy_register_ring0_service(CAP_POLICY_FUNC_CLASS_MAX + 1, 0x10, 1), -1,
              "func_class > MAX is invalid");
    ASSERT_EQ(cap_policy_register_ring0_service(0xFF, 0x10, 1), -1,
              "func_class 0xFF is invalid");
}

static void test_register_duplicate_rejected(void)
{
    TEST("register_duplicate_rejected");

    ASSERT_EQ(cap_policy_register_ring0_service(CAP_POLICY_FUNC_CLASS_SERIAL, 0x10, 67), 0,
              "first registration: ok");
    ASSERT_EQ(cap_policy_register_ring0_service(CAP_POLICY_FUNC_CLASS_SERIAL, 0x11, 67), -1,
              "second registration for same class: REJECTED (non-reinvention)");

    cap_policy_unregister_ring0_service(CAP_POLICY_FUNC_CLASS_SERIAL);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Tests: find
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_find_registered_service(void)
{
    TEST("find_registered_service");

    cap_policy_register_ring0_service(CAP_POLICY_FUNC_CLASS_NET, 0xAB, 68);

    uint32_t pd = 0, ch = 0;
    ASSERT_EQ(cap_policy_find_ring0_service(CAP_POLICY_FUNC_CLASS_NET, &pd, &ch), 1,
              "find registered net service: found");
    ASSERT_EQ(pd, 0xAB, "pd_handle matches registered value");
    ASSERT_EQ(ch, 68,   "channel_id matches registered value");

    cap_policy_unregister_ring0_service(CAP_POLICY_FUNC_CLASS_NET);
}

static void test_find_unregistered_service(void)
{
    TEST("find_unregistered_service");

    uint32_t pd = 0xDEAD, ch = 0xDEAD;
    ASSERT_EQ(cap_policy_find_ring0_service(CAP_POLICY_FUNC_CLASS_SERIAL, &pd, &ch), 0,
              "find unregistered serial service: not found");
    ASSERT_EQ(pd, 0xDEAD, "pd_handle unchanged when not found");
    ASSERT_EQ(ch, 0xDEAD, "channel_id unchanged when not found");
}

static void test_find_null_out_pointers(void)
{
    TEST("find_null_out_pointers_tolerated");

    cap_policy_register_ring0_service(CAP_POLICY_FUNC_CLASS_USB, 0x77, 70);

    ASSERT_EQ(cap_policy_find_ring0_service(CAP_POLICY_FUNC_CLASS_USB, (void*)0, (void*)0), 1,
              "find with NULL out-pointers returns 1 (found) without crash");

    cap_policy_unregister_ring0_service(CAP_POLICY_FUNC_CLASS_USB);
}

static void test_find_out_of_range(void)
{
    TEST("find_out_of_range");

    uint32_t pd = 0, ch = 0;
    ASSERT_EQ(cap_policy_find_ring0_service(0, &pd, &ch), 0,
              "func_class 0: not found");
    ASSERT_EQ(cap_policy_find_ring0_service(0xFF, &pd, &ch), 0,
              "func_class 0xFF: not found");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Tests: unregister
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_unregister_then_reregister(void)
{
    TEST("unregister_then_reregister");

    ASSERT_EQ(cap_policy_register_ring0_service(CAP_POLICY_FUNC_CLASS_FB, 0xF0, 71), 0,
              "register fb: ok");

    uint32_t pd = 0, ch = 0;
    ASSERT_EQ(cap_policy_find_ring0_service(CAP_POLICY_FUNC_CLASS_FB, &pd, &ch), 1,
              "fb registered: found");

    cap_policy_unregister_ring0_service(CAP_POLICY_FUNC_CLASS_FB);

    ASSERT_EQ(cap_policy_find_ring0_service(CAP_POLICY_FUNC_CLASS_FB, &pd, &ch), 0,
              "after unregister: not found");

    ASSERT_EQ(cap_policy_register_ring0_service(CAP_POLICY_FUNC_CLASS_FB, 0xF1, 71), 0,
              "re-register after unregister: ok");

    ASSERT_EQ(cap_policy_find_ring0_service(CAP_POLICY_FUNC_CLASS_FB, &pd, &ch), 1,
              "re-registered fb: found");
    ASSERT_EQ(pd, 0xF1, "re-registered pd_handle correct");

    cap_policy_unregister_ring0_service(CAP_POLICY_FUNC_CLASS_FB);
}

static void test_unregister_not_registered_is_safe(void)
{
    TEST("unregister_not_registered_is_safe");

    cap_policy_unregister_ring0_service(CAP_POLICY_FUNC_CLASS_BLOCK);
    cap_policy_unregister_ring0_service(0);
    cap_policy_unregister_ring0_service(0xFF);

    ASSERT_TRUE(1, "unregister of unregistered entries does not crash");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Tests: non-reinvention enforcement scenario
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_non_reinvention_scenario(void)
{
    TEST("non_reinvention_scenario");

    /* serial_pd boots and registers itself */
    ASSERT_EQ(cap_policy_register_ring0_service(CAP_POLICY_FUNC_CLASS_SERIAL, 100, 67), 0,
              "serial_pd registers: ok");

    /* A VibeOS instance tries to bind a second serial PD — must be blocked */
    ASSERT_EQ(cap_policy_register_ring0_service(CAP_POLICY_FUNC_CLASS_SERIAL, 200, 99), -1,
              "second serial PD: REJECTED (non-reinvention)");

    /* Querying reveals the existing handle for reuse */
    uint32_t pd = 0, ch = 0;
    ASSERT_EQ(cap_policy_find_ring0_service(CAP_POLICY_FUNC_CLASS_SERIAL, &pd, &ch), 1,
              "query serial: existing service found");
    ASSERT_EQ(pd, 100, "existing serial pd_handle returned for reuse");
    ASSERT_EQ(ch, 67,  "existing serial channel returned");

    /* Other device classes remain unaffected */
    ASSERT_EQ(cap_policy_find_ring0_service(CAP_POLICY_FUNC_CLASS_NET, &pd, &ch), 0,
              "net service: still unregistered");
    ASSERT_EQ(cap_policy_find_ring0_service(CAP_POLICY_FUNC_CLASS_BLOCK, &pd, &ch), 0,
              "block service: still unregistered");

    cap_policy_unregister_ring0_service(CAP_POLICY_FUNC_CLASS_SERIAL);
}

static void test_all_classes_independent(void)
{
    TEST("all_classes_independent");

    /* Register all five classes */
    cap_policy_register_ring0_service(CAP_POLICY_FUNC_CLASS_SERIAL, 10, 67);
    cap_policy_register_ring0_service(CAP_POLICY_FUNC_CLASS_NET,    20, 68);
    cap_policy_register_ring0_service(CAP_POLICY_FUNC_CLASS_BLOCK,  30, 69);
    cap_policy_register_ring0_service(CAP_POLICY_FUNC_CLASS_USB,    40, 70);
    cap_policy_register_ring0_service(CAP_POLICY_FUNC_CLASS_FB,     50, 71);

    /* All must be independently findable with correct values */
    uint32_t pd, ch;
    cap_policy_find_ring0_service(CAP_POLICY_FUNC_CLASS_SERIAL, &pd, &ch);
    ASSERT_EQ(pd, 10, "serial pd_handle");
    cap_policy_find_ring0_service(CAP_POLICY_FUNC_CLASS_NET, &pd, &ch);
    ASSERT_EQ(pd, 20, "net pd_handle");
    cap_policy_find_ring0_service(CAP_POLICY_FUNC_CLASS_BLOCK, &pd, &ch);
    ASSERT_EQ(pd, 30, "block pd_handle");
    cap_policy_find_ring0_service(CAP_POLICY_FUNC_CLASS_USB, &pd, &ch);
    ASSERT_EQ(pd, 40, "usb pd_handle");
    cap_policy_find_ring0_service(CAP_POLICY_FUNC_CLASS_FB, &pd, &ch);
    ASSERT_EQ(pd, 50, "fb pd_handle");

    /* Unregister all */
    cap_policy_unregister_ring0_service(CAP_POLICY_FUNC_CLASS_SERIAL);
    cap_policy_unregister_ring0_service(CAP_POLICY_FUNC_CLASS_NET);
    cap_policy_unregister_ring0_service(CAP_POLICY_FUNC_CLASS_BLOCK);
    cap_policy_unregister_ring0_service(CAP_POLICY_FUNC_CLASS_USB);
    cap_policy_unregister_ring0_service(CAP_POLICY_FUNC_CLASS_FB);

    /* All must now be absent */
    ASSERT_EQ(cap_policy_find_ring0_service(CAP_POLICY_FUNC_CLASS_SERIAL, &pd, &ch), 0,
              "serial: not found after unregister");
    ASSERT_EQ(cap_policy_find_ring0_service(CAP_POLICY_FUNC_CLASS_FB, &pd, &ch), 0,
              "fb: not found after unregister");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Main
 * ══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    printf("╔════════════════════════════════════════════════════════╗\n");
    printf("║  agentOS Cap-Policy Ring-0 Registry — Test Suite       ║\n");
    printf("╚════════════════════════════════════════════════════════╝\n");

    test_register_valid_classes();
    test_register_out_of_range();
    test_register_duplicate_rejected();

    test_find_registered_service();
    test_find_unregistered_service();
    test_find_null_out_pointers();
    test_find_out_of_range();

    test_unregister_then_reregister();
    test_unregister_not_registered_is_safe();

    test_non_reinvention_scenario();
    test_all_classes_independent();

    printf("\n══════════════════════════════════════════════════════════\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    if (tests_failed > 0) {
        printf("FAIL\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
