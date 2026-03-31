/*
 * agentOS watchdog_pd — per-slot heartbeat monitoring with hot-reload freeze
 *
 * Passive PD, priority 107 (below mem_profiler at 108).
 *
 * Each running swap slot registers with the watchdog and must call
 * OP_WD_HEARTBEAT within its configured timeout window.  If a heartbeat
 * is missed the watchdog notifies the controller, which respawns the slot.
 *
 * OP_WD_FREEZE suspends monitoring for a slot during hot-reload so that a
 * legitimate pause (new module loading, memory snapshot/restore) does not
 * trigger a false-positive timeout alert.
 *
 * Protocol (MR0 = op code):
 *   OP_WD_REGISTER   (0x50) — MR1=slot_id, MR2=heartbeat_ticks
 *                             → MR0=WD_OK | WD_ERR_FULL
 *   OP_WD_HEARTBEAT  (0x51) — MR1=slot_id
 *                             → MR0=WD_OK | WD_ERR_NOENT
 *   OP_WD_STATUS     (0x52) — MR1=slot_id
 *                             → MR0=WD_OK, MR1=state, MR2=ticks_remaining
 *   OP_WD_UNREGISTER (0x53) — MR1=slot_id
 *                             → MR0=WD_OK | WD_ERR_NOENT
 *   OP_WD_FREEZE     (0x54) — MR1=slot_id
 *                             → MR0=WD_OK | WD_ERR_NOENT
 *                             Suspends timeout countdown for slot during
 *                             hot-reload.  Monitoring resumes on OP_WD_RESUME.
 *   OP_WD_RESUME     (0x55) — MR1=slot_id, MR2=new_module_hash_lo (u32)
 *                             → MR0=WD_OK | WD_ERR_NOENT
 *                             Resumes monitoring after hot-reload completes.
 *                             Resets timeout counter; records new module hash.
 *
 * On timeout: notify controller via CH_TIMEOUT (id=1), MR0=slot_id.
 *
 * Channels (from watchdog_pd perspective):
 *   id=0 CH_IN:      controller PPCs in (register / heartbeat / freeze / resume)
 *   id=1 CH_TIMEOUT: notify controller when slot heartbeat times out
 *
 * State: table[WD_MAX_SLOTS] of watchdog_slot_t
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"

/* Opcodes OP_WD_REGISTER (0x50) .. OP_WD_RESUME (0x55) defined in agentos.h.
 * Return codes WD_OK (0x00), WD_ERR_NOENT (0x01), WD_ERR_FULL (0x02) likewise. */

/* ── Channel IDs (local to this PD) ────────────────────────────────── */
#define CH_IN      0   /* controller PPCs in */
#define CH_TIMEOUT 1   /* notify controller on heartbeat timeout */

/* ── Slot states ────────────────────────────────────────────────────── */
typedef enum {
    WD_SLOT_FREE     = 0,   /* unused entry */
    WD_SLOT_ACTIVE   = 1,   /* monitoring; heartbeat must arrive within timeout */
    WD_SLOT_FROZEN   = 2,   /* suspended during hot-reload; no timeout countdown */
    WD_SLOT_TIMEDOUT = 3,   /* timeout fired; controller notified; waiting for cleanup */
} wd_slot_state_t;

/* ── Per-slot descriptor ────────────────────────────────────────────── */
#define WD_MAX_SLOTS          16
#define WD_DEFAULT_TIMEOUT    200u   /* ticks before timeout alert */

typedef struct {
    uint32_t         slot_id;
    wd_slot_state_t  state;
    uint32_t         heartbeat_ticks;    /* configured timeout window */
    uint32_t         ticks_remaining;    /* counts down on each OP_WD_HEARTBEAT tick */
    uint32_t         module_hash_lo;     /* low 32 bits of current module BLAKE3 hash */
    uint64_t         total_heartbeats;   /* lifetime heartbeat count */
    uint64_t         freeze_count;       /* number of hot-reload freezes seen */
} watchdog_slot_t;

/* ── State ──────────────────────────────────────────────────────────── */
static watchdog_slot_t wd_slots[WD_MAX_SLOTS];
static uint32_t        wd_slot_count = 0;

/* ── Helpers ────────────────────────────────────────────────────────── */

static int wd_find(uint32_t slot_id) {
    for (int i = 0; i < WD_MAX_SLOTS; i++) {
        if (wd_slots[i].state != WD_SLOT_FREE &&
            wd_slots[i].slot_id == slot_id)
            return i;
    }
    return -1;
}

static int wd_alloc(void) {
    for (int i = 0; i < WD_MAX_SLOTS; i++) {
        if (wd_slots[i].state == WD_SLOT_FREE) return i;
    }
    return -1;
}

