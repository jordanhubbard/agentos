/*
 * timer_pd.c — agentOS Generic Timer Protection Domain
 *
 * OS-neutral IPC timer and RTC service. Provides CREATE/DESTROY/START/STOP/STATUS/
 * CONFIGURE/SET_RTC/GET_RTC over seL4 IPC. Timers fire by notifying a caller-specified
 * microkit channel; periodic timers auto-reload.
 *
 * IPC Protocol (caller -> timer_pd, channel CH_TIMER_PD):
 *   MSG_TIMER_CREATE    (0x1040) — MR1=period_us MR2=flags → MR1=timer_id
 *   MSG_TIMER_DESTROY   (0x1041) — MR1=timer_id → ok
 *   MSG_TIMER_START     (0x1042) — MR1=timer_id → ok
 *   MSG_TIMER_STOP      (0x1043) — MR1=timer_id → ok
 *   MSG_TIMER_STATUS    (0x1044) — MR1=timer_id → MR1=running MR2=elapsed_us
 *   MSG_TIMER_CONFIGURE (0x1045) — MR1=timer_id MR2=period_us → ok
 *   MSG_TIMER_SET_RTC   (0x1046) — MR1=unix_ts_lo MR2=unix_ts_hi → ok
 *   MSG_TIMER_GET_RTC   (0x1047) — → MR1=unix_ts_lo MR2=unix_ts_hi
 *
 * Hardware: ARM Generic Timer (CNTP/CNTV) via device capability.
 *           Actual hardware programming is Phase 2.5 (see TODO below).
 *
 * Priority: 225
 */
#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "contracts/timer_contract.h"

/* ── State ───────────────────────────────────────────────────────────────── */
static bool     s_slot_used[TIMER_MAX_TIMERS];
static bool     s_running[TIMER_MAX_TIMERS];
static uint32_t s_period_us[TIMER_MAX_TIMERS];
static uint32_t s_flags[TIMER_MAX_TIMERS];
static uint32_t s_elapsed_us[TIMER_MAX_TIMERS]; /* simulated elapsed time */

/* RTC: stored as 64-bit UNIX timestamp split into two 32-bit words */
static uint32_t s_rtc_lo;
static uint32_t s_rtc_hi;

/* ── Internal helpers ────────────────────────────────────────────────────── */

static void fire_timer(uint32_t id)
{
    /*
     * TODO Phase 2.5: notify the channel registered by caller at CREATE time.
     * For now, advance a simulated elapsed counter.
     */
    if (s_flags[id] & TIMER_FLAG_PERIODIC) {
        s_elapsed_us[id] = 0; /* reset for next period */
    } else {
        s_running[id] = false; /* one-shot: stop after firing */
    }
}

/* ── Microkit entry points ───────────────────────────────────────────────── */

void init(void)
{
    for (uint32_t i = 0; i < TIMER_MAX_TIMERS; i++) {
        s_slot_used[i]  = false;
        s_running[i]    = false;
        s_period_us[i]  = 0;
        s_flags[i]      = 0;
        s_elapsed_us[i] = 0;
    }
    s_rtc_lo = 0;
    s_rtc_hi = 0;
    microkit_dbg_puts("[timer_pd] ready\n");
}

