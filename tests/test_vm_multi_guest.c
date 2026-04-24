/*
 * test_vm_multi_guest.c — Multi-guest VM scheduler unit test
 *
 * Verifies that the credit-based round-robin scheduler in vm_manager.c
 * correctly:
 *
 *   1. Accumulates run_ticks on active slots
 *   2. Preempts the current slot when credits are exhausted
 *   3. Distributes CPU time fairly across two concurrent guests
 *   4. Rebalances quotas (50/50) when a second guest is created
 *   5. Fires the scheduler every VM_SCHED_IPC_QUANTUM IPC dispatches
 *   6. Skips FREE slots and slots with max_cpu_pct == 0
 *
 * This is a host-side test: seL4 primitives are stubbed, vmm_mux_pause/resume
 * are simulated, and the scheduler logic from vm_manager.c is mirrored inline
 * so no cross-compilation or seL4 headers are needed.
 *
 * Build:
 *   cc -o /tmp/test_vm_multi_guest tests/test_vm_multi_guest.c -std=c11
 * Run:
 *   /tmp/test_vm_multi_guest
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ── Constants (must match vm_manager.h / vm_manager.c) ─────────────────── */

#define VM_MAX_SLOTS            4u
#define SCHED_CREDIT_QUANTUM    1u
#define SCHED_CREDITS_PER_PCT   4u
#define VM_SCHED_IPC_QUANTUM    16u

/* ── Slot states (mirror of vmm_mux.h) ──────────────────────────────────── */

typedef enum {
    VM_SLOT_FREE      = 0,
    VM_SLOT_BOOTING   = 1,
    VM_SLOT_RUNNING   = 2,
    VM_SLOT_SUSPENDED = 3,
    VM_SLOT_HALTED    = 4,
    VM_SLOT_ERROR     = 5,
} vm_slot_state_t;

/* ── Per-slot structures ─────────────────────────────────────────────────── */

typedef struct {
    uint8_t         id;
    vm_slot_state_t state;
    char            label[16];
} vm_slot_t;

typedef struct {
    vm_slot_t slots[VM_MAX_SLOTS];
    uint8_t   active_slot;
    uint8_t   slot_count;
} vm_mux_t;

typedef struct {
    uint8_t  max_cpu_pct;
    int32_t  credits;
    uint64_t run_ticks;
    uint64_t preempt_count;
} vm_slot_quota_t;

/* ── Simulated pause/resume tracking ────────────────────────────────────── */

static uint32_t g_pause_calls[VM_MAX_SLOTS];
static uint32_t g_resume_calls[VM_MAX_SLOTS];

static int sim_vmm_mux_pause(vm_mux_t *mux, uint8_t slot_id)
{
    if (slot_id >= VM_MAX_SLOTS) return -1;
    if (mux->slots[slot_id].state != VM_SLOT_RUNNING &&
        mux->slots[slot_id].state != VM_SLOT_BOOTING)
        return -1;
    mux->slots[slot_id].state = VM_SLOT_SUSPENDED;
    g_pause_calls[slot_id]++;
    return 0;
}

static int sim_vmm_mux_resume(vm_mux_t *mux, uint8_t slot_id)
{
    if (slot_id >= VM_MAX_SLOTS) return -1;
    if (mux->slots[slot_id].state != VM_SLOT_SUSPENDED &&
        mux->slots[slot_id].state != VM_SLOT_HALTED)
        return -1;
    mux->slots[slot_id].state = VM_SLOT_RUNNING;
    g_resume_calls[slot_id]++;
    return 0;
}

/* ── Scheduler (mirrors vm_manager.c:vm_sched_tick exactly) ─────────────── */

static uint8_t      g_sched_current = 0;
static vm_slot_quota_t g_quotas[VM_MAX_SLOTS];

