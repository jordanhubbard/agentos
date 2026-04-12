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
 *   OP_DEBUG_IPC_SEND (0x62) — enqueue a command into the seL4→Linux ring
 *   OP_DEBUG_IPC_POLL (0x63) — poll the Linux→seL4 ring for a response
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
 *   id=7: IPC shmem notification (Linux→seL4 response ring has data)
 *
 * Memory:
 *   debug_ring    (256KB shared MR): ring buffer for debug events
 *   IPC shmem MR  (mapped at IPC_SHMEM_BASE): command + response rings
 *                 seL4→Linux cmd  ring at IPC_SHMEM_BASE + IPC_CMD_RING_OFFSET
 *                 Linux→seL4 resp ring at IPC_SHMEM_BASE + IPC_RESP_RING_OFFSET
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "ipc_bridge.h"

/* ── Opcodes ──────────────────────────────────────────────────────────────── */
#define OP_DBG_ATTACH      0x70  /* Attach debugger: MR1=slot_id */
#define OP_DBG_DETACH      0x71  /* Detach debugger: MR1=slot_id */
#define OP_DBG_BREAKPOINT  0x72  /* Set/clear BP: MR1=slot_id, MR2=wasm_offset, MR3=enable(1)/disable(0) */
#define OP_DBG_STEP        0x73  /* Single-step: MR1=slot_id, MR2=step_mode (1=into,2=over,3=out) */
#define OP_DBG_STATUS      0x74  /* Query: MR1=slot_id (0xFF=ring status) */

/*
 * IPC bridge opcodes (seL4 ↔ Linux VM command/response rings).
 * These share the debug_bridge dispatch table; the IPC bridge state lives
 * in BSS and is initialised alongside the debug ring.
 *
 *   OP_DEBUG_IPC_SEND (0x62)
 *     MR1 = IPC_OP_* operation code
 *     MR2 = vm_slot (0–3)
 *     MR3 = payload offset within the shared MR (Linux reads payload here)
 *     MR4 = payload_len (bytes, ≤ IPC_PAYLOAD_LEN)
 *     Reply: MR0=0 (ok) or 0xFF (ring full/error), MR1=seq
 *
 *   OP_DEBUG_IPC_POLL (0x63)
 *     MR1 = seq (from a prior OP_DEBUG_IPC_SEND)
 *     Reply: MR0=status (0=found,1=pending,0xFF=error),
 *            MR1=resp.status, MR2=resp.payload_offset, MR3=resp.payload_len
 */
#define OP_DEBUG_IPC_SEND  0x62
#define OP_DEBUG_IPC_POLL  0x63

/* Channel on which the Linux VM notifies us that the response ring has data */
#define DBG_CH_IPC_NOTIFY  7

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

/* ═══════════════════════════════════════════════════════════════════════════
 * IPC Bridge — seL4 ↔ Linux VM shared-memory command/response rings
 * ═══════════════════════════════════════════════════════════════════════════ */

/* ── IPC bridge BSS state ─────────────────────────────────────────────────── */

/* Virtual address of the IPC shmem MR base (set by ipc_bridge_init). */
static uintptr_t ipc_shmem_vaddr;

/* Monotonically increasing sequence counter for outgoing commands. */
static uint32_t ipc_seq_next;

/* Optional per-response callback registered via ipc_bridge_set_resp_cb(). */
static ipc_resp_cb_t ipc_resp_cb;
static void         *ipc_resp_cb_cookie;

/* Convenience accessors — never called before ipc_bridge_init(). */
#define IPC_CMD_RING  ((volatile ipc_cmd_ring_t *) \
    (ipc_shmem_vaddr + IPC_CMD_RING_OFFSET))
#define IPC_RESP_RING ((volatile ipc_resp_ring_t *) \
    (ipc_shmem_vaddr + IPC_RESP_RING_OFFSET))

/* ── Minimal memcpy (no libc) ─────────────────────────────────────────────── */
static void ipc_memcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

