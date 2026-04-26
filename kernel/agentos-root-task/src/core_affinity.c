/*
 * agentOS Core Affinity Protection Domain
 *
 * Passive PD (priority 200). Manages a static core-assignment table that pins
 * long-running GPU inference agents to dedicated seL4 cores on sparky GB10
 * (28-core ARM), preventing scheduling interference between GPU agents and
 * background PDs.
 *
 * GB10 core topology:
 *   [0,  3]  — GPU-adjacent performance cores (Cortex-A78)
 *   [4, 19]  — General compute cores (Cortex-A78)
 *   [20, 27] — ARM efficiency cores (Cortex-A55)
 *
 * Messages handled in protected() — MR0 = opcode:
 *   OP_AFFINITY_PIN      (0xB0): MR1=slot_id, MR2=core_id, MR3=flags
 *   OP_AFFINITY_UNPIN    (0xB1): MR1=slot_id
 *   OP_AFFINITY_STATUS   (0xB2): returns packed assignment table
 *   OP_AFFINITY_RESERVE  (0xB3): MR1=core_id
 *   OP_AFFINITY_UNRESERVE(0xB4): MR1=core_id
 *   OP_AFFINITY_SUGGEST  (0xB5): MR1=slot_priority, MR2=flags → MR0=core_id
 *   OP_AFFINITY_RESET    (0xB6): clear all state
 *
 * time_partition integration:
 *   On OP_AFFINITY_PIN, a PPC is issued to time_partition via
 *   CA_CH_TIME_PARTITION (OP_TP_CONFIGURE) to restrict the slot's scheduling
 *   class to CLASS_GPU_WORKER or CLASS_BACKGROUND based on flags.
 *
 * Channel assignments (agentos.system):
 *   id=0: controller <-> core_affinity  (controller pp=true)
 *   id=1: core_affinity <-> time_partition (core_affinity pp=true)
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "sel4_server.h"
#include "core_affinity.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* time_partition opcodes and class IDs (mirrored from time_partition.c) */
#define OP_TP_CONFIGURE   0xD0u
#define TP_CLASS_GPU      0u    /* CLASS_GPU_WORKER */
#define TP_CLASS_BG       4u    /* CLASS_BACKGROUND */

/* ── State ───────────────────────────────────────────────────────────────── */

static CoreAffinityState g_state;
static uint32_t          g_tick = 0;   /* monotonic op counter used as tick proxy */

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static CoreAssignment *find_slot(uint8_t slot_id)
{
    for (uint8_t i = 0; i < g_state.slot_count; i++) {
        if (g_state.slots[i].slot_id == slot_id)
            return &g_state.slots[i];
    }
    return NULL;
}

/* Count how many slots are currently assigned to core_id. */
static uint32_t core_load(uint8_t core_id)
{
    uint32_t n = 0;
    for (uint8_t i = 0; i < g_state.slot_count; i++) {
        if (g_state.slots[i].core_id == core_id)
            n++;
    }
    return n;
}

/*
 * Find the least-loaded core within [lo, hi] (inclusive).
 * If exclusive is set, require core_load == 0 and not reserved by another slot.
 * Returns MAX_CORES on failure (no suitable core).
 */
static uint8_t pick_core(uint8_t lo, uint8_t hi, bool exclusive)
{
    uint8_t  best_core  = MAX_CORES;
    uint32_t best_load  = UINT32_MAX;

    for (uint8_t c = lo; c <= hi && c < MAX_CORES; c++) {
        if (g_state.reserved_cores[c]) continue;  /* already reserved */
        uint32_t load = core_load(c);
        if (exclusive && load > 0) continue;       /* must be empty */
        if (load < best_load) {
            best_load  = load;
            best_core  = c;
        }
    }
    return best_core;
}

/*
 * Notify time_partition about the slot's new scheduling class.
 * Issued as a PPC on CA_CH_TIME_PARTITION.
 * If the channel is not wired (non-GB10 build), this is a no-op.
 */
static void notify_time_partition(uint8_t slot_id, uint32_t flags)
{
    IPC_STUB_LOCALS
    uint32_t tp_class = (flags & CORE_FLAG_GPU) ? TP_CLASS_GPU : TP_CLASS_BG;
    rep_u32(rep, 4, (uint32_t)slot_id);
    rep_u32(rep, 8, tp_class);
    /* E5-S8: ppcall stubbed */
}

/* ── Microkit entry points ───────────────────────────────────────────────── */

static void core_affinity_pd_init(void)
{
    agentos_log_boot("core_affinity");
    memset(&g_state, 0, sizeof(g_state));
    sel4_dbg_puts("[core_affinity] GB10 core affinity table initialised\n");
    sel4_dbg_puts("[core_affinity] GPU cores [0-3], BG cores [20-27], "
                      "MAX_SLOTS=16, MAX_CORES=32\n");
}

