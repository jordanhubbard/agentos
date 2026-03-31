/*
 * agentOS Debug Bridge Protection Domain
 *
 * Passive PD (priority 160) that provides a debug channel for live WASM
 * slot debugging. Host-side tooling (DAP/GDB bridge) attaches to WASM
 * slots through this PD, sets breakpoints, single-steps, and inspects
 * suspended execution state.
 *
 * The debug bridge maintains per-slot debug state (breakpoints, step mode,
 * suspended flag) and writes debug events (breakpoint hits, step completions)
 * to a 256KB shared-memory ring buffer (debug_ring) that host-side tooling
 * polls for consumption.
 *
 * IPC operations (via PPC from controller or vibe_engine):
 *   OP_DBG_ATTACH     (0x70) — attach debugger to a WASM slot
 *   OP_DBG_DETACH     (0x71) — detach debugger from a WASM slot
 *   OP_DBG_BREAKPOINT (0x72) — set/clear breakpoint at WASM byte offset
 *   OP_DBG_STEP       (0x73) — single-step a suspended slot
 *   OP_DBG_STATUS     (0x74) — query debug state for a slot or the ring
 *
 * On breakpoint hit (signaled by swap_slot via notify):
 *   1. Writes a debug_event to debug_ring
 *   2. Sets slot's suspended flag
 *   3. Notifies controller so host-side DAP bridge can poll
 *
 * Channels (from debug_bridge perspective):
 *   id=0: vibe_engine PPCs in (breakpoint/step/attach/detach)
 *   id=1: controller PPCs in (status queries, resume commands)
 *   id=2: debug_bridge notifies controller (debug event available)
 *
 * Memory:
 *   debug_ring (256KB shared MR): ring buffer for debug events
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"

/* ── Opcodes ──────────────────────────────────────────────────────────────── */
#define OP_DBG_ATTACH      0x70  /* Attach debugger: MR1=slot_id */
#define OP_DBG_DETACH      0x71  /* Detach debugger: MR1=slot_id */
#define OP_DBG_BREAKPOINT  0x72  /* Set/clear BP: MR1=slot_id, MR2=wasm_offset, MR3=enable(1)/disable(0) */
#define OP_DBG_STEP        0x73  /* Single-step: MR1=slot_id, MR2=step_mode (1=into,2=over,3=out) */
#define OP_DBG_STATUS      0x74  /* Query: MR1=slot_id (0xFF=ring status) */

/* ── Debug event types (written to ring) ──────────────────────────────────── */
#define DBG_EVT_BREAKPOINT_HIT  1  /* WASM execution hit a breakpoint */
#define DBG_EVT_STEP_COMPLETE   2  /* single-step completed */
#define DBG_EVT_ATTACHED        3  /* debugger attached to slot */
#define DBG_EVT_DETACHED        4  /* debugger detached from slot */
#define DBG_EVT_SUSPENDED       5  /* slot suspended (e.g., on fault) */
#define DBG_EVT_RESUMED         6  /* slot resumed after step/continue */

/* ── Step modes ───────────────────────────────────────────────────────────── */
#define STEP_INTO  1
#define STEP_OVER  2
#define STEP_OUT   3

/* ── Limits ───────────────────────────────────────────────────────────────── */
#define MAX_DEBUG_SLOTS     4   /* matches swap_slot_0..3 */
#define MAX_BREAKPOINTS     32  /* per slot */

/* ── Per-slot breakpoint entry ────────────────────────────────────────────── */
typedef struct {
    uint32_t wasm_offset;   /* byte offset into WASM code section */
    uint8_t  enabled;       /* 1=active, 0=disabled */
} breakpoint_t;

/* ── Per-slot debug state ─────────────────────────────────────────────────── */
typedef struct {
    uint8_t      attached;                    /* 1 if debugger is attached */
    uint8_t      suspended;                   /* 1 if execution is suspended */
    uint8_t      step_mode;                   /* 0=none, STEP_INTO/OVER/OUT */
    uint8_t      _pad;
    uint32_t     bp_count;                    /* number of breakpoints set */
    breakpoint_t breakpoints[MAX_BREAKPOINTS];
    uint64_t     suspend_pc;                  /* WASM PC where suspended */
    uint64_t     hit_count;                   /* total breakpoint hits */
} slot_debug_state_t;

