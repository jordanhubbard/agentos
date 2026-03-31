/*
 * agentOS Quota PD — Unit Test
 *
 * Tests the quota enforcement logic by simulating register, tick, and
 * revocation scenarios. This is a host-side test that exercises the
 * quota table logic directly.
 *
 * Build:  cc -o test_quota tests/test_quota.c -I kernel/agentos-root-task/include
 * Run:    ./test_quota
 *
 * For the seL4 Microkit build (integration test), the quota_pd is wired
 * into the QEMU boot sequence and validated by scripts/run-tests.sh.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

/* ── Opcodes (must match quota_pd.c) ──────────────────────────────────────── */
#define OP_QUOTA_REGISTER  0x60
#define OP_QUOTA_TICK      0x61
#define OP_QUOTA_STATUS    0x62
#define OP_QUOTA_SET       0x63

/* ── Quota flags ──────────────────────────────────────────────────────────── */
#define QUOTA_FLAG_ACTIVE     (1 << 0)
#define QUOTA_FLAG_CPU_EXCEED (1 << 1)
#define QUOTA_FLAG_MEM_EXCEED (1 << 2)
#define QUOTA_FLAG_REVOKED    (1 << 3)

/* ── Quota table (mirror of quota_pd.c structs) ──────────────────────────── */
#define MAX_QUOTA_SLOTS  16

typedef struct {
    uint32_t agent_id;
    uint32_t cpu_limit_ms;
    uint32_t mem_limit_kb;
    uint32_t cpu_used_ms;
    uint32_t mem_used_kb;
    uint32_t flags;
    uint64_t tick_count;
    uint64_t exceed_tick;
} quota_entry_t;

/* Test harness state */
static quota_entry_t quota_table[MAX_QUOTA_SLOTS];
static uint32_t active_count = 0;
static uint32_t revoke_count = 0;
static uint32_t last_revoked_agent = 0;
static uint32_t last_revoke_reason = 0;

/* ── Simulated quota logic (mirrors quota_pd.c) ──────────────────────────── */

static int find_slot(uint32_t agent_id) {
    for (int i = 0; i < MAX_QUOTA_SLOTS; i++) {
        if ((quota_table[i].flags & QUOTA_FLAG_ACTIVE) &&
            quota_table[i].agent_id == agent_id) {
            return i;
        }
    }
    return -1;
}

static int find_free_slot(void) {
    for (int i = 0; i < MAX_QUOTA_SLOTS; i++) {
        if (!(quota_table[i].flags & QUOTA_FLAG_ACTIVE)) return i;
    }
    return -1;
}

static void sim_revoke_caps(uint32_t agent_id, uint32_t reason) {
    revoke_count++;
    last_revoked_agent = agent_id;
    last_revoke_reason = reason;
    printf("  [SIM] REVOKE agent=%u reason=0x%x\n", agent_id, reason);
}

static int sim_register(uint32_t agent_id, uint32_t cpu_ms, uint32_t mem_kb) {
    if (find_slot(agent_id) >= 0) return -2;  /* already exists */

    int slot = find_free_slot();
    if (slot < 0) return -1;  /* table full */

    quota_table[slot].agent_id     = agent_id;
    quota_table[slot].cpu_limit_ms = cpu_ms;
    quota_table[slot].mem_limit_kb = mem_kb;
    quota_table[slot].cpu_used_ms  = 0;
    quota_table[slot].mem_used_kb  = 0;
    quota_table[slot].flags        = QUOTA_FLAG_ACTIVE;
    quota_table[slot].tick_count   = 0;
    quota_table[slot].exceed_tick  = 0;
    active_count++;

    return slot;
}

