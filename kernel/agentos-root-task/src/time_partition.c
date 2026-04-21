/*
 * agentOS Time-Partitioning Scheduler Protection Domain
 *
 * Passive PD (priority 250 — highest user-space priority, below seL4 kernel).
 * Enforces fixed CPU budget windows per agent class using seL4 MCS scheduling
 * contexts, preventing bursty GPU agents from starving the watchdog heartbeat.
 *
 * Architecture
 * ────────────
 * seL4 MCS (Mixed-Criticality Scheduling) assigns each thread a scheduling
 * context (SchedContext) with a periodic budget: `budget_us` CPU microseconds
 * every `period_us`.  When a thread exhausts its budget it is blocked until
 * the next period begins.
 *
 * This PD manages a policy table mapping agent class → {budget_us, period_us}.
 * PDs call OP_TP_CONFIGURE at startup to register their class; time_partition
 * calls seL4_SchedContext_SetPeriodAndBudget on their bound SchedContext.
 *
 * Policy classes (defaults, overridable via OP_TP_SET_POLICY):
 *   CLASS_GPU_WORKER:   budget=5000µs / period=10000µs   (50% CPU, 10ms quantum)
 *   CLASS_MESH_AGENT:   budget=2000µs / period=10000µs   (20%)
 *   CLASS_WATCHDOG:     budget=1000µs / period= 5000µs   (20%, 5ms period)
 *   CLASS_INIT_AGENT:   budget= 500µs / period=10000µs   ( 5%)
 *   CLASS_BACKGROUND:   budget= 200µs / period=10000µs   ( 2%)
 *
 * cap_policy DSL binding (from cap_policy.nano):
 *   agent gpu_worker { cpu_budget_us: 5000; period_us: 10000; }
 *   agent mesh_agent { cpu_budget_us: 2000; period_us: 10000; }
 *   agent watchdog   { cpu_budget_us: 1000; period_us:  5000; }
 *
 * Messages (MR0 opcode):
 *   OP_TP_CONFIGURE    (0xD0): MR1=pd_id, MR2=class_id → ok
 *   OP_TP_SET_POLICY   (0xD1): MR1=class_id, MR2=budget_us (lo), MR3=period_us (lo) → ok
 *   OP_TP_GET_POLICY   (0xD2): MR1=class_id → budget_us (MR0), period_us (MR1)
 *   OP_TP_SUSPEND      (0xD3): MR1=pd_id → immediately suspend until next period
 *   OP_TP_QUERY        (0xD4): MR1=pd_id → remaining_budget_us (MR0), class_id (MR1)
 *   OP_TP_TICK         (0xD5): sent by controller each scheduler tick
 *   OP_TP_RESET        (0xD6): clear all registrations (test harnesses)
 *
 * Channel assignments (agentos.system):
 *   id=0: controller <-> time_partition (pp=true on controller end)
 *   id=1..8: worker_0..7 <-> time_partition (pp=true on worker end)
 *   id=9: init_agent <-> time_partition (pp=true)
 *
 * seL4 capability requirements:
 *   time_partition must hold a SchedContext cap for each managed PD.
 *   In Microkit, the root-task grants SchedContext caps at boot via the
 *   system description.  We model this with MICROKIT_SC_CAP_BASE + pd_id.
 *
 * On non-MCS seL4 (or QEMU without SchedContext support), the seL4 calls
 * are compiled away via AGENTOS_NO_MCS guard; the policy table is maintained
 * but enforcement is no-op.
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "contracts/time_partition_contract.h"
#include "prio_inherit.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifdef MICROKIT_SC_CAP_BASE
#  include <sel4/sel4.h>
#  define TP_MCS_AVAILABLE 1
#else
#  define TP_MCS_AVAILABLE 0
#endif

/* ── Configuration ─────────────────────────────────────────────────────── */

#define MAX_PDS          32
#define MAX_CLASSES       8

/* Agent class IDs */
#define CLASS_GPU_WORKER   0
#define CLASS_MESH_AGENT   1
#define CLASS_WATCHDOG     2
#define CLASS_INIT_AGENT   3
#define CLASS_BACKGROUND   4
#define CLASS_CUSTOM       5   /* user-defined via OP_TP_SET_POLICY */

/* Opcodes */
#define OP_TP_CONFIGURE    0xD0
#define OP_TP_SET_POLICY   0xD1
#define OP_TP_GET_POLICY   0xD2
#define OP_TP_SUSPEND      0xD3
#define OP_TP_QUERY        0xD4
#define OP_TP_TICK         0xD5
#define OP_TP_RESET        0xD6

/* ── Data structures ───────────────────────────────────────────────────── */

typedef struct {
    uint32_t budget_us;    /* CPU microseconds per period */
    uint32_t period_us;    /* scheduling period in microseconds */
    char     name[32];     /* class name for diagnostics */
} TPClass;

typedef struct {
    uint32_t pd_id;
    uint8_t  class_id;
    bool     registered;
    uint32_t remaining_budget_us;   /* decremented each tick */
    uint32_t ticks_this_period;
    bool     suspended;
} TPEntry;

