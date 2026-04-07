/*
 * agentOS Trace Recorder Protection Domain — stub implementation
 *
 * Receives inter-PD dispatch events from the controller via fast
 * microkit_notify() signals (channel 1 = CH_TRACE_NOTIFY from controller
 * perspective CH_TRACE_NOTIFY=7).  PPC channel (id=0) handles
 * START/STOP/QUERY/DUMP ops.
 *
 * This stub discards events but counts them so QUERY returns a live count.
 * A full implementation would store packed trace entries in a shared-memory
 * ring and export them via DUMP for post-mortem analysis.
 *
 * Channel assignments (from trace_recorder's perspective):
 *   id=0: receives PPC from controller (START/STOP/QUERY/DUMP)
 *   id=1: receives notify from controller (one per inter-PD dispatch)
 *
 * Priority: 128 (interactive) — below real-time PDs, above workers.
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"

/* ── Channel IDs (trace_recorder's local view) ────────────────────────── */
#define CH_CTRL_PPC    0   /* controller PPCs in for START/STOP/QUERY/DUMP */
#define CH_CTRL_NOTIFY 1   /* controller notifies on each dispatch event   */

/* ── State ─────────────────────────────────────────────────────────────── */
static uint32_t event_count  = 0;
static bool     recording    = false;
static uint32_t overflow_count = 0;

/* ── Microkit entry points ──────────────────────────────────────────────── */

void init(void) {
    event_count   = 0;
    recording     = true;  /* default: on */
    overflow_count = 0;
    microkit_dbg_puts("[trace_recorder] stub ready — recording enabled\n");
}

void notified(microkit_channel ch) {
    /* Each notification = one inter-PD dispatch event from controller.
     * MR0 holds the packed src/dst/label word set by trace_notify(). */
    if (ch == CH_CTRL_NOTIFY && recording) {
        event_count++;
        /* Full impl would read MR0-MR2 and append to a ring buffer here */
    }
    (void)ch;
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msg) {
    uint32_t op = (uint32_t)microkit_mr_get(0);
    (void)ch; (void)msg;

    switch (op) {
        case OP_TRACE_START:
            recording = true;
            microkit_mr_set(0, 0);
            return microkit_msginfo_new(0, 1);

        case OP_TRACE_STOP:
            recording = false;
            microkit_mr_set(0, 0);
            return microkit_msginfo_new(0, 1);

        case OP_TRACE_QUERY:
            /* MR0=status, MR1=event_count, MR2=bytes_used (0 in stub) */
            microkit_mr_set(0, 0);
            microkit_mr_set(1, event_count);
            microkit_mr_set(2, 0);
            return microkit_msginfo_new(0, 3);

        case OP_TRACE_DUMP:
            /* Stub: nothing to dump — return zero entries */
            microkit_mr_set(0, 0);
            microkit_mr_set(1, 0);
            return microkit_msginfo_new(0, 2);

        default:
            microkit_mr_set(0, 0xFF);
            return microkit_msginfo_new(0, 1);
    }
}