static void core_affinity_pd_notified(uint32_t ch)
{
    /* Passive PD — no unsolicited notifications expected. */
    (void)ch;
}

static uint32_t core_affinity_h_dispatch(sel4_badge_t b, const sel4_msg_t *req, sel4_msg_t *rep, void *ctx)
{
    (void)b; (void)ctx;
    g_tick++;

    uint32_t op   = (uint32_t)msg_u32(req, 0);
    uint32_t arg1 = (uint32_t)msg_u32(req, 4);
    uint32_t arg2 = (uint32_t)msg_u32(req, 8);
    uint32_t arg3 = (uint32_t)msg_u32(req, 12);

    switch (op) {

    /* ── OP_AFFINITY_PIN ───────────────────────────────────────────────── */
    case OP_AFFINITY_PIN: {
        uint8_t  slot_id   = (uint8_t)arg1;
        uint8_t  core_id   = (uint8_t)arg2;
        uint32_t flags     = arg3;
        bool     exclusive = !!(flags & CORE_FLAG_EXCLUSIVE);

        if (core_id >= MAX_CORES) {
            rep_u32(rep, 0, CA_ERR_INVAL);
            rep->length = 4;
        return SEL4_ERR_OK;
        }

        /* Reject if core is reserved by someone else and we're not its owner. */
        if (g_state.reserved_cores[core_id] && !exclusive) {
            rep_u32(rep, 0, CA_ERR_BUSY);
            rep->length = 4;
        return SEL4_ERR_OK;
        }

        CoreAssignment *entry = find_slot(slot_id);
        if (entry) {
            /* Slot already pinned — update (counts as a migration if core changed). */
            if (entry->core_id != core_id)
                g_state.migrations++;
            entry->core_id       = core_id;
            entry->exclusive     = exclusive ? 1u : 0u;
            entry->pinned_at_tick = g_tick;
            entry->flags         = flags;
        } else {
            /* New assignment. */
            if (g_state.slot_count >= MAX_SLOTS) {
                rep_u32(rep, 0, CA_ERR_FULL);
                rep->length = 4;
        return SEL4_ERR_OK;
            }
            entry = &g_state.slots[g_state.slot_count++];
            entry->slot_id        = slot_id;
            entry->core_id        = core_id;
            entry->exclusive      = exclusive ? 1u : 0u;
            entry->pinned_at_tick = g_tick;
            entry->flags          = flags;
        }

        /* Mark core reserved if exclusive. */
        if (exclusive)
            g_state.reserved_cores[core_id] = 1u;

        /* Notify time_partition to restrict this slot's scheduling budget. */
        notify_time_partition(slot_id, flags);

        rep_u32(rep, 0, CA_OK);
        rep_u32(rep, 4, (uint32_t)core_id);
        rep->length = 8;
        return SEL4_ERR_OK;
    }

    /* ── OP_AFFINITY_UNPIN ─────────────────────────────────────────────── */
    case OP_AFFINITY_UNPIN: {
        uint8_t slot_id = (uint8_t)arg1;
        CoreAssignment *entry = find_slot(slot_id);
        if (!entry) {
            rep_u32(rep, 0, CA_ERR_NOENT);
            rep->length = 4;
        return SEL4_ERR_OK;
        }

        /* Release exclusive reservation if this slot held it. */
        if (entry->exclusive)
            g_state.reserved_cores[entry->core_id] = 0u;

        /* Compact table: move last entry into this slot's position. */
        uint8_t idx = (uint8_t)(entry - g_state.slots);
        if (idx < g_state.slot_count - 1u)
            g_state.slots[idx] = g_state.slots[g_state.slot_count - 1u];
        g_state.slot_count--;

        rep_u32(rep, 0, CA_OK);
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    /* ── OP_AFFINITY_STATUS ────────────────────────────────────────────── */
    case OP_AFFINITY_STATUS: {
        /*
         * Returns:
         *   MR0 = CA_OK
         *   MR1 = slot_count
         *   MR2 = migrations
         *   MR(3 + i*3 + 0) = slot_id       for i in [0, slot_count)
         *   MR(3 + i*3 + 1) = core_id
         *   MR(3 + i*3 + 2) = pinned_at_tick
         *
         * Callers should not request status when slot_count > 16 entries as
         * MR capacity limits the response to MAX_SLOTS (16) slots.
         */
        rep_u32(rep, 0, CA_OK);
        rep_u32(rep, 4, (uint32_t)g_state.slot_count);
        rep_u32(rep, 8, g_state.migrations);
        uint32_t n = g_state.slot_count;
        if (n > MAX_SLOTS) n = MAX_SLOTS;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t base = 3u + i * 3u;
            rep_u32(rep, (base + 0u) * 4, (uint32_t)g_state.slots[i].slot_id);
            rep_u32(rep, (base + 1u) * 4, (uint32_t)g_state.slots[i].core_id);
            rep_u32(rep, (base + 2u) * 4, g_state.slots[i].pinned_at_tick);
        }
        rep->length = (3u + n * 3u) * 4u;
        return SEL4_ERR_OK;
    }

    /* ── OP_AFFINITY_RESERVE ───────────────────────────────────────────── */
    case OP_AFFINITY_RESERVE: {
        uint8_t core_id = (uint8_t)arg1;
        if (core_id >= MAX_CORES) {
            rep_u32(rep, 0, CA_ERR_INVAL);
            rep->length = 4;
        return SEL4_ERR_OK;
        }
        if (g_state.reserved_cores[core_id]) {
            rep_u32(rep, 0, CA_ERR_BUSY);
            rep->length = 4;
        return SEL4_ERR_OK;
        }
        g_state.reserved_cores[core_id] = 1u;
        rep_u32(rep, 0, CA_OK);
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    /* ── OP_AFFINITY_UNRESERVE ─────────────────────────────────────────── */
    case OP_AFFINITY_UNRESERVE: {
        uint8_t core_id = (uint8_t)arg1;
        if (core_id >= MAX_CORES) {
            rep_u32(rep, 0, CA_ERR_INVAL);
            rep->length = 4;
        return SEL4_ERR_OK;
        }
        g_state.reserved_cores[core_id] = 0u;
        rep_u32(rep, 0, CA_OK);
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    /* ── OP_AFFINITY_SUGGEST ───────────────────────────────────────────── */
    case OP_AFFINITY_SUGGEST: {
        /*
         * MR1 = slot_priority (unused in current heuristic, reserved)
         * MR2 = flags: CORE_FLAG_GPU | CORE_FLAG_BG | CORE_FLAG_EXCLUSIVE
         *
         * Policy:
         *   GPU flag set  → search [GB10_GPU_CORE_LO, GB10_GPU_CORE_HI]
         *   BG flag set   → search [GB10_BG_CORE_LO,  GB10_BG_CORE_HI]
         *   Neither       → search full range [0, MAX_CORES-1]
         *   Exclusive     → require core_load == 0
         *   Otherwise     → least-loaded core in range
         */
        (void)arg1; /* slot_priority reserved */
        uint32_t flags     = arg2;
        bool     exclusive = !!(flags & CORE_FLAG_EXCLUSIVE);
        bool     gpu       = !!(flags & CORE_FLAG_GPU);
        bool     bg        = !!(flags & CORE_FLAG_BG);

        uint8_t lo, hi;
        if (gpu) {
            lo = GB10_GPU_CORE_LO;
            hi = GB10_GPU_CORE_HI;
        } else if (bg) {
            lo = GB10_BG_CORE_LO;
            hi = GB10_BG_CORE_HI;
        } else {
            lo = 0u;
            hi = (uint8_t)(MAX_CORES - 1u);
        }

        uint8_t suggested = pick_core(lo, hi, exclusive);
        if (suggested >= MAX_CORES) {
            /* Widen search to full range as fallback (non-exclusive only). */
            if (!exclusive)
                suggested = pick_core(0, (uint8_t)(MAX_CORES - 1u), false);
        }

        if (suggested >= MAX_CORES) {
            rep_u32(rep, 0, CA_ERR_NOCORE);
            rep->length = 4;
        return SEL4_ERR_OK;
        }

        rep_u32(rep, 0, CA_OK);
        rep_u32(rep, 4, (uint32_t)suggested);
        rep_u32(rep, 8, core_load(suggested));
        rep->length = 12;
        return SEL4_ERR_OK;
    }

    /* ── OP_AFFINITY_RESET ─────────────────────────────────────────────── */
    case OP_AFFINITY_RESET: {
        memset(&g_state, 0, sizeof(g_state));
        g_tick = 0;
        rep_u32(rep, 0, CA_OK);
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    default:
        rep_u32(rep, 0, 0xDEADu);
        rep->length = 4;
        return SEL4_ERR_OK;
    }
}

/* ── E5-S8: Entry point ─────────────────────────────────────────────────── */
void core_affinity_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    (void)ns_ep;
    core_affinity_pd_init();
    static sel4_server_t srv;
    sel4_server_init(&srv, my_ep);
    /* Dispatch all opcodes through the generic handler */
    sel4_server_register(&srv, SEL4_SERVER_OPCODE_ANY, core_affinity_h_dispatch, (void *)0);
    sel4_server_run(&srv);
}