/* ── Debug event ring entry (48 bytes) ────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint64_t seq;           /* monotonic sequence number */
    uint64_t tick;          /* boot tick at event time */
    uint32_t event_type;    /* DBG_EVT_* */
    uint32_t slot_id;       /* which WASM slot */
    uint64_t wasm_pc;       /* WASM program counter / byte offset */
    uint32_t bp_index;      /* breakpoint index (for BP hit events) */
    uint32_t step_mode;     /* step mode at time of event */
    uint8_t  _pad[8];       /* pad to 48 bytes */
} debug_event_t;

/* ── Ring buffer header (64 bytes) ────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t magic;         /* 0xDB6B1D6E ("DEBUG RING") */
    uint32_t version;       /* 1 */
    uint64_t capacity;      /* number of event slots */
    uint64_t head;          /* next write index */
    uint64_t count;         /* total events written */
    uint64_t drops;         /* events overwritten (ring wrap) */
    uint8_t  _pad[16];     /* pad to 64 bytes */
} debug_ring_header_t;

#define DEBUG_RING_MAGIC  0xDB6B1D6E

/* ── Shared memory ────────────────────────────────────────────────────────── */
uintptr_t debug_ring_vaddr;

#define DBG_HDR     ((volatile debug_ring_header_t *)debug_ring_vaddr)
#define DBG_ENTRIES ((volatile debug_event_t *) \
    ((uint8_t *)debug_ring_vaddr + sizeof(debug_ring_header_t)))

/* ── Channel IDs (from debug_bridge perspective) ──────────────────────────── */
#define DBG_CH_VIBE_ENGINE   0   /* vibe_engine PPCs in */
#define DBG_CH_CONTROLLER    1   /* controller PPCs in / queries */
#define DBG_CH_NOTIFY_CTRL   2   /* debug_bridge notifies controller */

/* ── State ────────────────────────────────────────────────────────────────── */
static slot_debug_state_t slot_state[MAX_DEBUG_SLOTS];
static uint64_t boot_tick = 0;

/* ── Init ─────────────────────────────────────────────────────────────────── */
static void debug_bridge_init(void) {
    volatile debug_ring_header_t *hdr = DBG_HDR;

    uint64_t region_size = 0x40000;  /* 256KB */
    uint64_t entry_space = region_size - sizeof(debug_ring_header_t);
    uint64_t cap = entry_space / sizeof(debug_event_t);

    hdr->magic    = DEBUG_RING_MAGIC;
    hdr->version  = 1;
    hdr->capacity = cap;
    hdr->head     = 0;
    hdr->count    = 0;
    hdr->drops    = 0;

    /* Zero out all slot debug state */
    for (int i = 0; i < MAX_DEBUG_SLOTS; i++) {
        slot_state[i].attached  = 0;
        slot_state[i].suspended = 0;
        slot_state[i].step_mode = 0;
        slot_state[i].bp_count  = 0;
        slot_state[i].suspend_pc = 0;
        slot_state[i].hit_count = 0;
    }

    console_log(12, 12, "[debug_bridge] Ring initialized: capacity=5000+ events, 48B each\n");
}

/* ── Append debug event to ring ───────────────────────────────────────────── */
static void debug_event_append(uint32_t event_type, uint32_t slot_id,
                                uint64_t wasm_pc, uint32_t bp_index,
                                uint32_t step_mode) {
    volatile debug_ring_header_t *hdr = DBG_HDR;
    volatile debug_event_t *entries = DBG_ENTRIES;

    uint64_t idx = hdr->head % hdr->capacity;
    volatile debug_event_t *e = &entries[idx];

    e->seq        = hdr->count;
    e->tick       = boot_tick;
    e->event_type = event_type;
    e->slot_id    = slot_id;
    e->wasm_pc    = wasm_pc;
    e->bp_index   = bp_index;
    e->step_mode  = step_mode;

    /* Memory barrier before advancing head */
    __asm__ volatile("" ::: "memory");

    hdr->head = (hdr->head + 1) % hdr->capacity;
    if (hdr->count >= hdr->capacity) {
        hdr->drops++;
    }
    hdr->count++;
}

/* ── Notify controller that a debug event is available ────────────────────── */
static void notify_controller_debug_event(uint32_t slot_id, uint32_t event_type) {
    microkit_mr_set(0, slot_id);
    microkit_mr_set(1, event_type);
    microkit_notify(DBG_CH_NOTIFY_CTRL);
}