static uint32_t sim_tick(uint32_t agent_id, uint32_t cpu_delta_ms, uint32_t mem_cur_kb) {
    int slot = find_slot(agent_id);
    if (slot < 0) return 0xE2;

    quota_entry_t *entry = &quota_table[slot];
    entry->cpu_used_ms += cpu_delta_ms;
    entry->mem_used_kb  = mem_cur_kb;
    entry->tick_count++;

    /* Already revoked */
    if (entry->flags & QUOTA_FLAG_REVOKED) return entry->flags;

    bool exceeded = false;
    uint32_t reason = 0;

    if (entry->cpu_limit_ms > 0 && entry->cpu_used_ms >= entry->cpu_limit_ms) {
        if (!(entry->flags & QUOTA_FLAG_CPU_EXCEED)) {
            entry->flags |= QUOTA_FLAG_CPU_EXCEED;
            exceeded = true;
            reason |= QUOTA_FLAG_CPU_EXCEED;
        }
    }
    if (entry->mem_limit_kb > 0 && entry->mem_used_kb >= entry->mem_limit_kb) {
        if (!(entry->flags & QUOTA_FLAG_MEM_EXCEED)) {
            entry->flags |= QUOTA_FLAG_MEM_EXCEED;
            exceeded = true;
            reason |= QUOTA_FLAG_MEM_EXCEED;
        }
    }
    if (exceeded) {
        entry->flags |= QUOTA_FLAG_REVOKED;
        entry->exceed_tick = entry->tick_count;
        sim_revoke_caps(entry->agent_id, reason);
    }

    return entry->flags;
}

/* ── Tests ────────────────────────────────────────────────────────────────── */

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    do { printf("\n=== TEST: %s ===\n", name); } while(0)

#define ASSERT_EQ(a, b, msg) do { \
    if ((a) != (b)) { \
        printf("  FAIL: %s (expected %u, got %u)\n", msg, (unsigned)(b), (unsigned)(a)); \
        tests_failed++; \
    } else { \
        printf("  PASS: %s\n", msg); \
        tests_passed++; \
    } \
} while(0)

#define ASSERT_TRUE(expr, msg) do { \
    if (!(expr)) { \
        printf("  FAIL: %s\n", msg); \
        tests_failed++; \
    } else { \
        printf("  PASS: %s\n", msg); \
        tests_passed++; \
    } \
} while(0)

static void reset_state(void) {
    memset(quota_table, 0, sizeof(quota_table));
    active_count = 0;
    revoke_count = 0;
    last_revoked_agent = 0;
    last_revoke_reason = 0;
}

static void test_register_basic(void) {
    TEST("register_basic");
    reset_state();

    int slot = sim_register(42, 1000, 512);
    ASSERT_TRUE(slot >= 0, "registration succeeds");
    ASSERT_EQ(quota_table[slot].agent_id, 42, "agent_id matches");
    ASSERT_EQ(quota_table[slot].cpu_limit_ms, 1000, "cpu_limit matches");
    ASSERT_EQ(quota_table[slot].mem_limit_kb, 512, "mem_limit matches");
    ASSERT_EQ(quota_table[slot].flags, QUOTA_FLAG_ACTIVE, "flags = ACTIVE");
    ASSERT_EQ(active_count, 1, "active_count = 1");
}

static void test_register_duplicate(void) {
    TEST("register_duplicate");
    reset_state();

    int slot1 = sim_register(42, 1000, 512);
    ASSERT_TRUE(slot1 >= 0, "first registration succeeds");

    int slot2 = sim_register(42, 2000, 1024);
    ASSERT_EQ(slot2, -2, "duplicate registration rejected");
}

static void test_register_full(void) {
    TEST("register_full");
    reset_state();

    for (uint32_t i = 0; i < MAX_QUOTA_SLOTS; i++) {
        int s = sim_register(100 + i, 1000, 512);
        ASSERT_TRUE(s >= 0, "registration within capacity");
    }
    int overflow = sim_register(999, 1000, 512);
    ASSERT_EQ(overflow, -1, "table full returns -1");
}

static void test_tick_basic(void) {
    TEST("tick_basic");
    reset_state();

    sim_register(42, 1000, 512);

    uint32_t flags = sim_tick(42, 100, 128);
    ASSERT_EQ(flags, QUOTA_FLAG_ACTIVE, "under quota: flags = ACTIVE only");
    ASSERT_EQ(quota_table[find_slot(42)].cpu_used_ms, 100, "cpu accumulates");
    ASSERT_EQ(revoke_count, 0, "no revocation yet");
}