/* ── ipc_bridge_init ──────────────────────────────────────────────────────── */
void ipc_bridge_init(uintptr_t shmem_vaddr)
{
    ipc_shmem_vaddr     = shmem_vaddr;
    ipc_seq_next        = 1;
    ipc_resp_cb         = (ipc_resp_cb_t)0;
    ipc_resp_cb_cookie  = (void *)0;

    /* Initialise the seL4→Linux command ring. */
    volatile ipc_cmd_ring_t *cr = IPC_CMD_RING;
    cr->head  = 0;
    cr->tail  = 0;
    cr->_pad  = 0;
    /* Zero all command slots. */
    for (uint32_t i = 0; i < IPC_RING_DEPTH; i++) {
        volatile ipc_cmd_t *c = &cr->cmds[i];
        c->seq         = 0;
        c->op          = 0;
        c->vm_slot     = 0;
        c->payload_len = 0;
        for (uint32_t j = 0; j < IPC_PAYLOAD_LEN; j++) c->payload[j] = 0;
    }
    /* Write magic last so the Linux side sees a consistent ring. */
    __asm__ volatile("" ::: "memory");
    cr->magic = IPC_CMD_MAGIC;

    /* Initialise the Linux→seL4 response ring. */
    volatile ipc_resp_ring_t *rr = IPC_RESP_RING;
    rr->head  = 0;
    rr->tail  = 0;
    rr->_pad  = 0;
    for (uint32_t i = 0; i < IPC_RING_DEPTH; i++) {
        volatile ipc_resp_t *r = &rr->resps[i];
        r->seq         = 0;
        r->status      = 0;
        r->payload_len = 0;
        r->_pad        = 0;
        for (uint32_t j = 0; j < IPC_PAYLOAD_LEN; j++) r->payload[j] = 0;
    }
    __asm__ volatile("" ::: "memory");
    rr->magic = IPC_RESP_MAGIC;

    console_log(12, 12, "[ipc_bridge] Rings initialised: cmd@0x1000 resp@0x2000, depth=64\n");
}

/* ── ipc_bridge_send_cmd ──────────────────────────────────────────────────── */
int ipc_bridge_send_cmd(uint32_t op, uint32_t vm_slot,
                        const void *payload, uint32_t payload_len,
                        uint32_t *out_seq)
{
    if (!ipc_shmem_vaddr) return -1;

    volatile ipc_cmd_ring_t *cr = IPC_CMD_RING;

    /* Verify the ring was properly initialised. */
    if (cr->magic != IPC_CMD_MAGIC) {
        console_log(12, 12, "[ipc_bridge] WARN: cmd ring magic mismatch\n");
        return -1;
    }

    uint32_t head = cr->head;
    uint32_t tail = cr->tail;

    /* Ring full check: head - tail == IPC_RING_DEPTH */
    if ((head - tail) >= IPC_RING_DEPTH) {
        console_log(12, 12, "[ipc_bridge] WARN: cmd ring full\n");
        return -1;
    }

    uint32_t idx = head % IPC_RING_DEPTH;
    volatile ipc_cmd_t *slot = &cr->cmds[idx];

    uint32_t seq = ipc_seq_next++;
    slot->seq     = seq;
    slot->op      = op;
    slot->vm_slot = vm_slot;

    /* Clamp payload length and copy inline payload. */
    if (payload_len > IPC_PAYLOAD_LEN) payload_len = IPC_PAYLOAD_LEN;
    slot->payload_len = payload_len;

    if (payload && payload_len) {
        ipc_memcpy((void *)slot->payload, payload, payload_len);
    }

    /* Memory barrier then advance head so the Linux reader sees a complete entry. */
    __asm__ volatile("" ::: "memory");
    cr->head = head + 1;

    if (out_seq) *out_seq = seq;
    return 0;
}