/* ── Handle: OP_DBG_ATTACH ────────────────────────────────────────────────── */
static microkit_msginfo handle_attach(void) {
    uint32_t slot_id = (uint32_t)microkit_mr_get(1);

    if (slot_id >= MAX_DEBUG_SLOTS) {
        console_log(12, 12, "[debug_bridge] ATTACH failed: invalid slot_id\n");
        microkit_mr_set(0, 0xFF); /* error: invalid slot */
        return microkit_msginfo_new(0, 1);
    }

    if (slot_state[slot_id].attached) {
        console_log(12, 12, "[debug_bridge] ATTACH: slot already attached\n");
        microkit_mr_set(0, 0xFE); /* error: already attached */
        return microkit_msginfo_new(0, 1);
    }

    slot_state[slot_id].attached  = 1;
    slot_state[slot_id].suspended = 0;
    slot_state[slot_id].step_mode = 0;
    slot_state[slot_id].bp_count  = 0;
    slot_state[slot_id].hit_count = 0;

    /* Write attach event to ring */
    debug_event_append(DBG_EVT_ATTACHED, slot_id, 0, 0, 0);
    notify_controller_debug_event(slot_id, DBG_EVT_ATTACHED);

    console_log(12, 12, "[debug_bridge] ATTACHED slot=");
    char buf[4];
    buf[0] = '0' + (slot_id % 10);
    buf[1] = '\0';
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = buf; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = "\n"; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        console_log(12, 12, _cl_buf);
    }

    microkit_mr_set(0, 0); /* success */
    return microkit_msginfo_new(0, 1);
}

/* ── Handle: OP_DBG_DETACH ────────────────────────────────────────────────── */
static microkit_msginfo handle_detach(void) {
    uint32_t slot_id = (uint32_t)microkit_mr_get(1);

    if (slot_id >= MAX_DEBUG_SLOTS) {
        microkit_mr_set(0, 0xFF);
        return microkit_msginfo_new(0, 1);
    }

    if (!slot_state[slot_id].attached) {
        microkit_mr_set(0, 0xFE); /* not attached */
        return microkit_msginfo_new(0, 1);
    }

    /* If suspended, resume before detaching */
    if (slot_state[slot_id].suspended) {
        slot_state[slot_id].suspended = 0;
        debug_event_append(DBG_EVT_RESUMED, slot_id,
                           slot_state[slot_id].suspend_pc, 0, 0);
    }

    slot_state[slot_id].attached  = 0;
    slot_state[slot_id].step_mode = 0;
    slot_state[slot_id].bp_count  = 0;

    debug_event_append(DBG_EVT_DETACHED, slot_id, 0, 0, 0);
    notify_controller_debug_event(slot_id, DBG_EVT_DETACHED);

    console_log(12, 12, "[debug_bridge] DETACHED slot=");
    char buf[4];
    buf[0] = '0' + (slot_id % 10);
    buf[1] = '\0';
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = buf; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = "\n"; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        console_log(12, 12, _cl_buf);
    }

    microkit_mr_set(0, 0);
    return microkit_msginfo_new(0, 1);
}

/* ── Handle: OP_DBG_BREAKPOINT ────────────────────────────────────────────── */
static microkit_msginfo handle_breakpoint(void) {
    uint32_t slot_id     = (uint32_t)microkit_mr_get(1);
    uint32_t wasm_offset = (uint32_t)microkit_mr_get(2);
    uint32_t enable      = (uint32_t)microkit_mr_get(3);

    if (slot_id >= MAX_DEBUG_SLOTS || !slot_state[slot_id].attached) {
        microkit_mr_set(0, 0xFF);
        return microkit_msginfo_new(0, 1);
    }

    slot_debug_state_t *s = &slot_state[slot_id];

    if (enable) {
        /* Set breakpoint */
        /* Check if already exists */
        for (uint32_t i = 0; i < s->bp_count; i++) {
            if (s->breakpoints[i].wasm_offset == wasm_offset) {
                s->breakpoints[i].enabled = 1;
                microkit_mr_set(0, 0);
                microkit_mr_set(1, i); /* breakpoint index */
                return microkit_msginfo_new(0, 2);
            }
        }

        /* Add new breakpoint */
        if (s->bp_count >= MAX_BREAKPOINTS) {
            console_log(12, 12, "[debug_bridge] BP table full\n");
            microkit_mr_set(0, 0xFD); /* table full */
            return microkit_msginfo_new(0, 1);
        }

        uint32_t idx = s->bp_count++;
        s->breakpoints[idx].wasm_offset = wasm_offset;
        s->breakpoints[idx].enabled     = 1;

        console_log(12, 12, "[debug_bridge] BP set slot=");
        char buf[4];
        buf[0] = '0' + (slot_id % 10);
        buf[1] = '\0';
        {
            char _cl_buf[256] = {};
            char *_cl_p = _cl_buf;
            for (const char *_s = buf; *_s; _s++) *_cl_p++ = *_s;
            for (const char *_s = " offset=0x...\n"; *_s; _s++) *_cl_p++ = *_s;
            *_cl_p = 0;
            console_log(12, 12, _cl_buf);
        }

        microkit_mr_set(0, 0);
        microkit_mr_set(1, idx);
        return microkit_msginfo_new(0, 2);
    } else {
        /* Disable breakpoint at offset */
        for (uint32_t i = 0; i < s->bp_count; i++) {
            if (s->breakpoints[i].wasm_offset == wasm_offset) {
                s->breakpoints[i].enabled = 0;
                microkit_mr_set(0, 0);
                return microkit_msginfo_new(0, 1);
            }
        }
        microkit_mr_set(0, 0xFC); /* not found */
        return microkit_msginfo_new(0, 1);
    }
}

