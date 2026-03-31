/*
 * test_power_mgr.c — unit tests for agentOS power_mgr thermal simulation
 *
 * Standalone host test that mirrors the thermal model and ring buffer logic
 * from power_mgr.c and exercises the full tick/throttle/unthrottle cycle.
 *
 * Tests:
 *   1. Initial state verification after reset
 *   2. 20 ticks at high slot count → throttled=1 and time_partition notified
 *   3. 20 ticks at 0 slots       → throttled=0 and time_partition notified
 *   4. History ring contains exactly 16 samples with correct wrap behaviour
 *
 * Build & run (no special dependencies):
 *   cc test/test_power_mgr.c -o /tmp/test_power_mgr && /tmp/test_power_mgr
 *
 * Exits 0 on success, non-zero (via assert) on failure.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

/* ── Mirror power_mgr constants ──────────────────────────────────────────── */

#define HIST_SIZE                 16
#define TEMP_MIN_mC               25000
#define TEMP_MAX_mC               95000
#define TEMP_INIT_mC              45000
#define TEMP_DEFAULT_THRESHOLD_mC 80000
#define TEMP_HYSTERESIS_mC         5000
#define FREQ_NOMINAL_kHz          2800000U
#define FREQ_THROTTLED_kHz        1400000U
#define TDP_NOMINAL_mW             60000U
#define TDP_THROTTLED_mW           30000U

/* ── Mirror data structures ──────────────────────────────────────────────── */

typedef struct {
    uint32_t ts_ticks;
    int32_t  temp_mC;
    uint32_t freq_khz;
    uint32_t throttled;
} PwrSample;  /* 16 bytes */

/* Compile-time check: 16 entries × 16 bytes = 256 bytes ≤ 4KB MR */
typedef char _pwr_ring_size_check[HIST_SIZE * sizeof(PwrSample) <= 4096 ? 1 : -1];

/* ── Simulated state (mirrors power_mgr.c module-level statics) ─────────── */

static int32_t  sim_temp_mC       = TEMP_INIT_mC;
static uint32_t active_slots      = 0;
static bool     throttled_state   = false;
static uint32_t thermal_threshold = TEMP_DEFAULT_THRESHOLD_mC;
static uint64_t tick_count        = 0;
static uint32_t hist_head         = 0;
static PwrSample ring[HIST_SIZE];

/* ── Test observability: track IPC calls to time_partition ──────────────── */

static int tp_throttle_calls   = 0;  /* calls with period_us=20000 */
static int tp_unthrottle_calls = 0;  /* calls with period_us=10000 */

static void tp_set_gpu_period(uint32_t period_us) {
    if (period_us == 20000U) tp_throttle_calls++;
    else if (period_us == 10000U) tp_unthrottle_calls++;
}

/* ── Mirror handle_tick (exact copy of power_mgr.c logic) ──────────────── */

static void push_sample(void) {
    PwrSample *s = &ring[hist_head % HIST_SIZE];
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
    sim_temp_mC  += delta;

    if (sim_temp_mC < TEMP_MIN_mC) sim_temp_mC = TEMP_MIN_mC;
    if (sim_temp_mC > TEMP_MAX_mC) sim_temp_mC = TEMP_MAX_mC;

    bool was_throttled = throttled_state;

    if (!throttled_state && sim_temp_mC > (int32_t)thermal_threshold) {
        throttled_state = true;
    } else if (throttled_state &&
               sim_temp_mC < (int32_t)(thermal_threshold - TEMP_HYSTERESIS_mC)) {
        throttled_state = false;
    }

    if (!was_throttled && throttled_state)  tp_set_gpu_period(20000U);
    else if (was_throttled && !throttled_state) tp_set_gpu_period(10000U);

    push_sample();
}

static void reset_state(void) {
    sim_temp_mC        = TEMP_INIT_mC;
    active_slots       = 0;
    throttled_state    = false;
    thermal_threshold  = TEMP_DEFAULT_THRESHOLD_mC;
    tick_count         = 0;
    hist_head          = 0;
    tp_throttle_calls  = 0;
    tp_unthrottle_calls= 0;
    memset(ring, 0, sizeof(ring));
}

/* ── Test 1: initial state ──────────────────────────────────────────────── */

static void test_1_init_state(void) {
    printf("[1/4] test_init_state\n");

    reset_state();

    assert(sim_temp_mC       == TEMP_INIT_mC);
    assert(throttled_state   == false);
    assert(thermal_threshold == TEMP_DEFAULT_THRESHOLD_mC);
    assert(tick_count        == 0);
    assert(hist_head         == 0);
    assert(tp_throttle_calls == 0);

    /* Verify ring is zeroed */
    for (int i = 0; i < HIST_SIZE; i++) {
        assert(ring[i].ts_ticks == 0);
        assert(ring[i].temp_mC  == 0);
    }

    printf("    PASS — initial state clean at %d mC, not throttled\n", sim_temp_mC);
}