/* ── ipc_bridge_poll_resp ─────────────────────────────────────────────────── */
int ipc_bridge_poll_resp(uint32_t seq, ipc_resp_t *out_resp)
{
    if (!ipc_shmem_vaddr || !out_resp) return -1;

    volatile ipc_resp_ring_t *rr = IPC_RESP_RING;

    if (rr->magic != IPC_RESP_MAGIC) {
        console_log(12, 12, "[ipc_bridge] WARN: resp ring magic mismatch\n");
        return -1;
    }

    uint32_t tail = rr->tail;
    uint32_t head = rr->head;

    /* Scan all unconsumed response slots for a matching seq. */
    for (uint32_t i = tail; i != head; i++) {
        uint32_t idx = i % IPC_RING_DEPTH;
        volatile ipc_resp_t *r = &rr->resps[idx];

        if (r->seq != seq) continue;

        /* Found — copy response out. */
        out_resp->seq         = r->seq;
        out_resp->status      = r->status;
        out_resp->payload_len = r->payload_len;
        out_resp->_pad        = 0;

        uint32_t plen = r->payload_len;
        if (plen > IPC_PAYLOAD_LEN) plen = IPC_PAYLOAD_LEN;
        ipc_memcpy(out_resp->payload, (const void *)r->payload, plen);

        /*
         * Consume: only advance tail when this entry is the oldest one.
         * If it is not (i > tail), leave tail alone — a future drain in
         * ipc_bridge_notified() will advance tail past all consumed slots.
         * Mark the slot seq=0 so notified() knows it has been taken.
         */
        r->seq = 0;
        __asm__ volatile("" ::: "memory");
        if (i == tail) {
            /* Advance tail over any already-consumed (seq==0) entries. */
            while (rr->tail != rr->head && rr->resps[rr->tail % IPC_RING_DEPTH].seq == 0)
                rr->tail++;
        }

        return 1;
    }

    return 0; /* still pending */
}

/* ── ipc_bridge_set_resp_cb ───────────────────────────────────────────────── */
void ipc_bridge_set_resp_cb(ipc_resp_cb_t cb, void *cookie)
{
    ipc_resp_cb        = cb;
    ipc_resp_cb_cookie = cookie;
}

/* ── ipc_bridge_notified ──────────────────────────────────────────────────── */
void ipc_bridge_notified(void)
{
    if (!ipc_shmem_vaddr) return;

    volatile ipc_resp_ring_t *rr = IPC_RESP_RING;

    if (rr->magic != IPC_RESP_MAGIC) {
        console_log(12, 12, "[ipc_bridge] WARN: notified but resp magic bad\n");
        return;
    }

    uint32_t drained = 0;

    /* Drain all pending responses in arrival order. */
    while (rr->tail != rr->head) {
        uint32_t idx = rr->tail % IPC_RING_DEPTH;
        volatile ipc_resp_t *r = &rr->resps[idx];

        /* Skip slots already consumed by ipc_bridge_poll_resp(). */
        if (r->seq == 0) {
            rr->tail++;
            continue;
        }

        if (ipc_resp_cb) {
            /* Build a non-volatile local copy for the callback. */
            ipc_resp_t local;
            local.seq         = r->seq;
            local.status      = r->status;
            local.payload_len = r->payload_len;
            local._pad        = 0;
            uint32_t plen = r->payload_len;
            if (plen > IPC_PAYLOAD_LEN) plen = IPC_PAYLOAD_LEN;
            ipc_memcpy(local.payload, (const void *)r->payload, plen);
            ipc_resp_cb(&local, ipc_resp_cb_cookie);
        }

        r->seq = 0; /* mark consumed */
        __asm__ volatile("" ::: "memory");
        rr->tail++;
        drained++;
    }

    if (drained) {
        console_log(12, 12, "[ipc_bridge] notified: drained resp(s) from Linux\n");
    }
}

/* ── Handle: OP_DEBUG_IPC_SEND ───────────────────────────────────────────── */
/*
 * MR1 = IPC_OP_* op code
 * MR2 = vm_slot
 * MR3 = payload_offset (caller's offset into the shared MR; we read from there)
 * MR4 = payload_len
 *
 * Reply: MR0=0 (ok)/0xFF (error), MR1=seq
 */