/* ── Handle: OP_DBG_STEP ──────────────────────────────────────────────────── */
static microkit_msginfo handle_step(void) {
    uint32_t slot_id   = (uint32_t)microkit_mr_get(1);
    uint32_t step_mode = (uint32_t)microkit_mr_get(2);

    if (slot_id >= MAX_DEBUG_SLOTS || !slot_state[slot_id].attached) {
        microkit_mr_set(0, 0xFF);
        return microkit_msginfo_new(0, 1);
    }

    if (!slot_state[slot_id].suspended) {
        microkit_mr_set(0, 0xFE); /* not suspended, can't step */
        return microkit_msginfo_new(0, 1);
    }

    if (step_mode < STEP_INTO || step_mode > STEP_OUT) {
        microkit_mr_set(0, 0xFD); /* invalid step mode */
        return microkit_msginfo_new(0, 1);
    }

    slot_state[slot_id].step_mode = (uint8_t)step_mode;
    slot_state[slot_id].suspended = 0;

    debug_event_append(DBG_EVT_RESUMED, slot_id,
                       slot_state[slot_id].suspend_pc, 0, step_mode);

    console_log(12, 12, "[debug_bridge] STEP slot=");
    char buf[4];
    buf[0] = '0' + (slot_id % 10);
    buf[1] = '\0';
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = buf; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = " mode="; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        console_log(12, 12, _cl_buf);
    }
    buf[0] = '0' + (step_mode % 10);
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = buf; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = "\n"; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        console_log(12, 12, _cl_buf);
    }

    /*
     * The actual stepping happens in the swap_slot PD's WASM interpreter.
     * We set the step_mode flag; when the swap_slot detects it (via shared
     * state or next PPC), it will execute one instruction and notify us
     * back via the debug notification channel.
     */

    microkit_mr_set(0, 0);
    return microkit_msginfo_new(0, 1);
}

/* ── Handle: OP_DBG_STATUS ────────────────────────────────────────────────── */
static microkit_msginfo handle_status(void) {
    uint32_t slot_id = (uint32_t)microkit_mr_get(1);

    if (slot_id == 0xFF) {
        /* Ring buffer status */
        volatile debug_ring_header_t *hdr = DBG_HDR;
        microkit_mr_set(0, (uint32_t)(hdr->count & 0xFFFFFFFF));
        microkit_mr_set(1, (uint32_t)(hdr->head & 0xFFFFFFFF));
        microkit_mr_set(2, (uint32_t)(hdr->capacity & 0xFFFFFFFF));
        microkit_mr_set(3, (uint32_t)(hdr->drops & 0xFFFFFFFF));
        return microkit_msginfo_new(0, 4);
    }

    if (slot_id >= MAX_DEBUG_SLOTS) {
        microkit_mr_set(0, 0xFF);
        return microkit_msginfo_new(0, 1);
    }

    slot_debug_state_t *s = &slot_state[slot_id];
    microkit_mr_set(0, s->attached);
    microkit_mr_set(1, s->suspended);
    microkit_mr_set(2, s->step_mode);
    microkit_mr_set(3, s->bp_count);
    microkit_mr_set(4, (uint32_t)(s->suspend_pc & 0xFFFFFFFF));
    microkit_mr_set(5, (uint32_t)(s->hit_count & 0xFFFFFFFF));
    return microkit_msginfo_new(0, 6);
}

/* ── Microkit entry points ────────────────────────────────────────────────── */

void init(void) {
    agentos_log_boot("debug_bridge");
    debug_bridge_init();
    console_log(12, 12, "[debug_bridge] Ready — passive debug channel, priority 160\n");
}

