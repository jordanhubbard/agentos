/*
 * agentOS Log Drain Protection Domain
 *
 * Drains per-PD log ring buffers to the debug UART.  Each PD writes log
 * lines into its own 4KB ring in the shared log_drain_rings region and
 * notifies this PD to flush it.
 *
 * Ring layout (per PD, 4KB each):
 *   [0..3]     magic (LOG_RING_MAGIC)
 *   [4..7]     pd_id (which PD owns this ring)
 *   [8..11]    head  (write offset, updated by PD)
 *   [12..15]   tail  (read offset, updated by log_drain)
 *   [16..4095] circular character buffer (4080 bytes)
 *
 * IPC Protocol (any PD -> log_drain, channel CH_LOG_DRAIN):
 *   MR0 = op code:
 *     OP_LOG_WRITE  (0x87) - MR1=slot, MR2=pd_id: register ring + drain
 *     OP_LOG_STATUS (0x86) - return: MR1=ring_count, MR2=bytes_total
 *
 * Priority: 160 (above workers/agents, below eventbus — drain promptly)
 * Mode: passive (only runs when notified or PPC'd into)
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "sel4_server.h"
#include "contracts/log_drain_contract.h"
#include "contracts/serial_contract.h"

/* ─── serial_pd output channel ───────────────────────────────────────── */

/* Channel 1 on the log_drain side connects to serial_pd (see manifest). */
#define CH_SERIAL_OUT  1u

/* ─── Configuration ───────────────────────────────────────────────────── */

#define MAX_LOG_RINGS    16
#define RING_SIZE        4096
#define RING_HEADER_SIZE 16
#define RING_BUF_SIZE    (RING_SIZE - RING_HEADER_SIZE)

/* ─── Per-ring state ──────────────────────────────────────────────────── */

typedef struct {
    bool     active;
    uint32_t pd_id;
    uint64_t bytes_total;
} log_ring_state_t;

/* ─── State ───────────────────────────────────────────────────────────── */

uintptr_t log_drain_rings_vaddr;

/* Microkit setvar_vaddr — shared with serial_pd for IPC data transfer */
uintptr_t serial_shmem_vaddr;

static log_ring_state_t ring_states[MAX_LOG_RINGS];
static uint32_t         ring_count = 0;
static uint64_t         total_bytes = 0;

/* serial_pd output state — lazily initialized on first write */
static uint32_t serial_slot  = (uint32_t)-1;
static bool     serial_ready = false;

/* PD name table — maps pd_id to friendly name */
static const struct { uint32_t id; const char *name; } pd_names[] = {
    { 0,  "controller" },
    { 1,  "event_bus"  },
    { 2,  "init_agent" },
    { 3,  "agentfs"    },
    { 4,  "vibe_engine"},
    { 5,  "worker_0"   },
    { 6,  "worker_1"   },
    { 7,  "worker_2"   },
    { 8,  "worker_3"   },
    { 9,  "swap_slot_0"},
    { 10, "swap_slot_1"},
    { 11, "swap_slot_2"},
    { 12, "swap_slot_3"},
    { 13, "log_drain"  },
    { 14, "linux_vmm"  },
    { 15, "fault_hndlr"},
};
#define NUM_PD_NAMES (sizeof(pd_names) / sizeof(pd_names[0]))

/* ─── Helpers ─────────────────────────────────────────────────────────── */

static volatile log_ring_header_t *get_ring(uint32_t slot) {
    return (volatile log_ring_header_t *)
        (log_drain_rings_vaddr + (slot * RING_SIZE));
}

static volatile char *get_ring_buf(uint32_t slot) {
    return (volatile char *)
        (log_drain_rings_vaddr + (slot * RING_SIZE) + RING_HEADER_SIZE);
}

static const char *pd_name_for(uint32_t pd_id) {
    for (uint32_t i = 0; i < NUM_PD_NAMES; i++) {
        if (pd_names[i].id == pd_id) return pd_names[i].name;
    }
    return "unknown";
}