static void test_cpu_quota_exceeded(void) {
    TEST("cpu_quota_exceeded");
    reset_state();

    sim_register(42, 500, 0);  /* 500ms CPU limit, no mem limit */

    /* Tick 5 times at 100ms each = 500ms total → should trigger */
    uint32_t flags = 0;
    for (int i = 0; i < 4; i++) {
        flags = sim_tick(42, 100, 64);
        ASSERT_EQ(flags, QUOTA_FLAG_ACTIVE, "under quota during accumulation");
    }

    /* 5th tick reaches exactly the limit */
    flags = sim_tick(42, 100, 64);
    ASSERT_TRUE(flags & QUOTA_FLAG_CPU_EXCEED, "CPU exceed flag set");
    ASSERT_TRUE(flags & QUOTA_FLAG_REVOKED, "REVOKED flag set");
    ASSERT_EQ(revoke_count, 1, "revocation happened once");
    ASSERT_EQ(last_revoked_agent, 42, "correct agent revoked");
    ASSERT_TRUE(last_revoke_reason & QUOTA_FLAG_CPU_EXCEED, "reason includes CPU");
}

static void test_mem_quota_exceeded(void) {
    TEST("mem_quota_exceeded");
    reset_state();

    sim_register(99, 0, 256);  /* no CPU limit, 256kb mem limit */

    uint32_t flags = sim_tick(99, 10, 128);
    ASSERT_EQ(flags, QUOTA_FLAG_ACTIVE, "under mem quota");

    flags = sim_tick(99, 10, 256);
    ASSERT_TRUE(flags & QUOTA_FLAG_MEM_EXCEED, "MEM exceed flag set");
    ASSERT_TRUE(flags & QUOTA_FLAG_REVOKED, "REVOKED flag set");
    ASSERT_EQ(revoke_count, 1, "revocation happened once");
    ASSERT_EQ(last_revoked_agent, 99, "correct agent revoked");
}

static void test_revocation_only_once(void) {
    TEST("revocation_only_once");
    reset_state();

    sim_register(42, 100, 0);

    /* Exceed CPU quota */
    sim_tick(42, 100, 0);
    ASSERT_EQ(revoke_count, 1, "first revocation");

    /* Additional ticks should NOT trigger additional revocations */
    sim_tick(42, 50, 0);
    sim_tick(42, 50, 0);
    ASSERT_EQ(revoke_count, 1, "no additional revocations after first");
}

static void test_tick_unknown_agent(void) {
    TEST("tick_unknown_agent");
    reset_state();

    uint32_t result = sim_tick(999, 100, 64);
    ASSERT_EQ(result, 0xE2, "unknown agent returns ERR_NOT_FOUND");
}

static void test_unlimited_quota(void) {
    TEST("unlimited_quota");
    reset_state();

    sim_register(42, 0, 0);  /* unlimited CPU and mem */

    for (int i = 0; i < 100; i++) {
        sim_tick(42, 1000, 1024);
    }
    ASSERT_EQ(revoke_count, 0, "unlimited agent never revoked");
    ASSERT_EQ(quota_table[find_slot(42)].cpu_used_ms, 100000, "CPU still accumulates");
}

static void test_multiple_agents(void) {
    TEST("multiple_agents");
    reset_state();

    sim_register(1, 200, 0);
    sim_register(2, 300, 0);
    sim_register(3, 400, 0);

    /* Tick agent 1 past its limit */
    sim_tick(1, 200, 0);
    ASSERT_EQ(revoke_count, 1, "only agent 1 revoked");
    ASSERT_EQ(last_revoked_agent, 1, "agent 1 was the target");

    /* Agent 2 should still be fine */
    uint32_t flags2 = sim_tick(2, 100, 0);
    ASSERT_EQ(flags2, QUOTA_FLAG_ACTIVE, "agent 2 still active");

    /* Agent 3 also fine */
    uint32_t flags3 = sim_tick(3, 100, 0);
    ASSERT_EQ(flags3, QUOTA_FLAG_ACTIVE, "agent 3 still active");
}

int main(void) {
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║       agentOS Quota PD — Test Suite          ║\n");
    printf("╚══════════════════════════════════════════════╝\n");

    test_register_basic();
    test_register_duplicate();
    test_register_full();
    test_tick_basic();
    test_cpu_quota_exceeded();
    test_mem_quota_exceeded();
    test_revocation_only_once();
    test_tick_unknown_agent();
    test_unlimited_quota();
    test_multiple_agents();

    printf("\n══════════════════════════════════════════════\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);

    if (tests_failed > 0) {
        printf("FAIL\n");
        return 1;
    }

    printf("ALL TESTS PASSED ✓\n");
    return 0;
}