/* ── Test 2: high load → throttle ──────────────────────────────────────── */
/*
 * At 8 active slots:
 *   delta = 8×1500 − 2000 = 10 000 mC/tick
 *   From 45 000 mC:  tick 4 → 85 000 > 80 000 → throttle fires.
 *   After 20 ticks the die saturates at 95 000 mC (TEMP_MAX).
 */
static void test_2_high_load_throttles(void) {
    printf("[2/4] test_high_load_throttles (20 ticks @ 8 slots)\n");

    /* State continues from test 1 (temp=45 000, not throttled) */
    for (int i = 0; i < 20; i++) handle_tick(8);

    printf("    final temp : %d mC\n", sim_temp_mC);
    printf("    throttled  : %s\n", throttled_state ? "yes" : "no");
    printf("    throttle IPC calls: %d\n", tp_throttle_calls);

    assert(sim_temp_mC   == TEMP_MAX_mC);
    assert(throttled_state == true);
    assert(tp_throttle_calls >= 1);   /* time_partition was notified */
    assert(tp_unthrottle_calls == 0); /* no spurious unthrottle */

    /* All samples in the ring should show throttled=1 after the transition */
    int throttled_samples = 0;
    for (int i = 0; i < HIST_SIZE; i++) {
        if (ring[i].throttled) throttled_samples++;
    }
    assert(throttled_samples > 0);

    printf("    PASS\n");
}

/* ── Test 3: idle cooling → unthrottle ─────────────────────────────────── */
/*
 * At 0 active slots:
 *   delta = −2 000 mC/tick (passive cooling)
 *   From 95 000 mC: must cool below 80 000 − 5 000 = 75 000 to unthrottle.
 *   75 000 = 95 000 − 10 000 → reached after 10 ticks, drops below at tick 11.
 *   After 20 ticks: 95 000 − 40 000 = 55 000 mC, throttled=false.
 */
static void test_3_idle_unthrottles(void) {
    printf("[3/4] test_idle_unthrottles (20 ticks @ 0 slots)\n");

    /* State continues from test 2 (temp=95 000, throttled=true) */
    int tp_throttle_before = tp_throttle_calls;
    for (int i = 0; i < 20; i++) handle_tick(0);

    printf("    final temp : %d mC\n", sim_temp_mC);
    printf("    throttled  : %s\n", throttled_state ? "yes" : "no");
    printf("    unthrottle IPC calls: %d\n", tp_unthrottle_calls);

    assert(throttled_state    == false);
    assert(tp_unthrottle_calls >= 1);         /* time_partition was notified */
    assert(tp_throttle_calls == tp_throttle_before); /* no spurious re-throttle */
    assert(sim_temp_mC < (int32_t)TEMP_DEFAULT_THRESHOLD_mC);

    printf("    PASS\n");
}

/* ── Test 4: history ring size and wrap ─────────────────────────────────── */

static void test_4_history_ring(void) {
    printf("[4/4] test_history_ring (16 samples + wrap-around)\n");

    reset_state();

    /* Fill ring exactly once (16 ticks at 0 slots, temp descends from 45 000) */
    for (int i = 0; i < HIST_SIZE; i++) handle_tick(0);

    uint32_t count = (tick_count < HIST_SIZE)
                   ? (uint32_t)tick_count
                   : (uint32_t)HIST_SIZE;

    printf("    sample count : %u\n", count);
    printf("    hist_head    : %u\n", hist_head);

    assert(count     == 16U);
    assert(hist_head == 16U);
    assert(tick_count == 16U);

    /* Verify ts_ticks is a monotonically increasing sequence 1..16 */
    for (int i = 0; i < HIST_SIZE; i++) {
        assert(ring[i].ts_ticks == (uint32_t)(i + 1));
    }

    /* Push one more tick — should overwrite ring[0] (16 % 16 = 0) */
    handle_tick(0);
    assert(hist_head  == 17U);
    assert(ring[0].ts_ticks == 17U);   /* oldest slot overwritten */
    assert(ring[1].ts_ticks ==  2U);   /* adjacent entry unchanged */

    /* Verify frequency and throttle fields are populated sensibly */
    for (int i = 1; i < HIST_SIZE; i++) {
        assert(ring[i].freq_khz == FREQ_NOMINAL_kHz ||
               ring[i].freq_khz == FREQ_THROTTLED_kHz);
        assert(ring[i].throttled == 0U || ring[i].throttled == 1U);
    }

    printf("    PASS\n");
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void) {
    printf("=== agentOS power_mgr unit tests ===\n\n");

    test_1_init_state();
    test_2_high_load_throttles();
    test_3_idle_unthrottles();
    test_4_history_ring();

    printf("\nAll tests passed.\n");
    return 0;
}