/* ── Handlers ───────────────────────────────────────────────────────── */

/*
 * OP_WD_REGISTER: begin monitoring a slot.
 *   MR1 = slot_id
 *   MR2 = heartbeat_ticks (0 → use WD_DEFAULT_TIMEOUT)
 */
static microkit_msginfo handle_wd_register(void) {
    uint32_t slot_id = (uint32_t)microkit_mr_get(1);
    uint32_t hb_ticks = (uint32_t)microkit_mr_get(2);
    if (hb_ticks == 0) hb_ticks = WD_DEFAULT_TIMEOUT;

    /* Idempotent: re-registration resets the timer */
    int idx = wd_find(slot_id);
    if (idx < 0) {
        idx = wd_alloc();
        if (idx < 0) {
            microkit_dbg_puts("[watchdog] REGISTER: table full\n");
            microkit_mr_set(0, WD_ERR_FULL);
            return microkit_msginfo_new(0, 1);
        }
        wd_slot_count++;
    }

    wd_slots[idx].slot_id          = slot_id;
    wd_slots[idx].state            = WD_SLOT_ACTIVE;
    wd_slots[idx].heartbeat_ticks  = hb_ticks;
    wd_slots[idx].ticks_remaining  = hb_ticks;
    wd_slots[idx].module_hash_lo   = 0;
    wd_slots[idx].total_heartbeats = 0;
    wd_slots[idx].freeze_count     = 0;

    microkit_dbg_puts("[watchdog] REGISTER slot=");
    char sb[4]; sb[0] = '0' + (slot_id % 10); sb[1] = '\0';
    microkit_dbg_puts(sb);
    microkit_dbg_puts("\n");

    microkit_mr_set(0, WD_OK);
    return microkit_msginfo_new(0, 1);
}

/*
 * OP_WD_HEARTBEAT: reset the timeout counter for a slot.
 * Called periodically by the controller on behalf of each active swap slot.
 * If ticks_remaining reaches 0 between heartbeats, we fire a timeout alert.
 *
 * For simplicity in this passive PD model: we count each missed heartbeat
 * as one decrement tick.  The controller drives the tick rate.
 */
static microkit_msginfo handle_wd_heartbeat(void) {
    uint32_t slot_id = (uint32_t)microkit_mr_get(1);
    int idx = wd_find(slot_id);

    if (idx < 0) {
        microkit_mr_set(0, WD_ERR_NOENT);
        return microkit_msginfo_new(0, 1);
    }

    watchdog_slot_t *s = &wd_slots[idx];

    if (s->state == WD_SLOT_FROZEN) {
        /* Heartbeats during freeze are silently absorbed; don't reset timer */
        microkit_mr_set(0, WD_OK);
        return microkit_msginfo_new(0, 1);
    }

    if (s->state == WD_SLOT_ACTIVE) {
        /* Check for timeout before resetting */
        if (s->ticks_remaining == 0) {
            s->state = WD_SLOT_TIMEDOUT;
            microkit_dbg_puts("[watchdog] TIMEOUT slot=");
            char sb[4]; sb[0] = '0' + (slot_id % 10); sb[1] = '\0';
            microkit_dbg_puts(sb);
            microkit_dbg_puts(" — notifying controller\n");
            microkit_mr_set(0, slot_id);
            microkit_notify(CH_TIMEOUT);
        } else {
            s->ticks_remaining--;
        }
        s->total_heartbeats++;
    }

    microkit_mr_set(0, WD_OK);
    return microkit_msginfo_new(0, 1);
}

/*
 * OP_WD_STATUS: query monitoring state for a slot.
 *   MR1 = slot_id → MR0=WD_OK, MR1=state, MR2=ticks_remaining, MR3=freeze_count
 */
static microkit_msginfo handle_wd_status(void) {
    uint32_t slot_id = (uint32_t)microkit_mr_get(1);
    int idx = wd_find(slot_id);

    if (idx < 0) {
        microkit_mr_set(0, WD_ERR_NOENT);
        return microkit_msginfo_new(0, 1);
    }

    microkit_mr_set(0, WD_OK);
    microkit_mr_set(1, (uint32_t)wd_slots[idx].state);
    microkit_mr_set(2, wd_slots[idx].ticks_remaining);
    microkit_mr_set(3, (uint32_t)wd_slots[idx].freeze_count);
    return microkit_msginfo_new(0, 4);
}

/*
 * OP_WD_UNREGISTER: stop monitoring a slot (slot is being torn down).
 *   MR1 = slot_id
 */
