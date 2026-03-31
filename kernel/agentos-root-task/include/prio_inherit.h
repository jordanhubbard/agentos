/*
 * prio_inherit.h — Explicit priority donation for passive PD PPC calls
 *
 * PROBLEM
 * -------
 * In seL4 Microkit, passive PDs run on the *caller's* scheduling context.
 * A low-priority PD (e.g. controller, prio 50) calling a passive PD
 * (e.g. event_bus, prio 200) causes the server to execute at priority 50.
 * Any thread at priority 51–199 can preempt the server mid-call, producing
 * priority inversion: a worker (prio 80) blocks a controller→event_bus PPC.
 *
 * SOLUTION
 * --------
 * Before calling a passive PD whose declared priority exceeds the caller's,
 * raise the calling thread's scheduling context priority to max(caller, server)
 * for the duration of the call, then restore it on return.
 *
 * seL4 mechanism: seL4_TCB_SetMCPriority on the caller's own TCB, or
 * (on Microkit) via seL4_SchedContext_SetPriority on the bound SchedContext.
 * We use seL4_TCB_SetSchedParams which is available in seL4 Microkit 2.x.
 *
 * USAGE
 * -----
 *   #include "prio_inherit.h"
 *
 *   // Instead of:
 *   microkit_msginfo reply = microkit_ppcall(CH_EVENTBUS, msg);
 *
 *   // Use:
 *   microkit_msginfo reply = ppcall_with_prio(CH_EVENTBUS, msg,
 *                                              MY_PRIORITY, PRIO_EVENTBUS);
 *
 * Or use the convenience macro that reads caller priority from a constant:
 *   PPCALL_DONATE(CH_EVENTBUS, msg, PRIO_CONTROLLER, PRIO_EVENTBUS)
 *
 * seL4 REFERENCE: seL4 Reference Manual §6.3 (Priority Inheritance);
 *                 Microkit SDK seL4_SchedContext_SetPriority (MCS kernel).
 *
 * LIMITATIONS
 * -----------
 * - Requires MCS kernel (seL4 with Mixed-Criticality Scheduling extensions).
 *   On a standard seL4 kernel, the fallback is a direct microkit_ppcall.
 * - seL4_TCB_SetSchedParams requires authority capability.  In Microkit,
 *   the root-task holds the TCB cap for all PDs; the monitor PD must be
 *   the one to perform the boost.  If called from a PD without TCB cap,
 *   the boost silently falls back to the unadorned ppcall.
 * - Priority restoration is best-effort: if the callee returns an error or
 *   the PPC is interrupted, the restored priority may not be guaranteed.
 *   Use within a single linear control path only.
 */

#ifndef PRIO_INHERIT_H
#define PRIO_INHERIT_H

#include <stdint.h>
#include "agentos.h"   /* agentos_priority_t, microkit_ppcall */

/* ── seL4 MCS priority-donation API ─────────────────────────────────────── */

/*
 * agentos_self_tcb_cap() — returns this PD's own TCB capability.
 *
 * In seL4 Microkit, the Microkit SDK exposes a per-PD TCB cap as
 * MICROKIT_TCB_CAP (a slot in the CNode).  We use it to call
 * seL4_TCB_SetSchedParams with a raised budget.
 *
 * If MICROKIT_TCB_CAP is not defined (older SDK), priority donation is a
 * compile-time no-op and the call proceeds at the caller's native priority.
 */

#ifdef MICROKIT_TCB_CAP

#include <sel4/sel4.h>   /* seL4_TCB_SetSchedParams, seL4_Word */

/*
 * prio_donate_begin() — raise this thread's priority to `server_prio`
 *                        if server_prio > caller_prio.
 * Returns the original priority for restoration by prio_donate_end().
 */