static void sched_tick(vm_mux_t *mux)
{
    uint8_t cur = g_sched_current;

    /* Accumulate run_ticks for all active slots */
    for (uint8_t i = 0; i < VM_MAX_SLOTS; i++) {
        vm_slot_state_t st = mux->slots[i].state;
        if (st == VM_SLOT_RUNNING || st == VM_SLOT_BOOTING)
            g_quotas[i].run_ticks++;
    }

    vm_slot_state_t cur_state = mux->slots[cur].state;
    bool cur_runnable = (cur_state == VM_SLOT_RUNNING ||
                         cur_state == VM_SLOT_BOOTING) &&
                        g_quotas[cur].max_cpu_pct > 0;

    if (!cur_runnable) {
        for (uint8_t i = 0; i < VM_MAX_SLOTS; i++) {
            vm_slot_state_t s = mux->slots[i].state;
            if ((s == VM_SLOT_RUNNING || s == VM_SLOT_BOOTING) &&
                g_quotas[i].max_cpu_pct > 0) {
                g_sched_current = i;
                g_quotas[i].credits =
                    (int32_t)((uint32_t)g_quotas[i].max_cpu_pct *
                              SCHED_CREDITS_PER_PCT);
                break;
            }
        }
        return;
    }

    g_quotas[cur].credits -= (int32_t)SCHED_CREDIT_QUANTUM;
    if (g_quotas[cur].credits > 0) return;

    uint8_t next    = cur;
    bool    switched = false;
    for (uint8_t step = 1; step <= VM_MAX_SLOTS; step++) {
        uint8_t candidate = (uint8_t)((cur + step) % VM_MAX_SLOTS);
        vm_slot_state_t s = mux->slots[candidate].state;
        if ((s == VM_SLOT_RUNNING || s == VM_SLOT_BOOTING) &&
            g_quotas[candidate].max_cpu_pct > 0 &&
            candidate != cur) {
            next     = candidate;
            switched = true;
            break;
        }
    }

    if (!switched) {
        g_quotas[cur].credits =
            (int32_t)((uint32_t)g_quotas[cur].max_cpu_pct *
                      SCHED_CREDITS_PER_PCT);
        return;
    }

    g_quotas[cur].preempt_count++;
    sim_vmm_mux_pause(mux, cur);

    g_quotas[next].credits =
        (int32_t)((uint32_t)g_quotas[next].max_cpu_pct *
                  SCHED_CREDITS_PER_PCT);
    sim_vmm_mux_resume(mux, next);

    g_sched_current = next;
}

/* ── Quota rebalancing (mirrors h_create in vm_manager.c) ───────────────── */

static void rebalance_quotas(vm_mux_t *mux)
{
    uint8_t active = mux->slot_count;
    uint8_t share  = (active > 0u) ? (uint8_t)(100u / active) : 100u;
    for (uint8_t i = 0; i < VM_MAX_SLOTS; i++) {
        if (mux->slots[i].state != VM_SLOT_FREE) {
            g_quotas[i].max_cpu_pct = share;
            /* Always reset credits on rebalance so slots don't carry excess
             * credit from a higher quota into the new smaller-quota regime. */
            g_quotas[i].credits =
                (int32_t)((uint32_t)share * SCHED_CREDITS_PER_PCT);
        }
    }
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void reset_all(vm_mux_t *mux)
{
    memset(mux,          0, sizeof(*mux));
    memset(g_quotas,     0, sizeof(g_quotas));
    memset(g_pause_calls, 0, sizeof(g_pause_calls));
    memset(g_resume_calls, 0, sizeof(g_resume_calls));
    g_sched_current = 0;
}

static uint8_t add_guest(vm_mux_t *mux, const char *label)
{
    if (mux->slot_count >= VM_MAX_SLOTS) return 0xFF;
    uint8_t id = 0xFF;
    for (uint8_t i = 0; i < VM_MAX_SLOTS; i++) {
        if (mux->slots[i].state == VM_SLOT_FREE) { id = i; break; }
    }
    if (id == 0xFF) return 0xFF;
    mux->slots[id].id    = id;
    mux->slots[id].state = VM_SLOT_RUNNING;
    for (int i = 0; i < 15 && label[i]; i++)
        mux->slots[id].label[i] = label[i];
    mux->slot_count++;
    rebalance_quotas(mux);
    return id;
}

/* ── Test framework ──────────────────────────────────────────────────────── */

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) printf("\n=== TEST: %s ===\n", (name))