static microkit_msginfo handle_ipc_send(void)
{
    uint32_t op             = (uint32_t)microkit_mr_get(1);
    uint32_t vm_slot        = (uint32_t)microkit_mr_get(2);
    uint32_t payload_offset = (uint32_t)microkit_mr_get(3);
    uint32_t payload_len    = (uint32_t)microkit_mr_get(4);

    /* Validate op code. */
    if (op < IPC_OP_EXEC || op > IPC_OP_SIGNAL) {
        console_log(12, 12, "[ipc_bridge] IPC_SEND: invalid op\n");
        microkit_mr_set(0, 0xFF);
        microkit_mr_set(1, 0);
        return microkit_msginfo_new(0, 2);
    }

    /* Validate vm_slot. */
    if (vm_slot >= 4) {
        console_log(12, 12, "[ipc_bridge] IPC_SEND: invalid vm_slot\n");
        microkit_mr_set(0, 0xFF);
        microkit_mr_set(1, 0);
        return microkit_msginfo_new(0, 2);
    }

    /* Resolve the payload pointer from the shared MR. */
    const void *payload = (void *)0;
    if (payload_len > 0 && ipc_shmem_vaddr) {
        payload = (const void *)(ipc_shmem_vaddr + payload_offset);
    }

    uint32_t seq = 0;
    int rc = ipc_bridge_send_cmd(op, vm_slot, payload, payload_len, &seq);
    if (rc < 0) {
        microkit_mr_set(0, 0xFF);
        microkit_mr_set(1, 0);
        return microkit_msginfo_new(0, 2);
    }

    console_log(12, 12, "[ipc_bridge] IPC_SEND: enqueued cmd op=");
    /* Log op code (single hex digit for common ops 1-6). */
    {
        char _cl_buf[64] = {};
        char *p = _cl_buf;
        *p++ = '0' + (char)(op & 0xF);
        *p++ = ' ';
        *p++ = 's';
        *p++ = 'l';
        *p++ = 'o';
        *p++ = 't';
        *p++ = '=';
        *p++ = '0' + (char)(vm_slot & 0xF);
        *p++ = '\n';
        *p   = '\0';
        console_log(12, 12, _cl_buf);
    }

    microkit_mr_set(0, 0);
    microkit_mr_set(1, seq);
    return microkit_msginfo_new(0, 2);
}

/* ── Handle: OP_DEBUG_IPC_POLL ───────────────────────────────────────────── */
/*
 * MR1 = seq (from a prior OP_DEBUG_IPC_SEND)
 *
 * Reply:
 *   MR0 = 0 (response found), 1 (still pending), 0xFF (error)
 *   MR1 = resp.status     (valid when MR0==0)
 *   MR2 = resp.payload_offset — not meaningful here; we report 0
 *   MR3 = resp.payload_len
 */
static microkit_msginfo handle_ipc_poll(void)
{
    uint32_t seq = (uint32_t)microkit_mr_get(1);

    ipc_resp_t resp;
    int rc = ipc_bridge_poll_resp(seq, &resp);

    if (rc < 0) {
        /* Bridge not initialised or bad pointer. */
        microkit_mr_set(0, 0xFF);
        microkit_mr_set(1, 0);
        microkit_mr_set(2, 0);
        microkit_mr_set(3, 0);
        return microkit_msginfo_new(0, 4);
    }

    if (rc == 0) {
        /* Still pending — no response yet. */
        microkit_mr_set(0, 1);
        microkit_mr_set(1, 0);
        microkit_mr_set(2, 0);
        microkit_mr_set(3, 0);
        return microkit_msginfo_new(0, 4);
    }

    /* rc == 1: response found. */
    console_log(12, 12, "[ipc_bridge] IPC_POLL: response received\n");
    microkit_mr_set(0, 0);
    microkit_mr_set(1, resp.status);
    microkit_mr_set(2, 0);            /* payload_offset — inline only */
    microkit_mr_set(3, resp.payload_len);
    return microkit_msginfo_new(0, 4);
}

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

    /* Initialise the IPC bridge rings in the shared MR. */
    ipc_bridge_init(IPC_SHMEM_BASE);
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
    case OP_DBG_ATTACH:      return handle_attach();
    case OP_DBG_DETACH:      return handle_detach();
    case OP_DBG_BREAKPOINT:  return handle_breakpoint();
    case OP_DBG_STEP:        return handle_step();
    case OP_DBG_STATUS:      return handle_status();
    case OP_DEBUG_IPC_SEND:  return handle_ipc_send();
    case OP_DEBUG_IPC_POLL:  return handle_ipc_poll();
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

    /* IPC shmem notification: Linux→seL4 response ring has data (channel 7). */
    if (ch == DBG_CH_IPC_NOTIFY) {
        ipc_bridge_notified();
        return;
    }

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