static inline uint8_t prio_donate_begin(uint8_t caller_prio,
                                         uint8_t server_prio) {
    if (server_prio <= caller_prio) return caller_prio;   /* no inversion */

    /*
     * seL4_TCB_SetSchedParams(tcb, auth, mcp, priority)
     * We boost only the *priority* (scheduling priority), not the budget.
     * MCP (max controlled priority) must be >= the new priority; use 255.
     */
    seL4_TCB_SetSchedParams(
        MICROKIT_TCB_CAP,   /* our own TCB cap (granted by Microkit) */
        MICROKIT_TCB_CAP,   /* authority: self (sufficient for own boost) */
        (seL4_Word)255,     /* mcp — keep at maximum (was set by root-task) */
        (seL4_Word)server_prio
    );
    return caller_prio;     /* caller saves original priority */
}

/*
 * prio_donate_end() — restore priority to `saved_prio` after the PPC
 *                     call returns.
 */
static inline void prio_donate_end(uint8_t caller_prio,
                                    uint8_t saved_prio) {
    if (caller_prio == saved_prio) return;   /* was not boosted */

    seL4_TCB_SetSchedParams(
        MICROKIT_TCB_CAP,
        MICROKIT_TCB_CAP,
        (seL4_Word)255,
        (seL4_Word)saved_prio
    );
}

#else /* !MICROKIT_TCB_CAP — no TCB cap, donation is a no-op */

static inline uint8_t prio_donate_begin(uint8_t caller_prio,
                                         uint8_t server_prio) {
    (void)server_prio;
    return caller_prio;
}

static inline void prio_donate_end(uint8_t caller_prio,
                                    uint8_t saved_prio) {
    (void)caller_prio; (void)saved_prio;
}

#endif /* MICROKIT_TCB_CAP */

/* ── High-level wrappers ─────────────────────────────────────────────────── */

/*
 * ppcall_with_prio() — priority-donating PPC call
 *
 * Raises the caller's thread priority to max(caller_prio, server_prio)
 * for the duration of the seL4 protected procedure call, then restores.
 *
 * This prevents a mid-priority thread from preempting the passive server
 * PD during the call window — the key source of priority inversion in
 * the agentOS PPC graph.
 *
 *   ch          — channel id (as defined in the SDF .system file)
 *   msg         — microkit_msginfo constructed before the call
 *   caller_prio — this PD's compile-time priority constant
 *   server_prio — target passive PD's compile-time priority constant
 */
static inline microkit_msginfo
ppcall_with_prio(microkit_channel ch,
                 microkit_msginfo  msg,
                 uint8_t           caller_prio,
                 uint8_t           server_prio) {
    uint8_t saved = prio_donate_begin(caller_prio, server_prio);
    microkit_msginfo reply = microkit_ppcall(ch, msg);
    prio_donate_end(caller_prio, saved);
    return reply;
}

/* ── Convenience macros ──────────────────────────────────────────────────── */

/*
 * PPCALL_DONATE(ch, msg, caller_prio, server_prio)
 *
 * Drop-in replacement for microkit_ppcall when the server's priority
 * may exceed the caller's.  All arguments are evaluated once.
 *
 * Example:
 *   // controller (prio 50) → event_bus (prio 200)
 *   PPCALL_DONATE(CH_EVENTBUS, microkit_msginfo_new(EVT_SUBSCRIBE, 0),
 *                 PRIO_CONTROLLER, PRIO_EVENTBUS)
 *
 *   // init_agent (prio 100) → vibe_engine (prio 140)
 *   PPCALL_DONATE(CH_VIBE, microkit_msginfo_new(OP_VIBE_PROPOSE, 3),
 *                 PRIO_INIT_AGENT, PRIO_VIBE_ENGINE)
 */
#define PPCALL_DONATE(ch, msg, caller_prio, server_prio) \
    ppcall_with_prio((ch), (msg), (uint8_t)(caller_prio), (uint8_t)(server_prio))

/*
 * PD-local priority constants (mirror agentos.system priorities).
 * Include this header in each PD that makes PPC calls.
 */
#define PRIO_CONTROLLER   50
#define PRIO_SWAP_SLOT    75
#define PRIO_WORKER       80
#define PRIO_INIT_AGENT   100
#define PRIO_MEM_PROFILER 108
#define PRIO_VIBE_ENGINE  140
#define PRIO_AGENTFS      150
#define PRIO_CONSOLE_MUX  160
#define PRIO_EVENTBUS     200
#define PRIO_MONITOR      254

#endif /* PRIO_INHERIT_H */