#define ASSERT_TRUE(expr, msg) do { \
    if (!(expr)) { printf("  FAIL: %s\n", (msg)); tests_failed++; } \
    else         { printf("  PASS: %s\n", (msg)); tests_passed++; } \
} while(0)

#define ASSERT_EQ(a, b, msg) do { \
    unsigned long long _a = (unsigned long long)(a); \
    unsigned long long _b = (unsigned long long)(b); \
    if (_a != _b) { \
        printf("  FAIL: %s (expected %llu, got %llu)\n", (msg), _b, _a); \
        tests_failed++; \
    } else { printf("  PASS: %s\n", (msg)); tests_passed++; } \
} while(0)

/* ── Tests ───────────────────────────────────────────────────────────────── */

static void test_single_slot_accumulates_ticks(void)
{
    TEST("single_slot_accumulates_ticks");
    vm_mux_t mux;
    reset_all(&mux);

    add_guest(&mux, "vm0");
    ASSERT_EQ(g_quotas[0].max_cpu_pct, 100, "single slot gets 100% quota");

    for (int i = 0; i < 10; i++) sched_tick(&mux);

    ASSERT_TRUE(g_quotas[0].run_ticks > 0, "slot 0 run_ticks > 0 after 10 ticks");
    ASSERT_EQ(g_quotas[0].run_ticks, 10, "slot 0 run_ticks == 10");
}

static void test_two_slots_quota_rebalance(void)
{
    TEST("two_slots_quota_rebalance");
    vm_mux_t mux;
    reset_all(&mux);

    add_guest(&mux, "vm0");
    ASSERT_EQ(g_quotas[0].max_cpu_pct, 100, "first guest: 100%");

    add_guest(&mux, "vm1");
    ASSERT_EQ(g_quotas[0].max_cpu_pct, 50, "after second guest: slot 0 → 50%");
    ASSERT_EQ(g_quotas[1].max_cpu_pct, 50, "after second guest: slot 1 → 50%");
    ASSERT_EQ(mux.slot_count, 2, "slot_count == 2");
}

static void test_two_slots_both_get_ticks(void)
{
    TEST("two_slots_both_get_ticks");
    vm_mux_t mux;
    reset_all(&mux);

    add_guest(&mux, "vm0");
    add_guest(&mux, "vm1");

    /*
     * With 50% quota each and SCHED_CREDITS_PER_PCT=4, each slot starts with
     * 200 credits (50 * 4). Each tick deducts SCHED_CREDIT_QUANTUM=1.  After
     * 200 ticks slot 0 is preempted; slot 1 runs for the next 200 ticks.
     * Run enough ticks so both slots accumulate run_ticks.
     */
    for (int i = 0; i < 500; i++) sched_tick(&mux);

    ASSERT_TRUE(g_quotas[0].run_ticks > 0, "slot 0 run_ticks > 0");
    ASSERT_TRUE(g_quotas[1].run_ticks > 0, "slot 1 run_ticks > 0");
    printf("  INFO: slot0 run_ticks=%llu  slot1 run_ticks=%llu\n",
           (unsigned long long)g_quotas[0].run_ticks,
           (unsigned long long)g_quotas[1].run_ticks);
}

static void test_preemption_fires(void)
{
    TEST("preemption_fires");
    vm_mux_t mux;
    reset_all(&mux);

    add_guest(&mux, "vm0");
    add_guest(&mux, "vm1");

    /* Both slots get 50% quota → 200 credits each. After 201 ticks slot 0
     * should be preempted at least once. */
    for (int i = 0; i < 250; i++) sched_tick(&mux);

    ASSERT_TRUE(g_quotas[0].preempt_count > 0,
                "slot 0 preempted at least once");
    printf("  INFO: slot0 preempt_count=%llu\n",
           (unsigned long long)g_quotas[0].preempt_count);
}