/*
 * Protected procedure call handler (passive PD).
 * Called when vibe_engine or controller PPCs into us.
 */
microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo) {
    (void)ch;
    (void)msginfo;

    uint64_t op = microkit_mr_get(0);
    boot_tick++;

    switch (op) {
    case OP_DBG_ATTACH:     return handle_attach();
    case OP_DBG_DETACH:     return handle_detach();
    case OP_DBG_BREAKPOINT: return handle_breakpoint();
    case OP_DBG_STEP:       return handle_step();
    case OP_DBG_STATUS:     return handle_status();
    default:
        console_log(12, 12, "[debug_bridge] WARN: unknown opcode\n");
        microkit_mr_set(0, 0xFF);
        return microkit_msginfo_new(0, 1);
    }
}

/*
 * Notification handler.
 *
 * Swap slots notify us when they hit a breakpoint or complete a step.
 * The notification carries:
 *   Channel N (3+slot_id): swap_slot_N signaling a debug event
 *
 * Since we're passive, notifications wake us up. The swap slot writes
 * the WASM PC and event details into its shared code region header
 * (a small metadata area), and we read it from here.
 *
 * For now, we use a simple protocol:
 *   - Swap slot notifies us on its debug channel
 *   - We check which slot by channel number
 *   - We read the WASM PC from the slot's state (would be shared mem in
 *     a full implementation; here we use a convention that the slot wrote
 *     the PC into MR0 of the notification badge)
 */
void notified(microkit_channel ch) {
    boot_tick++;

    /*
     * Channels 3..6 map to swap_slot_0..3 debug notifications.
     * When a swap slot hits a breakpoint or completes a step, it notifies
     * us on its assigned channel.
     */
    if (ch >= 3 && ch < 3 + MAX_DEBUG_SLOTS) {
        uint32_t slot_id = ch - 3;
        slot_debug_state_t *s = &slot_state[slot_id];

        if (!s->attached) return; /* ignore if no debugger attached */

        /*
         * In a full implementation, we'd read the WASM PC from shared
         * memory. For now, use a placeholder — the swap_slot PD would
         * write the hit address into a known offset in its code region.
         */
        uint64_t wasm_pc = 0; /* TODO: read from swap slot shared mem */

        if (s->step_mode) {
            /* Step completed */
            s->suspended  = 1;
            s->suspend_pc = wasm_pc;
            s->step_mode  = 0;

            debug_event_append(DBG_EVT_STEP_COMPLETE, slot_id,
                               wasm_pc, 0, s->step_mode);
            notify_controller_debug_event(slot_id, DBG_EVT_STEP_COMPLETE);

            console_log(12, 12, "[debug_bridge] STEP complete slot=");
            char buf[4];
            buf[0] = '0' + (slot_id % 10);
            buf[1] = '\0';
            {
                char _cl_buf[256] = {};
                char *_cl_p = _cl_buf;
                for (const char *_s = buf; *_s; _s++) *_cl_p++ = *_s;
                for (const char *_s = "\n"; *_s; _s++) *_cl_p++ = *_s;
                *_cl_p = 0;
                console_log(12, 12, _cl_buf);
            }
        } else {
            /* Breakpoint hit — check if PC matches any active breakpoint */
            uint32_t bp_idx = 0xFFFF;
            for (uint32_t i = 0; i < s->bp_count; i++) {
                if (s->breakpoints[i].enabled &&
                    s->breakpoints[i].wasm_offset == (uint32_t)wasm_pc) {
                    bp_idx = i;
                    break;
                }
            }

            s->suspended  = 1;
            s->suspend_pc = wasm_pc;
            s->hit_count++;

            debug_event_append(DBG_EVT_BREAKPOINT_HIT, slot_id,
                               wasm_pc, bp_idx, 0);
            notify_controller_debug_event(slot_id, DBG_EVT_BREAKPOINT_HIT);

            console_log(12, 12, "[debug_bridge] BREAKPOINT HIT slot=");
            char buf[4];
            buf[0] = '0' + (slot_id % 10);
            buf[1] = '\0';
            {
                char _cl_buf[256] = {};
                char *_cl_p = _cl_buf;
                for (const char *_s = buf; *_s; _s++) *_cl_p++ = *_s;
                for (const char *_s = "\n"; *_s; _s++) *_cl_p++ = *_s;
                *_cl_p = 0;
                console_log(12, 12, _cl_buf);
            }
        }
    }
}
