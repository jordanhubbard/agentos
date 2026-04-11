/*
 * agentOS Power Manager — Unit Test
 *
 * Tests the DVFS thermal model and hysteresis logic from power_mgr.c by
 * inlining the relevant state and helper functions and exercising them
 * without seL4 or Microkit.
 *
 * Build:  cc -o /tmp/test_power_mgr \
 *             tests/test_power_mgr.c \
 *             -I kernel/agentos-root-task/include \
 *             -DAGENTOS_TEST_HOST
 * Run:    /tmp/test_power_mgr
 *
 * (The -DAGENTOS_POWER_MGR flag is NOT required here; the test inlines only
 * the thermal model logic, not the Microkit entry points.)
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

/* ══════════════════════════════════════════════════════════════════════════
 * Host-side stubs
 * ══════════════════════════════════════════════════════════════════════════ */
#ifdef AGENTOS_TEST_HOST

static uint64_t _mrs[64];
static inline void     microkit_mr_set(uint32_t i, uint64_t v) { _mrs[i] = v; }
static inline uint64_t microkit_mr_get(uint32_t i)             { return _mrs[i]; }
typedef uint32_t microkit_channel;
typedef uint64_t microkit_msginfo_t;
static inline microkit_msginfo_t microkit_msginfo_new(uint64_t l, uint32_t c) {
    (void)c; return l;
}
static inline microkit_msginfo_t microkit_ppcall(microkit_channel ch, microkit_msginfo_t m) {
    (void)ch; (void)m; return 0;
}
static inline void microkit_dbg_puts(const char *s) { printf("%s", s); }

#define LOG(fmt, ...) printf("[test] " fmt "\n", ##__VA_ARGS__)

#endif /* AGENTOS_TEST_HOST */

/* ══════════════════════════════════════════════════════════════════════════
 * Inline thermal model (mirrored from power_mgr.c)
 * ══════════════════════════════════════════════════════════════════════════ */

#define TEMP_MIN_mC                25000
#define TEMP_MAX_mC                95000
#define TEMP_INIT_mC               45000
#define TEMP_DEFAULT_THRESHOLD_mC  80000
#define TEMP_HYSTERESIS_mC          5000
#define FREQ_NOMINAL_kHz         2800000U
#define FREQ_THROTTLED_kHz       1400000U
#define TDP_NOMINAL_mW            60000U
#define TDP_THROTTLED_mW          30000U
#define HIST_SIZE 16

typedef struct {
    uint32_t ts_ticks;
    int32_t  temp_mC;
    uint32_t freq_khz;
    uint32_t throttled;
} PwrSample;

static int32_t  sim_temp_mC        = TEMP_INIT_mC;
static uint32_t active_slots       = 0;
static bool     throttled_state    = false;
static uint32_t thermal_threshold  = TEMP_DEFAULT_THRESHOLD_mC;
static uint64_t tick_count         = 0;
static uint32_t hist_head          = 0;
static PwrSample pwr_ring_buf[HIST_SIZE];

/* Track tp_set_gpu_period calls for assertion */
static uint32_t tp_call_count         = 0;
static uint32_t tp_last_period_us     = 0;

static void tp_set_gpu_period_sim(uint32_t period_us) {
    tp_call_count++;
    tp_last_period_us = period_us;
}

static void push_sample(void) {
    PwrSample *s = &pwr_ring_buf[hist_head % HIST_SIZE];
    s->ts_ticks  = (uint32_t)(tick_count & 0xFFFFFFFFU);
    s->temp_mC   = sim_temp_mC;
    s->freq_khz  = throttled_state ? FREQ_THROTTLED_kHz : FREQ_NOMINAL_kHz;
    s->throttled = throttled_state ? 1U : 0U;
    hist_head++;
}