/* Policy table */
static TPClass tp_classes[MAX_CLASSES] = {
    [CLASS_GPU_WORKER]  = { 5000, 10000, "gpu_worker" },
    [CLASS_MESH_AGENT]  = { 2000, 10000, "mesh_agent" },
    [CLASS_WATCHDOG]    = { 1000,  5000, "watchdog"   },
    [CLASS_INIT_AGENT]  = {  500, 10000, "init_agent" },
    [CLASS_BACKGROUND]  = {  200, 10000, "background" },
    [CLASS_CUSTOM]      = {    0,     0, "custom"     },
};

/* Registration table */
static TPEntry tp_table[MAX_PDS];
static uint32_t tp_count    = 0;
static uint64_t tick_count  = 0;

/* ── seL4 MCS enforcement ──────────────────────────────────────────────── */

#if TP_MCS_AVAILABLE

static seL4_CPtr sc_cap_for_pd(uint32_t pd_id) {
    /* Microkit assigns SchedContext caps at MICROKIT_SC_CAP_BASE + pd_id.
     * This is a convention established in the root-task capability space. */
    return (seL4_CPtr)(MICROKIT_SC_CAP_BASE + pd_id);
}

static void tp_enforce_policy(TPEntry *e) {
    if (!e || !e->registered) return;
    const TPClass *cls = &tp_classes[e->class_id];
    if (cls->budget_us == 0 || cls->period_us == 0) return;

    seL4_CPtr sc = sc_cap_for_pd(e->pd_id);

    /* seL4_SchedContext_SetPeriodAndBudget(sc, period_ns, budget_ns) */
    uint64_t period_ns = (uint64_t)cls->period_us * 1000ULL;
    uint64_t budget_ns = (uint64_t)cls->budget_us * 1000ULL;

    /* seL4_SchedContext_Bind / seL4_SchedContext_SetPeriodAndBudget */
    seL4_Error err = seL4_SchedContext_SetPeriodAndBudget(
        sc, period_ns, budget_ns);
    if (err != seL4_NoError) {
        microkit_dbg_puts("[time_partition] WARNING: seL4_SchedContext_SetPeriodAndBudget failed\n");
    }
}

static void tp_suspend_pd(TPEntry *e) {
    if (!e || !e->registered || e->suspended) return;
    seL4_CPtr sc = sc_cap_for_pd(e->pd_id);
    /* Drain budget to zero to force suspension until next period.
     * This is done by setting budget to 0 temporarily. */
    seL4_SchedContext_SetPeriodAndBudget(sc,
        (uint64_t)tp_classes[e->class_id].period_us * 1000ULL, 0);
    e->suspended = true;
}

static void tp_restore_pd(TPEntry *e) {
    if (!e || !e->registered || !e->suspended) return;
    tp_enforce_policy(e);
    e->suspended = false;
    e->remaining_budget_us = tp_classes[e->class_id].budget_us;
}

#else /* !TP_MCS_AVAILABLE — simulation mode */

static void tp_enforce_policy(TPEntry *e) { (void)e; }
static void tp_suspend_pd(TPEntry *e) { if (e) e->suspended = true; }
static void tp_restore_pd(TPEntry *e) {
    if (e) {
        e->suspended = false;
        e->remaining_budget_us = e->class_id < MAX_CLASSES
            ? tp_classes[e->class_id].budget_us : 0;
    }
}

#endif /* TP_MCS_AVAILABLE */

/* ── Helpers ───────────────────────────────────────────────────────────── */

static TPEntry *find_pd(uint32_t pd_id) {
    for (uint32_t i = 0; i < tp_count; i++) {
        if (tp_table[i].registered && tp_table[i].pd_id == pd_id)
            return &tp_table[i];
    }
    return NULL;
}

static TPEntry *alloc_pd(uint32_t pd_id) {
    if (tp_count >= MAX_PDS) return NULL;
    TPEntry *e = &tp_table[tp_count++];
    memset(e, 0, sizeof(*e));
    e->pd_id = pd_id;
    return e;
}

/* ── Tick handler — called by controller each scheduling quantum ─────────── */
/*
 * Approximate budget tracking.  Each tick represents one scheduling quantum
 * (the quantum length in µs is passed in MR2 or assumed to be 500µs).
 */
static void handle_tick(uint32_t quantum_us) {
    tick_count++;
    for (uint32_t i = 0; i < tp_count; i++) {
        TPEntry *e = &tp_table[i];
        if (!e->registered) continue;
        const TPClass *cls = &tp_classes[e->class_id];

        /* Period rollover: restore suspended PDs at period boundary */
        uint32_t ticks_per_period = cls->period_us / (quantum_us > 0 ? quantum_us : 500);
        if (ticks_per_period < 1) ticks_per_period = 1;
        e->ticks_this_period++;
        if (e->ticks_this_period >= ticks_per_period) {
            e->ticks_this_period   = 0;
            if (e->suspended) tp_restore_pd(e);
            else e->remaining_budget_us = cls->budget_us;
        }
        /* Budget exhaustion: suspend if no MCS (MCS kernel handles this itself) */
        if (!TP_MCS_AVAILABLE) {
            if (e->remaining_budget_us > quantum_us)
                e->remaining_budget_us -= quantum_us;
            else if (!e->suspended) {
                tp_suspend_pd(e);
            }
        }
    }
}

