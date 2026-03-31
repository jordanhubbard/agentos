/*
 * agentOS Power Manager Protection Domain
 *
 * Passive PD (priority 240).  Implements dynamic voltage/frequency scaling
 * (DVFS) for sparky GB10 under simulated thermal load, and integrates with
 * the time_partition PD to adjust gpu_worker CPU budgets when the die
 * temperature exceeds the configurable throttle threshold.
 *
 * Architecture
 * ────────────
 * Since seL4 bare-metal cannot access /sys/thermal, the PD maintains a
 * software thermal model: temperature evolves each tick as
 *
 *   sim_temp_mC += (active_slots × 1500) − 2000
 *
 * where active_slots is passed by the controller each scheduling quantum.
 * Temperature is clamped to [25 000, 95 000] mC (25°C – 95°C).
 *
 * When sim_temp_mC crosses the threshold (default 80 000 mC = 80°C), the PD:
 *   1. Sets throttled_state = true, reduces freq to FREQ_THROTTLED_kHz.
 *   2. PPCs into time_partition (OP_TP_SET_POLICY) to double gpu_worker's
 *      period_us from 10 000 to 20 000 µs (halving its CPU allocation).
 *
 * When temp drops below threshold − 5 000 mC (hysteresis band):
 *   1. Clears throttled_state, restores FREQ_NOMINAL_kHz.
 *   2. PPCs into time_partition to restore gpu_worker period_us to 10 000 µs.
 *
 * If AGENTOS_POWER_MGR_TP_WIRED is not defined (system description lacks the
 * time_partition channel), the tp_set_gpu_period() call is a no-op stub.
 *
 * Messages (MR0 opcode)
 * ─────────────────────
 *   OP_PWR_STATUS      (0x90): → MR0=temp_mC, MR1=freq_khz, MR2=throttled, MR3=tdp_mW
 *   OP_PWR_SET_POLICY  (0x91): MR1=threshold_mC → ok (0 keeps current)
 *   OP_PWR_THROTTLE    (0x92): force throttle (testing)   → ok
 *   OP_PWR_UNTHROTTLE  (0x93): force unthrottle (testing) → ok
 *   OP_PWR_GET_HISTORY (0x94): write 16-sample ring to power_ring MR → MR0=count
 *   OP_PWR_RESET       (0x95): reset all state → ok
 *   OP_TP_TICK         (0xD5): MR1=active_slots → advance thermal model, MR0=tick_count
 *
 * Channel assignments (agentos.system):
 *   id=0: controller(pp=true) → power_mgr   (status polls + ticks)
 *   id=1: power_mgr(pp=true)  → time_partition  (throttle/unthrottle)
 *
 * Shared memory:
 *   power_ring (4KB MR):  16 × 16-byte PwrSample entries (ring buffer)
 *   Layout per entry: ts_ticks(u32) | temp_mC(i32) | freq_khz(u32) | throttled(u32)
 *
 * Prometheus metric names (wired in metrics_exporter):
 *   power_mgr_temp_mC   — current simulated die temperature in milli-Celsius
 *   power_mgr_throttled — 1 if DVFS throttle active, 0 otherwise
 *   power_mgr_freq_khz  — current effective CPU frequency in kHz
 *
 * Build flag: -DAGENTOS_POWER_MGR activates full logic; without it the PD
 * compiles to a no-op stub so the system image remains valid.
 * The time_partition IPC path requires -DAGENTOS_POWER_MGR_TP_WIRED as well
 * (set automatically when AGENTOS_POWER_MGR=1 in the Makefile).
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifdef AGENTOS_POWER_MGR

/* ── Configuration ─────────────────────────────────────────────────────── */

/* Channel IDs from power_mgr's perspective (must match agentos.system) */
#define PWRMGR_CH_CONTROLLER     0
#define PWRMGR_CH_TIME_PARTITION 1