static log_ring_state_t *find_ring_state(uint32_t pd_id) {
    for (uint32_t i = 0; i < ring_count; i++) {
        if (ring_states[i].active && ring_states[i].pd_id == pd_id)
            return &ring_states[i];
    }
    return NULL;
}

static log_ring_state_t *get_or_create_ring_state(uint32_t pd_id) {
    log_ring_state_t *s = find_ring_state(pd_id);
    if (s) return s;

    if (ring_count >= MAX_LOG_RINGS) return NULL;

    s = &ring_states[ring_count++];
    s->active      = true;
    s->pd_id       = pd_id;
    s->bytes_total = 0;
    return s;
}

/* ─── UART Output ─────────────────────────────────────────────────────── */

/*
 * Try to open a serial_pd slot on first use.  Falls back to microkit_dbg_puts
 * if serial_pd is not yet reachable (early boot or missing channel).
 * serial_shmem_vaddr must be mapped before this succeeds.
 */
static void try_serial_init(void)
{
    if (serial_ready || !serial_shmem_vaddr) return;

    rep_u32(rep, 0, MSG_SERIAL_OPEN);
    rep_u32(rep, 4, 0);   /* port_id 0 — raw output */
    uint32_t reply = /* E5-S8: ppcall stubbed */
    (void)reply;

    uint32_t ok   = (uint32_t)msg_u32(req, 0);
    uint32_t slot = (uint32_t)msg_u32(req, 4);

    if (ok == SERIAL_OK) {
        serial_slot  = slot;
        serial_ready = true;
    }
}

/*
 * Write a NUL-terminated string to the serial output.
 * Uses serial_pd IPC when available; falls back to microkit_dbg_puts
 * causing log_drain to fall back to microkit_dbg_puts.
 */
static void uart_puts(const char *s)
{
    if (!serial_ready) try_serial_init();

    if (!serial_ready) {
        sel4_dbg_puts(s);
        return;
    }

    /* Copy up to SERIAL_MAX_WRITE_BYTES per call — loop for longer strings */
    uint8_t *shmem = (uint8_t *)serial_shmem_vaddr;
    while (*s) {
        uint32_t n = 0;
        while (n < SERIAL_MAX_WRITE_BYTES && s[n]) n++;

        for (uint32_t i = 0; i < n; i++)
            shmem[i] = (uint8_t)s[i];

        rep_u32(rep, 0, MSG_SERIAL_WRITE);
        rep_u32(rep, 4, serial_slot);
        rep_u32(rep, 8, n);
        /* E5-S8: ppcall stubbed */

        s += n;
    }
}

static void uart_tagged_line(const char *pd_name, const char *line) {
    uart_puts("\033[36m[");
    uart_puts(pd_name);
    uart_puts("]\033[0m ");
    uart_puts(line);
    uart_puts("\n");
}

/* ─── Ring Drain ──────────────────────────────────────────────────────── */

/*
 * Drain one PD ring to UART.  Accumulates bytes into line-sized chunks so
 * each newline-terminated line is emitted as a tagged entry.
 */
static uint32_t drain_ring(uint32_t slot, log_ring_state_t *rs) {
    volatile log_ring_header_t *hdr = get_ring(slot);
    volatile char *buf = get_ring_buf(slot);

    if (hdr->magic != LOG_RING_MAGIC) return 0;

    uint32_t head    = hdr->head;
    uint32_t tail    = hdr->tail;
    uint32_t drained = 0;

    /* Line accumulator — static to avoid stack pressure in a passive PD */
    static char line_buf[256];
    static uint32_t line_pos = 0;

    const char *name = pd_name_for(rs->pd_id);

    while (tail != head) {
        char c = buf[tail % RING_BUF_SIZE];
        tail = (tail + 1) % RING_BUF_SIZE;
        drained++;

        if (c == '\n' || line_pos >= (uint32_t)(sizeof(line_buf) - 1)) {
            line_buf[line_pos] = '\0';
            uart_tagged_line(name, line_buf);
            line_pos = 0;
        } else {
            line_buf[line_pos++] = c;
        }
    }

    hdr->tail = tail;
    rs->bytes_total += drained;
    total_bytes     += drained;
    return drained;
}