void notified(microkit_channel ch)
{
    /*
     * A notification from the hardware timer IRQ PD fires this handler.
     * Walk all running timers and fire any that have expired.
     * TODO Phase 2.5: compare CNTPCT against each timer's deadline.
     */
    (void)ch;
    for (uint32_t i = 0; i < TIMER_MAX_TIMERS; i++) {
        if (s_slot_used[i] && s_running[i]) {
            s_elapsed_us[i] += s_period_us[i]; /* placeholder tick */
            fire_timer(i);
        }
    }
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo)
{
    (void)ch; (void)msginfo;
    uint32_t op       = (uint32_t)microkit_mr_get(0);
    uint32_t timer_id = (uint32_t)microkit_mr_get(1);

    switch (op) {
    case MSG_TIMER_CREATE: {
        uint32_t period = (uint32_t)microkit_mr_get(1);
        uint32_t flags  = (uint32_t)microkit_mr_get(2);
        if (period == 0) {
            microkit_mr_set(0, TIMER_ERR_BAD_PERIOD);
            return microkit_msginfo_new(0, 1);
        }
        uint32_t found = TIMER_MAX_TIMERS;
        for (uint32_t i = 0; i < TIMER_MAX_TIMERS; i++) {
            if (!s_slot_used[i]) { found = i; break; }
        }
        if (found == TIMER_MAX_TIMERS) {
            microkit_mr_set(0, TIMER_ERR_NO_SLOT);
            return microkit_msginfo_new(0, 1);
        }
        s_slot_used[found]  = true;
        s_running[found]    = false;
        s_period_us[found]  = period;
        s_flags[found]      = flags;
        s_elapsed_us[found] = 0;
        microkit_mr_set(0, TIMER_OK);
        microkit_mr_set(1, found);
        return microkit_msginfo_new(0, 2);
    }
    case MSG_TIMER_DESTROY:
        if (timer_id >= TIMER_MAX_TIMERS || !s_slot_used[timer_id]) {
            microkit_mr_set(0, TIMER_ERR_BAD_SLOT);
            return microkit_msginfo_new(0, 1);
        }
        s_running[timer_id]   = false;
        s_slot_used[timer_id] = false;
        microkit_mr_set(0, TIMER_OK);
        return microkit_msginfo_new(0, 1);
    case MSG_TIMER_START:
        if (timer_id >= TIMER_MAX_TIMERS || !s_slot_used[timer_id]) {
            microkit_mr_set(0, TIMER_ERR_BAD_SLOT);
            return microkit_msginfo_new(0, 1);
        }
        s_running[timer_id]    = true;
        s_elapsed_us[timer_id] = 0;
        /* TODO Phase 2.5: program ARM Generic Timer CNTP_CVAL for this timer's deadline */
        microkit_mr_set(0, TIMER_OK);
        return microkit_msginfo_new(0, 1);
    case MSG_TIMER_STOP:
        if (timer_id >= TIMER_MAX_TIMERS || !s_slot_used[timer_id]) {
            microkit_mr_set(0, TIMER_ERR_BAD_SLOT);
            return microkit_msginfo_new(0, 1);
        }
        s_running[timer_id] = false;
        microkit_mr_set(0, TIMER_OK);
        return microkit_msginfo_new(0, 1);
    case MSG_TIMER_STATUS:
        if (timer_id >= TIMER_MAX_TIMERS || !s_slot_used[timer_id]) {
            microkit_mr_set(0, TIMER_ERR_BAD_SLOT);
            return microkit_msginfo_new(0, 1);
        }
        microkit_mr_set(0, TIMER_OK);
        microkit_mr_set(1, s_running[timer_id] ? 1u : 0u);
        microkit_mr_set(2, s_elapsed_us[timer_id]);
        return microkit_msginfo_new(0, 3);
    case MSG_TIMER_CONFIGURE:
        if (timer_id >= TIMER_MAX_TIMERS || !s_slot_used[timer_id]) {
            microkit_mr_set(0, TIMER_ERR_BAD_SLOT);
            return microkit_msginfo_new(0, 1);
        }
        {
            uint32_t new_period = (uint32_t)microkit_mr_get(2);
            if (new_period == 0) {
                microkit_mr_set(0, TIMER_ERR_BAD_PERIOD);
                return microkit_msginfo_new(0, 1);
            }
            s_period_us[timer_id] = new_period;
        }
        microkit_mr_set(0, TIMER_OK);
        return microkit_msginfo_new(0, 1);
    case MSG_TIMER_SET_RTC:
        s_rtc_lo = (uint32_t)microkit_mr_get(1);
        s_rtc_hi = (uint32_t)microkit_mr_get(2);
        microkit_mr_set(0, TIMER_OK);
        return microkit_msginfo_new(0, 1);
    case MSG_TIMER_GET_RTC:
        microkit_mr_set(0, TIMER_OK);
        microkit_mr_set(1, s_rtc_lo);
        microkit_mr_set(2, s_rtc_hi);
        return microkit_msginfo_new(0, 3);
    default:
        microkit_dbg_puts("[timer_pd] unknown op\n");
        microkit_mr_set(0, TIMER_ERR_NOT_IMPL);
        return microkit_msginfo_new(0, 1);
    }
}