/* Thermal model constants */
#define TEMP_MIN_mC               25000    /* 25°C — minimum clamped temperature */
#define TEMP_MAX_mC               95000    /* 95°C — maximum clamped temperature */
#define TEMP_INIT_mC              45000    /* 45°C — startup temperature */
#define TEMP_DEFAULT_THRESHOLD_mC 80000    /* 80°C — default throttle threshold */
#define TEMP_HYSTERESIS_mC         5000    /*  5°C — unthrottle hysteresis band */

/* DVFS frequency steps (sparky GB10 ARM64 cluster) */
#define FREQ_NOMINAL_kHz          2800000U /* 2.8 GHz — full-speed */
#define FREQ_THROTTLED_kHz        1400000U /* 1.4 GHz — 50% DVFS step */

/* Power envelope */
#define TDP_NOMINAL_mW             60000U  /* 60 W nominal TDP */
#define TDP_THROTTLED_mW           30000U  /* 30 W throttled TDP */

/* History ring */
#define HIST_SIZE   16   /* number of thermal samples retained */

/* ── Opcodes ────────────────────────────────────────────────────────────── */

#define OP_PWR_STATUS       0x90
#define OP_PWR_SET_POLICY   0x91
#define OP_PWR_THROTTLE     0x92
#define OP_PWR_UNTHROTTLE   0x93
#define OP_PWR_GET_HISTORY  0x94
#define OP_PWR_RESET        0x95
/* OP_TP_TICK (0xD5) from agentos.h — controller tick dispatch */

/* ── Data structures ───────────────────────────────────────────────────── */

/*
 * PwrSample — one thermal snapshot (16 bytes, HIST_SIZE × 16 = 256 bytes).
 * 16 entries fit easily within the 4KB power_ring MR.
 */
typedef struct {
    uint32_t ts_ticks;   /* controller tick counter at sample time */
    int32_t  temp_mC;    /* simulated die temperature in milli-Celsius */
    uint32_t freq_khz;   /* effective CPU frequency at sample time */
    uint32_t throttled;  /* 1 if DVFS throttle was active, 0 otherwise */
} PwrSample;             /* 16 bytes */

/* power_ring_vaddr: Microkit linker sets this from the <map setvar_vaddr> directive */
extern uintptr_t power_ring_vaddr;

/* ── Module state ──────────────────────────────────────────────────────── */

static int32_t  sim_temp_mC       = TEMP_INIT_mC;
static uint32_t active_slots      = 0;
static bool     throttled_state   = false;
static uint32_t thermal_threshold = TEMP_DEFAULT_THRESHOLD_mC;
static uint64_t tick_count        = 0;
static uint32_t hist_head         = 0;   /* next write position in ring */

/* ── Internal helpers ──────────────────────────────────────────────────── */

static PwrSample *pwr_ring(void) {
    return (PwrSample *)(uintptr_t)power_ring_vaddr;
}

static void push_sample(void) {
    PwrSample *s  = &pwr_ring()[hist_head % HIST_SIZE];
    s->ts_ticks   = (uint32_t)(tick_count & 0xFFFFFFFFU);
    s->temp_mC    = sim_temp_mC;
    s->freq_khz   = throttled_state ? FREQ_THROTTLED_kHz : FREQ_NOMINAL_kHz;
    s->throttled  = throttled_state ? 1U : 0U;
    hist_head++;
}

/*
 * tp_set_gpu_period — notify time_partition to adjust gpu_worker's CPU budget.
 *
 * OP_TP_SET_POLICY: MR1=class_id (0=CLASS_GPU_WORKER), MR2=budget_us (0=keep
 * current), MR3=period_us.  Doubling period_us halves the effective CPU share
 * without altering the budget quantum, achieving a soft frequency cap.
 *
 * When AGENTOS_POWER_MGR_TP_WIRED is defined (set by the Makefile when both
 * AGENTOS_POWER_MGR=1 and the channel is declared in agentos.system), the
 * real PPC is issued.  Otherwise we log the intent and return.
 */