static microkit_msginfo handle_wd_unregister(void) {
    uint32_t slot_id = (uint32_t)microkit_mr_get(1);
    int idx = wd_find(slot_id);

    if (idx < 0) {
        microkit_mr_set(0, WD_ERR_NOENT);
        return microkit_msginfo_new(0, 1);
    }

    wd_slots[idx].state = WD_SLOT_FREE;
    wd_slot_count--;

    microkit_dbg_puts("[watchdog] UNREGISTER slot=");
    char sb[4]; sb[0] = '0' + (slot_id % 10); sb[1] = '\0';
    microkit_dbg_puts(sb);
    microkit_dbg_puts("\n");

    microkit_mr_set(0, WD_OK);
    return microkit_msginfo_new(0, 1);
}

/*
 * OP_WD_FREEZE (0x54): suspend heartbeat monitoring for a slot.
 *
 * Called by the controller before performing a hot-reload swap on a slot.
 * While frozen, the timeout countdown is halted and heartbeat ticks are
 * absorbed without alarm.  The slot remains registered; a subsequent
 * OP_WD_RESUME re-arms the timer with the original heartbeat_ticks window.
 *
 *   MR1 = slot_id
 */
static microkit_msginfo handle_wd_freeze(void) {
    uint32_t slot_id = (uint32_t)microkit_mr_get(1);
    int idx = wd_find(slot_id);

    if (idx < 0) {
        microkit_dbg_puts("[watchdog] FREEZE: slot not registered\n");
        microkit_mr_set(0, WD_ERR_NOENT);
        return microkit_msginfo_new(0, 1);
    }

    wd_slots[idx].state = WD_SLOT_FROZEN;
    wd_slots[idx].freeze_count++;

    microkit_dbg_puts("[watchdog] FREEZE slot=");
    char sb[4]; sb[0] = '0' + (slot_id % 10); sb[1] = '\0';
    microkit_dbg_puts(sb);
    microkit_dbg_puts(" — monitoring suspended for hot-reload\n");

    microkit_mr_set(0, WD_OK);
    return microkit_msginfo_new(0, 1);
}

/*
 * OP_WD_RESUME (0x55): resume monitoring after hot-reload completes.
 *
 * Resets ticks_remaining to the original heartbeat_ticks window and
 * records the new module hash for provenance tracking.
 *
 *   MR1 = slot_id
 *   MR2 = new_module_hash_lo (low 32 bits of BLAKE3 hash, for logging)
 */
static microkit_msginfo handle_wd_resume(void) {
    uint32_t slot_id         = (uint32_t)microkit_mr_get(1);
    uint32_t new_hash_lo     = (uint32_t)microkit_mr_get(2);
    int idx = wd_find(slot_id);

    if (idx < 0) {
        microkit_dbg_puts("[watchdog] RESUME: slot not registered\n");
        microkit_mr_set(0, WD_ERR_NOENT);
        return microkit_msginfo_new(0, 1);
    }

    wd_slots[idx].state           = WD_SLOT_ACTIVE;
    wd_slots[idx].ticks_remaining = wd_slots[idx].heartbeat_ticks;
    wd_slots[idx].module_hash_lo  = new_hash_lo;

    microkit_dbg_puts("[watchdog] RESUME slot=");
    char sb[4]; sb[0] = '0' + (slot_id % 10); sb[1] = '\0';
    microkit_dbg_puts(sb);
    microkit_dbg_puts(" — monitoring resumed after hot-reload\n");

    microkit_mr_set(0, WD_OK);
    return microkit_msginfo_new(0, 1);
}

/* ── Microkit entry points ──────────────────────────────────────────── */

void init(void) {
    for (int i = 0; i < WD_MAX_SLOTS; i++) {
        wd_slots[i].state = WD_SLOT_FREE;
    }
    microkit_dbg_puts("[watchdog] watchdog_pd ALIVE — monitoring ");
    char mx[4]; mx[0] = '0' + WD_MAX_SLOTS; mx[1] = '\0';
    microkit_dbg_puts(mx);
    microkit_dbg_puts(" slot capacity\n");
}

void notified(microkit_channel ch) {
    (void)ch;
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msg) {
    (void)ch;
    (void)msg;
    uint32_t op = (uint32_t)microkit_mr_get(0);

    switch (op) {
        case OP_WD_REGISTER:   return handle_wd_register();
        case OP_WD_HEARTBEAT:  return handle_wd_heartbeat();
        case OP_WD_STATUS:     return handle_wd_status();
        case OP_WD_UNREGISTER: return handle_wd_unregister();
        case OP_WD_FREEZE:     return handle_wd_freeze();
        case OP_WD_RESUME:     return handle_wd_resume();
        default:
            microkit_dbg_puts("[watchdog] Unknown op\n");
            microkit_mr_set(0, 0xFF);
            return microkit_msginfo_new(0, 1);
    }
}