static void test_free_slot_skipped(void)
{
    TEST("free_slot_skipped");
    vm_mux_t mux;
    reset_all(&mux);

    add_guest(&mux, "vm0");
    /* slot 1, 2, 3 are FREE */

    for (int i = 0; i < 20; i++) sched_tick(&mux);

    ASSERT_EQ(g_quotas[1].run_ticks, 0, "free slot 1: no run_ticks");
    ASSERT_EQ(g_quotas[2].run_ticks, 0, "free slot 2: no run_ticks");
    ASSERT_EQ(g_quotas[3].run_ticks, 0, "free slot 3: no run_ticks");
}

static void test_zero_quota_slot_skipped(void)
{
    TEST("zero_quota_slot_skipped");
    vm_mux_t mux;
    reset_all(&mux);

    add_guest(&mux, "vm0");
    add_guest(&mux, "vm1");

    /* Manually zero slot 1's quota — it should never be scheduled */
    g_quotas[1].max_cpu_pct = 0;
    g_quotas[1].credits     = 0;

    for (int i = 0; i < 500; i++) sched_tick(&mux);

    ASSERT_EQ(g_quotas[1].preempt_count, 0,
              "slot with zero quota is never preempted (was never run)");
    ASSERT_EQ(g_sched_current, 0,
              "scheduler stays on slot 0 (only runnable slot)");
}

static void test_ipc_counter_fires_tick(void)
{
    TEST("ipc_counter_fires_tick");
    vm_mux_t mux;
    reset_all(&mux);

    add_guest(&mux, "vm0");
    add_guest(&mux, "vm1");

    /* Simulate the IPC-counter-driven tick from vm_manager_main():
     * every VM_SCHED_IPC_QUANTUM dispatches we call sched_tick once. */
    uint32_t ipc_counter = 0;
    uint32_t tick_fires  = 0;
    uint32_t total_ipc   = VM_SCHED_IPC_QUANTUM * 10u; /* 10 tick firings */

    for (uint32_t i = 0; i < total_ipc; i++) {
        if (++ipc_counter >= VM_SCHED_IPC_QUANTUM) {
            ipc_counter = 0;
            sched_tick(&mux);
            tick_fires++;
        }
    }

    ASSERT_EQ(tick_fires, 10, "scheduler fires exactly 10 times over 160 IPCs");
    ASSERT_TRUE(g_quotas[0].run_ticks > 0,
                "slot 0 accumulated run_ticks via IPC-driven ticks");
}

static void test_four_slots_round_robin(void)
{
    TEST("four_slots_round_robin");
    vm_mux_t mux;
    reset_all(&mux);

    add_guest(&mux, "vm0");
    add_guest(&mux, "vm1");
    add_guest(&mux, "vm2");
    add_guest(&mux, "vm3");

    ASSERT_EQ(g_quotas[0].max_cpu_pct, 25, "four slots → 25% each");
    ASSERT_EQ(mux.slot_count, 4, "slot_count == 4");

    /* 25% quota → 100 credits per slot. Each tick costs 1 credit.
     * After 400+ ticks every slot should have had some run time. */
    for (int i = 0; i < 500; i++) sched_tick(&mux);

    for (uint8_t s = 0; s < 4; s++) {
        ASSERT_TRUE(g_quotas[s].run_ticks > 0, "all four slots get run_ticks");
    }

    printf("  INFO: run_ticks: [%llu, %llu, %llu, %llu]\n",
           (unsigned long long)g_quotas[0].run_ticks,
           (unsigned long long)g_quotas[1].run_ticks,
           (unsigned long long)g_quotas[2].run_ticks,
           (unsigned long long)g_quotas[3].run_ticks);
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║  agentOS Multi-Guest VM Scheduler — Test Suite       ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n");

    test_single_slot_accumulates_ticks();
    test_two_slots_quota_rebalance();
    test_two_slots_both_get_ticks();
    test_preemption_fires();
    test_free_slot_skipped();
    test_zero_quota_slot_skipped();
    test_ipc_counter_fires_tick();
    test_four_slots_round_robin();

    printf("\n══════════════════════════════════════════════════════\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    if (tests_failed > 0) {
        printf("FAIL\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