/* Drain all registered rings */
static void drain_all(void) {
    for (uint32_t i = 0; i < ring_count; i++) {
        if (ring_states[i].active)
            drain_ring(i, &ring_states[i]);
    }
}

/* ─── Ring Initialization ─────────────────────────────────────────────── */

static void init_rings(void) {
    for (uint32_t i = 0; i < MAX_LOG_RINGS; i++) {
        volatile log_ring_header_t *hdr = get_ring(i);
        hdr->magic = 0;
        hdr->pd_id = 0;
        hdr->head  = 0;
        hdr->tail  = 0;
    }
}

static void register_ring(uint32_t slot, uint32_t pd_id) {
    volatile log_ring_header_t *hdr = get_ring(slot);
    hdr->magic = LOG_RING_MAGIC;
    hdr->pd_id = pd_id;
    hdr->head  = 0;
    hdr->tail  = 0;

    get_or_create_ring_state(pd_id);

    uart_puts("\033[32m[+] log_drain: ");
    uart_puts(pd_name_for(pd_id));
    uart_puts(" registered\033[0m\n");
}

/* ─── IPC Handlers ────────────────────────────────────────────────────── */

static void handle_log_write(uint32_t slot, uint32_t pd_id) {
    if (slot >= MAX_LOG_RINGS) {
        rep_u32(rep, 0, 1);
        return;
    }

    volatile log_ring_header_t *hdr = get_ring(slot);
    if (hdr->magic != LOG_RING_MAGIC) {
        register_ring(slot, pd_id);
    }

    log_ring_state_t *rs = find_ring_state(pd_id);
    if (rs) drain_ring(slot, rs);

    rep_u32(rep, 0, 0);
}

static void handle_log_status(void) {
    rep_u32(rep, 0, 0);
    rep_u32(rep, 4, ring_count);
    rep_u32(rep, 8, (uint32_t)(total_bytes & 0xFFFFFFFF));
    rep_u32(rep, 12, (uint32_t)(total_bytes >> 32));
}

/* ─── Microkit Entry Points ───────────────────────────────────────────── */

static void log_drain_pd_init(void) {
    uart_puts("[log_drain] starting — agentOS log drain\n");

    init_rings();
    ring_count  = 0;
    total_bytes = 0;

    uart_puts("[log_drain] ready — waiting for PD ring registrations\n");
}

static void log_drain_pd_notified(uint32_t ch) {
    (void)ch;
    drain_all();
}

static uint32_t log_drain_h_dispatch(sel4_badge_t b, const sel4_msg_t *req, sel4_msg_t *rep, void *ctx) {
    (void)b;
    (void)ctx;

    uint32_t op = (uint32_t)msg_u32(req, 0);

    switch (op) {
        case OP_LOG_WRITE:
            handle_log_write((uint32_t)msg_u32(req, 4),
                             (uint32_t)msg_u32(req, 8));
            break;

        case OP_LOG_STATUS:
            handle_log_status();
            break;

        default:
            sel4_dbg_puts("[log_drain] unknown op\n");
            rep_u32(rep, 0, 0xFF);
            break;
    }

    rep->length = 16;
        return SEL4_ERR_OK;
}

/* ── E5-S8: Entry point ─────────────────────────────────────────────────── */
void log_drain_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    (void)ns_ep;
    log_drain_pd_init();
    static sel4_server_t srv;
    sel4_server_init(&srv, my_ep);
    /* Dispatch all opcodes through the generic handler */
    sel4_server_register(&srv, SEL4_SERVER_OPCODE_ANY, log_drain_h_dispatch, (void *)0);
    sel4_server_run(&srv);
}