/* ── Microkit entry points ─────────────────────────────────────────────── */

void init(void) {
    agentos_log_boot("time_partition");
    memset(tp_table, 0, sizeof(tp_table));
    microkit_dbg_puts("[time_partition] Scheduling policies initialized\n");
    microkit_dbg_puts("[time_partition] Classes: gpu_worker(5ms/10ms) mesh_agent(2ms/10ms)"
                      " watchdog(1ms/5ms)\n");
#if TP_MCS_AVAILABLE
    microkit_dbg_puts("[time_partition] seL4 MCS enforcement: ENABLED\n");
#else
    microkit_dbg_puts("[time_partition] seL4 MCS enforcement: SIMULATION (non-MCS kernel)\n");
#endif
}

void notified(microkit_channel ch) {
    /* Time partitioner is passive — notifications come from controller tick */
    (void)ch;
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo) {
    (void)ch;
    uint32_t op     = (uint32_t)microkit_msginfo_get_label(msginfo);
    uint32_t arg1   = (uint32_t)microkit_mr_get(1);
    uint32_t arg2   = (uint32_t)microkit_mr_get(2);
    uint32_t arg3   = (uint32_t)microkit_mr_get(3);

    switch (op) {
        case OP_TP_CONFIGURE: {
            /* MR1=pd_id, MR2=class_id */
            uint32_t pd_id    = arg1;
            uint8_t  class_id = (uint8_t)(arg2 < MAX_CLASSES ? arg2 : CLASS_BACKGROUND);
            TPEntry *e = find_pd(pd_id);
            if (!e) e = alloc_pd(pd_id);
            if (!e) {
                microkit_mr_set(0, 0);
                return microkit_msginfo_new(1, 1);
            }
            e->registered          = true;
            e->class_id            = class_id;
            e->remaining_budget_us = tp_classes[class_id].budget_us;
            e->suspended           = false;
            tp_enforce_policy(e);
            microkit_mr_set(0, 1);  /* ok */
            return microkit_msginfo_new(0, 1);
        }

        case OP_TP_SET_POLICY: {
            /* MR1=class_id, MR2=budget_us, MR3=period_us */
            uint8_t class_id = (uint8_t)(arg1 < MAX_CLASSES ? arg1 : CLASS_CUSTOM);
            tp_classes[class_id].budget_us = arg2 ? arg2 : tp_classes[class_id].budget_us;
            tp_classes[class_id].period_us = arg3 ? arg3 : tp_classes[class_id].period_us;
            /* Re-enforce for all registered PDs of this class */
            for (uint32_t i = 0; i < tp_count; i++) {
                if (tp_table[i].registered && tp_table[i].class_id == class_id)
                    tp_enforce_policy(&tp_table[i]);
            }
            microkit_mr_set(0, 1);
            return microkit_msginfo_new(0, 1);
        }

        case OP_TP_GET_POLICY: {
            /* MR1=class_id → MR0=budget_us, MR1=period_us */
            uint8_t class_id = (uint8_t)(arg1 < MAX_CLASSES ? arg1 : 0);
            microkit_mr_set(0, tp_classes[class_id].budget_us);
            microkit_mr_set(1, tp_classes[class_id].period_us);
            return microkit_msginfo_new(0, 2);
        }

        case OP_TP_SUSPEND: {
            /* MR1=pd_id */
            TPEntry *e = find_pd(arg1);
            if (e) tp_suspend_pd(e);
            microkit_mr_set(0, e ? 1 : 0);
            return microkit_msginfo_new(0, 1);
        }

        case OP_TP_QUERY: {
            /* MR1=pd_id → MR0=remaining_budget_us, MR1=class_id */
            TPEntry *e = find_pd(arg1);
            microkit_mr_set(0, e ? e->remaining_budget_us : 0);
            microkit_mr_set(1, e ? (uint32_t)e->class_id : 0xFF);
            microkit_mr_set(2, e ? (uint32_t)e->suspended : 0);
            return microkit_msginfo_new(0, 3);
        }

        case OP_TP_TICK: {
            /* MR1=quantum_us (0 = default 500µs) */
            uint32_t quantum = arg1 ? arg1 : 500;
            handle_tick(quantum);
            microkit_mr_set(0, (uint32_t)(tick_count & 0xFFFFFFFF));
            return microkit_msginfo_new(0, 1);
        }

        case OP_TP_RESET: {
            memset(tp_table, 0, sizeof(tp_table));
            tp_count   = 0;
            tick_count = 0;
            microkit_mr_set(0, 1);
            return microkit_msginfo_new(0, 1);
        }

        default:
            microkit_mr_set(0, 0xDEAD);
            return microkit_msginfo_new(1, 1);
    }
}