static void handle_tick(uint32_t slots) {
    tick_count++;
    active_slots = slots;
    int32_t delta = (int32_t)(slots * 1500U) - 2000;
    sim_temp_mC += delta;
    if (sim_temp_mC < TEMP_MIN_mC) sim_temp_mC = TEMP_MIN_mC;
    if (sim_temp_mC > TEMP_MAX_mC) sim_temp_mC = TEMP_MAX_mC;
    bool was_throttled = throttled_state;
    if (!throttled_state && sim_temp_mC > (int32_t)thermal_threshold)
        throttled_state = true;
    else if (throttled_state &&
             sim_temp_mC < (int32_t)(thermal_threshold - TEMP_HYSTERESIS_mC))
        throttled_state = false;
    if (!was_throttled && throttled_state)   tp_set_gpu_period_sim(20000U);
    else if (was_throttled && !throttled_state) tp_set_gpu_period_sim(10000U);
    push_sample();
}

static void reset_all(void) {
    sim_temp_mC       = TEMP_INIT_mC;
    active_slots      = 0;
    throttled_state   = false;
    thermal_threshold = TEMP_DEFAULT_THRESHOLD_mC;
    tick_count        = 0;
    hist_head         = 0;
    tp_call_count     = 0;
    tp_last_period_us = 0;
    memset(pwr_ring_buf, 0, sizeof(pwr_ring_buf));
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
    if ((int64_t)(a) != (int64_t)(b)) { \
        printf("  FAIL: %s (expected %lld, got %lld)\n", (msg), \
               (long long)(b), (long long)(a)); \
        tests_failed++; \
    } else { printf("  PASS: %s\n", (msg)); tests_passed++; } \
} while(0)

/* ── Tests ────────────────────────────────────────────────────────────────── */

static void test_initial_state(void) {
    TEST("initial_state");
    reset_all();
    ASSERT_EQ(sim_temp_mC, TEMP_INIT_mC, "start temp = 45 000 mC");
    ASSERT_TRUE(!throttled_state, "not throttled at boot");
    ASSERT_EQ(tick_count, 0, "tick_count = 0");
}

static void test_idle_cooling(void) {
    TEST("idle_cooling");
    reset_all();
    /* 0 active slots → delta = -2000 mC/tick; temp cools toward TEMP_MIN */
    for (int i = 0; i < 10; i++) handle_tick(0);
    ASSERT_TRUE(sim_temp_mC < TEMP_INIT_mC, "idle cools system");
    ASSERT_TRUE(sim_temp_mC >= TEMP_MIN_mC, "temp clamped at minimum");
    ASSERT_TRUE(!throttled_state, "no throttle during idle cooling");
}

static void test_min_clamp(void) {
    TEST("min_clamp");
    reset_all();
    /* Drive to minimum */
    for (int i = 0; i < 20; i++) handle_tick(0);
    ASSERT_EQ(sim_temp_mC, TEMP_MIN_mC, "temperature clamped at TEMP_MIN_mC");
}

static void test_heating_under_load(void) {
    TEST("heating_under_load");
    reset_all();
    /* 3 active slots → delta = 3×1500 - 2000 = 2500 mC/tick */
    for (int i = 0; i < 5; i++) handle_tick(3);
    ASSERT_TRUE(sim_temp_mC > TEMP_INIT_mC, "temperature rises under load");
    ASSERT_TRUE(!throttled_state, "no throttle yet (below threshold)");
}

static void test_throttle_on_overheat(void) {
    TEST("throttle_on_overheat");
    reset_all();
    /*
     * 4 active slots → delta = 4000 mC/tick.
     * From 45 000 mC we need (80 000 - 45 000) / 4000 = 8.75 → 9 ticks.
     */
    int throttled_at = -1;
    for (int i = 0; i < 30; i++) {
        handle_tick(4);
        if (throttled_state && throttled_at < 0) throttled_at = i;
    }
    ASSERT_TRUE(throttled_at >= 0, "throttle activated after heating");
    ASSERT_TRUE(sim_temp_mC > (int32_t)thermal_threshold, "temp above threshold");
    ASSERT_EQ(tp_last_period_us, 20000U, "tp period doubled to 20 000 µs");
}