static void tp_set_gpu_period(uint32_t period_us) {
#ifdef AGENTOS_POWER_MGR_TP_WIRED
    microkit_mr_set(1, 0U);          /* CLASS_GPU_WORKER */
    microkit_mr_set(2, 0U);          /* budget_us: 0 = leave unchanged */
    microkit_mr_set(3, period_us);
    microkit_ppcall(PWRMGR_CH_TIME_PARTITION,
                    microkit_msginfo_new(OP_TP_SET_POLICY, 3));
#else
    /* Channel to time_partition not wired in this build configuration.
     * Wire it by adding channel 101 in agentos.system and rebuilding with
     * AGENTOS_POWER_MGR=1 AGENTOS_POWER_MGR_TP_WIRED=1. */
    (void)period_us;
    microkit_dbg_puts("[power_mgr] STUB: would set gpu_worker period_us via time_partition\n");
#endif
}

/*
 * handle_tick — advance the thermal simulation by one controller quantum.
 *
 * Temperature model:
 *   delta = (active_slots × 1500) − 2000 mC/tick
 *   This models each active gpu_worker slot adding ~1.5°C/tick of heat, with
 *   a 2°C/tick passive cooling floor (heat-spreader + chassis airflow).
 *
 * Throttle transitions emit IPC to time_partition exactly on edge crossings
 * (hysteresis prevents flapping near the threshold).
 */
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

    /* Emit time_partition notification on state transitions only */
    if (!was_throttled && throttled_state) {
        microkit_dbg_puts("[power_mgr] THROTTLE: temp > threshold, doubling gpu_worker period\n");
        tp_set_gpu_period(20000U);  /* 2× default 10 000 µs → 50% → 25% effective */
    } else if (was_throttled && !throttled_state) {
        microkit_dbg_puts("[power_mgr] UNTHROTTLE: temp < hysteresis, restoring gpu_worker period\n");
        tp_set_gpu_period(10000U);  /* restore CLASS_GPU_WORKER default */
    }

    push_sample();
}

static void reset_all(void) {
    sim_temp_mC       = TEMP_INIT_mC;
    active_slots      = 0;
    throttled_state   = false;
    thermal_threshold = TEMP_DEFAULT_THRESHOLD_mC;
    tick_count        = 0;
    hist_head         = 0;
    memset((void *)(uintptr_t)power_ring_vaddr, 0, HIST_SIZE * sizeof(PwrSample));
}

/* ── Microkit entry points ─────────────────────────────────────────────── */

void init(void) {
    agentos_log_boot("power_mgr");
    reset_all();
    microkit_dbg_puts("[power_mgr] DVFS thermal manager initialized\n");
    microkit_dbg_puts("[power_mgr] Threshold: 80C  Hysteresis: 5C  Nominal: 2.8 GHz\n");
#ifdef AGENTOS_POWER_MGR_TP_WIRED
    microkit_dbg_puts("[power_mgr] time_partition channel: WIRED\n");
#else
    microkit_dbg_puts("[power_mgr] time_partition channel: STUB\n");
#endif
}