static void test_throttle_only_once(void) {
    TEST("throttle_only_once");
    reset_all();
    /* Heat to well above threshold */
    for (int i = 0; i < 30; i++) handle_tick(4);
    ASSERT_TRUE(throttled_state, "is throttled");
    uint32_t calls_at_throttle = tp_call_count;
    /* More ticks at high load — no additional tp calls */
    for (int i = 0; i < 10; i++) handle_tick(4);
    ASSERT_EQ(tp_call_count, calls_at_throttle, "tp not called again while already throttled");
}

static void test_unthrottle_with_hysteresis(void) {
    TEST("unthrottle_with_hysteresis");
    reset_all();
    /* Heat up */
    for (int i = 0; i < 30; i++) handle_tick(4);
    ASSERT_TRUE(throttled_state, "throttled after heating");

    /* Cool down: 0 slots → delta = -2000 mC/tick; unthrottle at 75 000 mC */
    for (int i = 0; i < 100; i++) handle_tick(0);
    ASSERT_TRUE(!throttled_state, "unthrottled after cooling");
    ASSERT_TRUE(sim_temp_mC < (int32_t)(thermal_threshold - TEMP_HYSTERESIS_mC),
                "temp below hysteresis band on unthrottle");
    ASSERT_EQ(tp_last_period_us, 10000U, "tp period restored to 10 000 µs");
}

static void test_max_clamp(void) {
    TEST("max_clamp");
    reset_all();
    /* 8 slots → delta = 10 000 mC/tick; drive to maximum */
    for (int i = 0; i < 100; i++) handle_tick(8);
    ASSERT_EQ(sim_temp_mC, TEMP_MAX_mC, "temperature clamped at TEMP_MAX_mC");
}

static void test_custom_threshold(void) {
    TEST("custom_threshold");
    reset_all();
    /* Lower the threshold to 60 000 mC */
    thermal_threshold = 60000U;
    /* 3 slots → delta = 2500/tick; from 45 000 to 60 000 = 6 ticks */
    int throttled_at = -1;
    for (int i = 0; i < 20; i++) {
        handle_tick(3);
        if (throttled_state && throttled_at < 0) throttled_at = i;
    }
    ASSERT_TRUE(throttled_at >= 0, "throttle with custom threshold");
    ASSERT_TRUE(sim_temp_mC > 60000, "temp exceeds custom threshold");
}

static void test_history_ring(void) {
    TEST("history_ring");
    reset_all();
    /* Advance HIST_SIZE + 2 ticks to confirm ring wraps */
    for (int i = 0; i < HIST_SIZE + 2; i++) handle_tick(2);
    ASSERT_EQ(tick_count, (uint64_t)(HIST_SIZE + 2), "tick_count correct");
    /* The ring wraps; just check that samples are non-zero */
    bool any_nonzero = false;
    for (int i = 0; i < HIST_SIZE; i++) {
        if (pwr_ring_buf[i].temp_mC != 0) { any_nonzero = true; break; }
    }
    ASSERT_TRUE(any_nonzero, "history ring has non-zero entries");
}

static void test_no_throttle_without_load(void) {
    TEST("no_throttle_without_load");
    reset_all();
    for (int i = 0; i < 1000; i++) handle_tick(1); /* 1 slot: delta = -500/tick */
    ASSERT_TRUE(!throttled_state, "1 active slot never triggers throttle");
    ASSERT_EQ(tp_call_count, 0, "no tp calls with 1 slot");
}

/* ── main ──────────────────────────────────────────────────────────────── */

int main(void) {
    printf("╔═════════════════════════════════════════════╗\n");
    printf("║   agentOS Power Manager — Test Suite        ║\n");
    printf("╚═════════════════════════════════════════════╝\n");

    test_initial_state();
    test_idle_cooling();
    test_min_clamp();
    test_heating_under_load();
    test_throttle_on_overheat();
    test_throttle_only_once();
    test_unthrottle_with_hysteresis();
    test_max_clamp();
    test_custom_threshold();
    test_history_ring();
    test_no_throttle_without_load();

    printf("\n══════════════════════════════════════════════\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    if (tests_failed > 0) {
        printf("FAIL\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