void notified(microkit_channel ch) {
    /* Passive PD — no unsolicited notifications expected */
    (void)ch;
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo) {
    (void)ch;
    uint32_t op   = (uint32_t)microkit_msginfo_get_label(msginfo);
    uint32_t arg1 = (uint32_t)microkit_mr_get(1);

    switch (op) {

        case OP_PWR_STATUS: {
            /*
             * Return current thermal snapshot.
             * MR0=temp_mC  MR1=freq_khz  MR2=throttled  MR3=tdp_mW
             *
             * Prometheus metrics:
             *   power_mgr_temp_mC
             *   power_mgr_throttled
             *   power_mgr_freq_khz
             */
            microkit_mr_set(0, (uint32_t)sim_temp_mC);
            microkit_mr_set(1, throttled_state ? FREQ_THROTTLED_kHz : FREQ_NOMINAL_kHz);
            microkit_mr_set(2, throttled_state ? 1U : 0U);
            microkit_mr_set(3, throttled_state ? TDP_THROTTLED_mW : TDP_NOMINAL_mW);
            return microkit_msginfo_new(0, 4);
        }

        case OP_PWR_SET_POLICY: {
            /*
             * Update thermal throttle threshold.
             * MR1=threshold_mC (0 = keep current value)
             */
            if (arg1 >= (uint32_t)TEMP_MIN_mC && arg1 <= (uint32_t)TEMP_MAX_mC)
                thermal_threshold = arg1;
            microkit_mr_set(0, 1U);
            return microkit_msginfo_new(0, 1);
        }

        case OP_PWR_THROTTLE: {
            /* Force throttle — used by test harnesses and fault injection */
            if (!throttled_state) {
                throttled_state = true;
                microkit_dbg_puts("[power_mgr] FORCE THROTTLE\n");
                tp_set_gpu_period(20000U);
            }
            microkit_mr_set(0, 1U);
            return microkit_msginfo_new(0, 1);
        }

        case OP_PWR_UNTHROTTLE: {
            /* Force unthrottle — used by test harnesses */
            if (throttled_state) {
                throttled_state = false;
                microkit_dbg_puts("[power_mgr] FORCE UNTHROTTLE\n");
                tp_set_gpu_period(10000U);
            }
            microkit_mr_set(0, 1U);
            return microkit_msginfo_new(0, 1);
        }

        case OP_PWR_GET_HISTORY: {
            /*
             * Expose the 16-entry thermal ring to the caller via shared memory.
             * Caller must map power_ring MR to read the PwrSample array.
             * MR0=sample_count (1..16), MR1=hist_head (next-write index)
             */
            uint32_t count = (tick_count < HIST_SIZE)
                           ? (uint32_t)tick_count
                           : (uint32_t)HIST_SIZE;
            microkit_mr_set(0, count);
            microkit_mr_set(1, hist_head % HIST_SIZE);
            return microkit_msginfo_new(0, 2);
        }

        case OP_PWR_RESET: {
            reset_all();
            microkit_dbg_puts("[power_mgr] state reset\n");
            microkit_mr_set(0, 1U);
            return microkit_msginfo_new(0, 1);
        }

        case OP_TP_TICK: {
            /*
             * Controller tick dispatch — advance thermal simulation.
             * MR1=active_slots (number of gpu_worker slots active this quantum).
             * Controller sends this to both time_partition and power_mgr each tick;
             * power_mgr polls OP_PWR_STATUS every 10 ticks for Prometheus metrics.
             * MR0=tick_count_lo (lower 32 bits)
             */
            handle_tick(arg1);
            microkit_mr_set(0, (uint32_t)(tick_count & 0xFFFFFFFFU));
            return microkit_msginfo_new(0, 1);
        }

        default:
            microkit_mr_set(0, 0xDEADU);
            return microkit_msginfo_new(1, 1);
    }
}

#else /* !AGENTOS_POWER_MGR — no-op stub ────────────────────────────────── */

/*
 * Stub PD: always compiled so power_mgr.elf links cleanly and the system
 * image remains valid when AGENTOS_POWER_MGR is not set.  Activate the full
 * implementation with: make build AGENTOS_POWER_MGR=1
 */

void init(void) {
    microkit_dbg_puts("[power_mgr] stub (build with AGENTOS_POWER_MGR=1 to enable)\n");
}

void notified(microkit_channel ch) { (void)ch; }

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo) {
    (void)ch;
    (void)msginfo;
    microkit_mr_set(0, 0U);
    return microkit_msginfo_new(1, 1);
}

#endif /* AGENTOS_POWER_MGR */
