/*
 * agentOS Fault Handler Protection Domain
 *
 * Passive PD (priority 250 — highest in system) that receives seL4 fault
 * notifications for registered PDs and handles them gracefully.
 *
 * Fault types handled:
 *   VM_FAULT    — invalid memory access
 *   CAP_FAULT   — capability violation
 *   UNKNOWN_SYS — unknown syscall number
 *   USER_EXC    — userspace exception
 *
 * IPC operations:
 *   OP_FAULT_STATUS      (0x60) — query ring buffer state
 *   OP_FAULT_DUMP        (0x61) — read recent fault entries
 *   OP_FAULT_CLEAR       (0x62) — clear the fault ring buffer
 *   OP_FAULT_POLICY_SET  (0xE0) — update per-slot restart policy
 *
 * Memory:
 *   fault_ring (256KB): ring buffer for fault log entries, living in the
 *   private 2 MiB region the root task maps at AOS_FAULT_RING_VA
 *   (platform/fault_ring.h). No other PD maps it.
 *
 * E5-S8: migrated from Microkit to raw seL4 IPC.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "sel4_server.h"
#include "contracts/fault_handler_contract.h"
#include <platform/fault_ring.h>

/* ── Opcodes ──────────────────────────────────────────────────────────────── */
#define OP_FAULT_STATUS      0x60
#define OP_FAULT_DUMP        0x61
#define OP_FAULT_CLEAR       0x62
/* OP_FAULT_POLICY_SET (0xE0) is defined in agentos.h */

/* ── Fault type constants ─────────────────────────────────────────────────── */
#define FAULT_VM_FAULT    1
#define FAULT_CAP_FAULT   2
#define FAULT_UNKNOWN_SYS 7
#define FAULT_USER_EXC    8

/* ── Fault log entry (48 bytes) ───────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint64_t seq;
    uint64_t tick;
    uint32_t fault_type;
    uint32_t pd_id;
    uint64_t fault_addr;
    uint64_t fault_ip;
    uint32_t fault_data;
    uint32_t flags;
    uint8_t  _pad[4];
} fault_entry_t;

/* ── Ring buffer header (64 bytes) ───────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint64_t capacity;
    uint64_t head;
    uint64_t count;
    uint64_t drops;
    uint8_t  _pad[16];
} fault_ring_header_t;

#define FAULT_RING_MAGIC  0xFA17DEAD

/* ── Fault ring region ────────────────────────────────────────────────────
 *
 * The root task allocates one 2 MiB frame and maps it into this PD's VSpace
 * at AOS_FAULT_RING_VA before starting the thread (see
 * provision_fault_ring() in kernel/agentos-root-task/src/main.c). This global
 * is assigned from that constant in fault_handler_init() and nowhere else on
 * target; it is left as a writable global rather than a constant so a host
 * test can point the same code at a plain buffer.
 *
 * It stayed zero for the whole life of this file before that provisioning
 * existed, which made every FAULT_HDR store below a store through NULL: the
 * PD died on its first instruction after entry, on every architecture, and
 * nothing asserted otherwise. fault_ring_selfcheck() below is why that cannot
 * happen silently again.
 */
uintptr_t fault_ring_vaddr;

#define FAULT_HDR     ((volatile fault_ring_header_t *)fault_ring_vaddr)
#define FAULT_ENTRIES ((volatile fault_entry_t *) \
    ((uint8_t *)fault_ring_vaddr + sizeof(fault_ring_header_t)))

static uint64_t boot_tick = 0;

/* ── Per-PD restart policy ────────────────────────────────────────────────── */

typedef struct {
    uint8_t  max_restarts;
    uint8_t  restart_count;
    uint8_t  total_faults;
    uint8_t  escalate_after;
    uint32_t restart_delay_ms;
} fault_policy_t;

static fault_policy_t fault_policies[16];

static void fault_policy_init(void) {
    for (int i = 0; i < 16; i++) {
        fault_policies[i].max_restarts    = FAULT_POLICY_MAX_RESTARTS_DEFAULT;
        fault_policies[i].restart_count   = 0;
        fault_policies[i].total_faults    = 0;
        fault_policies[i].escalate_after  = FAULT_POLICY_ESCALATE_AFTER;
        fault_policies[i].restart_delay_ms = FAULT_POLICY_RESTART_DELAY_MS;
    }
}

/* ── Policy-driven fault decision ─────────────────────────────────────────── */

static void handle_fault_policy(uint32_t pd_slot) {
    if (pd_slot >= 16) return;
    fault_policy_t *p = &fault_policies[pd_slot];
    if (p->total_faults < 255u) p->total_faults++;
    if (p->total_faults >= p->escalate_after) {
        log_drain_write(13, 13,
            "[fault_handler] PD exceeded escalation threshold\n");
        /* In production: seL4_Signal(watchdog_notification_cap) */
        return;
    }
    if (p->restart_count < p->max_restarts) {
        p->restart_count++;
        log_drain_write(13, 13, "[fault_handler] restarting PD (within restart budget)\n");
    } else {
        log_drain_write(13, 13, "[fault_handler] PD exceeded max restarts, killing\n");
    }
}

/* ── Init ─────────────────────────────────────────────────────────────────── */

_Static_assert(AOS_FAULT_RING_BYTES <= AOS_FAULT_RING_REGION,
               "fault ring must fit inside the region root maps for it");
_Static_assert(AOS_FAULT_RING_BYTES > sizeof(fault_ring_header_t) +
                                      sizeof(fault_entry_t),
               "fault ring must hold a header and at least one entry");

#define FAULT_RING_CAPACITY \
    ((AOS_FAULT_RING_BYTES - sizeof(fault_ring_header_t)) / sizeof(fault_entry_t))

static void fault_handler_init(void) {
    /* The ONLY assignment of this symbol on target. Must precede every
     * FAULT_HDR / FAULT_ENTRIES use, including the first store below. */
    fault_ring_vaddr = (uintptr_t)AOS_FAULT_RING_VA;

    volatile fault_ring_header_t *hdr = FAULT_HDR;
    hdr->magic    = FAULT_RING_MAGIC;
    hdr->version  = 1;
    hdr->capacity = FAULT_RING_CAPACITY;
    hdr->head     = 0;
    hdr->count    = 0;
    hdr->drops    = 0;
    fault_policy_init();
    log_drain_write(13, 13, "[fault_handler] Initialized. capacity=5000+ fault entries, 48B each\n");
}

/* ── Boot self-check: the ring is real, writable memory ──────────────────── */

/* Every ring access below is through a volatile pointer, so the compiler may
 * not reorder or elide them; this barrier only stops the surrounding stores
 * and loads from being shuffled across a checkpoint. */
#define FAULT_BARRIER() __asm__ volatile ("" ::: "memory")

#define FAULT_LINE_MAX 192u

static uint32_t fault_put_str(char *out, uint32_t n, const char *s) {
    while (*s && n + 1u < FAULT_LINE_MAX) out[n++] = *s++;
    return n;
}

/* Fixed-width 16 lowercase hex digits: a stable field width keeps the emitted
 * marker byte-identical from boot to boot, so the harness can assert the exact
 * line rather than a prefix. */
static uint32_t fault_put_hex64(char *out, uint32_t n, uint64_t v) {
    static const char digits[] = "0123456789abcdef";
    for (int i = 15; i >= 0 && n + 1u < FAULT_LINE_MAX; i--)
        out[n++] = digits[(v >> (i * 4)) & 0xFu];
    return n;
}

/*
 * fault_ring_selfcheck — prove at boot that the mapping root installed is
 * backed by writable memory across the whole ring, not just at offset 0.
 *
 * This is deliberately more than "the header readback matched": with the
 * region unmapped the PD faults at the first store and never reaches here at
 * all, so reaching here AND round-tripping both the first entry slot and the
 * final byte of the ring is what makes the emitted marker non-vacuous. Every
 * sentinel is cleared and the header re-verified before returning, so a real
 * fault arriving afterwards sees a clean ring.
 *
 * Returns 1 on success, 0 on any mismatch.
 */
static int fault_ring_selfcheck(void) {
    volatile fault_ring_header_t *hdr     = FAULT_HDR;
    volatile fault_entry_t       *entries = FAULT_ENTRIES;

    if (hdr->magic != FAULT_RING_MAGIC) return 0;
    if (hdr->version != 1u) return 0;
    if (hdr->capacity != FAULT_RING_CAPACITY) return 0;
    if (hdr->head != 0u || hdr->count != 0u || hdr->drops != 0u) return 0;

    /* First and last entry slot of the ring. */
    const uint64_t last = (uint64_t)FAULT_RING_CAPACITY - 1u;
    entries[0].seq    = UINT64_C(0x0123456789abcdef);
    entries[last].seq = UINT64_C(0xfedcba9876543210);
    FAULT_BARRIER();
    if (entries[0].seq    != UINT64_C(0x0123456789abcdef)) return 0;
    if (entries[last].seq != UINT64_C(0xfedcba9876543210)) return 0;

    /* Final addressable byte of the ring -- the last padding byte of the last
     * entry. A byte-granular probe at the exact end of the claimed region, so
     * a mapping that is whole pages short still fails here even if the last
     * entry's first word happened to land inside the mapping. */
    volatile uint8_t *span =
        (volatile uint8_t *)fault_ring_vaddr + (AOS_FAULT_RING_BYTES - 1u);
    *span = 0xA5u;
    FAULT_BARRIER();
    if (*span != 0xA5u) return 0;
    *span = 0u;

    entries[0].seq    = 0u;
    entries[last].seq = 0u;
    FAULT_BARRIER();

    /* Writing through the ring must not have disturbed the header. */
    if (hdr->magic != FAULT_RING_MAGIC) return 0;
    if (hdr->capacity != FAULT_RING_CAPACITY) return 0;
    if (hdr->head != 0u || hdr->count != 0u || hdr->drops != 0u) return 0;

    static char line[FAULT_LINE_MAX];
    uint32_t n = 0;
    n = fault_put_str(line, n, "[fault_handler] ring self-check: PASS va=0x");
    n = fault_put_hex64(line, n, (uint64_t)fault_ring_vaddr);
    n = fault_put_str(line, n, " magic=0x");
    n = fault_put_hex64(line, n, (uint64_t)hdr->magic);
    n = fault_put_str(line, n, " capacity=0x");
    n = fault_put_hex64(line, n, (uint64_t)hdr->capacity);
    n = fault_put_str(line, n, " span=0x");
    n = fault_put_hex64(line, n, AOS_FAULT_RING_BYTES - 1u);
    n = fault_put_str(line, n, "\n");
    line[n] = '\0';
    log_drain_write(13, 13, line);
    return 1;
}

/* ── Append fault entry to ring ───────────────────────────────────────────── */
static void fault_append(uint32_t fault_type, uint32_t pd_id,
                          uint64_t fault_addr, uint64_t fault_ip,
                          uint32_t fault_data, uint32_t flags) {
    volatile fault_ring_header_t *hdr    = FAULT_HDR;
    volatile fault_entry_t       *entries = FAULT_ENTRIES;
    uint64_t idx = hdr->head % hdr->capacity;
    volatile fault_entry_t *e = &entries[idx];
    e->seq        = hdr->count;
    e->tick       = boot_tick;
    e->fault_type = fault_type;
    e->pd_id      = pd_id;
    e->fault_addr = fault_addr;
    e->fault_ip   = fault_ip;
    e->fault_data = fault_data;
    e->flags      = flags;
    hdr->head = (hdr->head + 1) % hdr->capacity;
    if (hdr->count >= hdr->capacity) hdr->drops++;
    hdr->count++;
}

/* ── msg helpers ──────────────────────────────────────────────────────────── */
#ifndef AGENTOS_IPC_HELPERS_DEFINED
#define AGENTOS_IPC_HELPERS_DEFINED
static inline uint32_t msg_u32(const sel4_msg_t *m, uint32_t off) {
    uint32_t v = 0;
    if (off + 4u <= SEL4_MSG_DATA_BYTES) {
        v  = (uint32_t)m->data[off]; v |= (uint32_t)m->data[off+1]<<8;
        v |= (uint32_t)m->data[off+2]<<16; v |= (uint32_t)m->data[off+3]<<24;
    }
    return v;
}
static inline void rep_u32(sel4_msg_t *m, uint32_t off, uint32_t v) {
    if (off + 4u <= SEL4_MSG_DATA_BYTES) {
        m->data[off]=(uint8_t)v; m->data[off+1]=(uint8_t)(v>>8);
        m->data[off+2]=(uint8_t)(v>>16); m->data[off+3]=(uint8_t)(v>>24);
    }
}
#endif /* AGENTOS_IPC_HELPERS_DEFINED */
static inline void rep_u64(sel4_msg_t *m, uint32_t off, uint64_t v) {
    rep_u32(m, off,     (uint32_t)(v & 0xFFFFFFFFU));
    rep_u32(m, off + 4, (uint32_t)(v >> 32));
}

/* ── IPC handlers ─────────────────────────────────────────────────────────── */

static uint32_t h_fault_status(sel4_badge_t b, const sel4_msg_t *req,
                                 sel4_msg_t *rep, void *ctx) {
    (void)b; (void)req; (void)ctx;
    volatile fault_ring_header_t *hdr = FAULT_HDR;
    rep_u32(rep, 0,  (uint32_t)(hdr->count    & 0xFFFFFFFF));
    rep_u32(rep, 4,  (uint32_t)(hdr->head     & 0xFFFFFFFF));
    rep_u32(rep, 8,  (uint32_t)(hdr->capacity & 0xFFFFFFFF));
    rep_u32(rep, 12, (uint32_t)(hdr->drops    & 0xFFFFFFFF));
    rep->length = 16;
    return SEL4_ERR_OK;
}

static uint32_t h_fault_dump(sel4_badge_t b, const sel4_msg_t *req,
                               sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    volatile fault_ring_header_t *hdr    = FAULT_HDR;
    volatile fault_entry_t       *entries = FAULT_ENTRIES;
    uint32_t start_back = msg_u32(req, 4);
    uint32_t req_count  = msg_u32(req, 8);
    if (req_count > 2) req_count = 2;
    uint64_t avail = hdr->count < hdr->capacity ? hdr->count : hdr->capacity;
    if (start_back >= avail || avail == 0) {
        rep_u32(rep, 0, 0); rep->length = 4; return SEL4_ERR_OK;
    }
    uint32_t actual = req_count;
    if (start_back + actual > avail) actual = (uint32_t)(avail - start_back);
    rep_u32(rep, 0, actual);
    for (uint32_t i = 0; i < actual; i++) {
        uint64_t idx = (hdr->head + hdr->capacity - 1 - start_back - i) % hdr->capacity;
        volatile fault_entry_t *e = &entries[idx];
        uint32_t base = 4 + i * 24;
        rep_u64(rep, base +  0, e->seq);
        rep_u64(rep, base +  8, e->tick);
        rep_u64(rep, base + 16, ((uint64_t)e->fault_type << 32) | e->pd_id);
    }
    rep->length = 4 + actual * 24;
    return SEL4_ERR_OK;
}

static uint32_t h_fault_clear(sel4_badge_t b, const sel4_msg_t *req,
                                sel4_msg_t *rep, void *ctx) {
    (void)b; (void)req; (void)ctx;
    volatile fault_ring_header_t *hdr = FAULT_HDR;
    hdr->head  = 0; hdr->count = 0; hdr->drops = 0;
    log_drain_write(13, 13, "[fault_handler] Ring cleared by request\n");
    rep_u32(rep, 0, 1); rep->length = 4;
    return SEL4_ERR_OK;
}

static uint32_t h_policy_set(sel4_badge_t b, const sel4_msg_t *req,
                               sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;
    uint32_t slot          = msg_u32(req, 4);
    uint32_t max_restarts  = msg_u32(req, 8);
    uint32_t escalate_after = msg_u32(req, 12);
    if (slot < 16) {
        fault_policies[slot].max_restarts   = (uint8_t)(max_restarts & 0xFF);
        fault_policies[slot].escalate_after = (uint8_t)(escalate_after & 0xFF);
        fault_policies[slot].restart_count  = 0;
        fault_policies[slot].total_faults   = 0;
        rep_u32(rep, 0, 1u);
    } else {
        rep_u32(rep, 0, 0u);
    }
    rep->length = 4;
    return SEL4_ERR_OK;
}

/*
 * h_fault_notify — called when the badge encodes a faulting PD channel.
 * In a full seL4 system, fault endpoints deliver the fault context in
 * message registers. In raw IPC mode we receive it in sel4_msg_t.data.
 */
static uint32_t h_fault_notify(sel4_badge_t b, const sel4_msg_t *req,
                                 sel4_msg_t *rep, void *ctx) {
    (void)ctx;
    boot_tick++;
    uint32_t fault_type = msg_u32(req, 4);
    uint32_t pd_id      = (uint32_t)(b & 0xFFFF);
    uint64_t fault_addr = (uint64_t)msg_u32(req, 8) | ((uint64_t)msg_u32(req, 12) << 32);
    uint64_t fault_ip   = (uint64_t)msg_u32(req, 16) | ((uint64_t)msg_u32(req, 20) << 32);
    uint32_t fault_data = msg_u32(req, 24);
    uint32_t flags      = msg_u32(req, 28);

    fault_append(fault_type, pd_id, fault_addr, fault_ip, fault_data, flags);

    if (fault_type == FAULT_CAP_FAULT) {
        log_drain_write(13, 13,
            "[fault_handler] CAP_FAULT logged in ring — controller will forward to cap_audit_log\n");
    }
    handle_fault_policy(pd_id < 16 ? pd_id : 15);

    rep_u32(rep, 0, 0); rep->length = 4;
    return SEL4_ERR_OK;
}

void fault_handler_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    (void)ns_ep;
    fault_handler_init();
    if (!fault_ring_selfcheck()) {
        /* The region is mapped (we got here at all) but does not behave like
         * memory. Say so and refuse to advertise readiness: a fault handler
         * whose log silently discards entries is worse than an absent one. */
        log_drain_write(13, 13,
            "[fault_handler] ring self-check: FAIL — fault ring is not usable memory\n");
        for (;;) { __asm__ volatile ("" ::: "memory"); }
    }
    log_drain_write(13, 13, "[fault_handler] Ready — priority 250, passive, monitoring all PD faults\n");

    static sel4_server_t srv;
    sel4_server_init(&srv, my_ep);
    sel4_server_register(&srv, OP_FAULT_STATUS,      h_fault_status,  (void *)0);
    sel4_server_register(&srv, OP_FAULT_DUMP,        h_fault_dump,    (void *)0);
    sel4_server_register(&srv, OP_FAULT_CLEAR,       h_fault_clear,   (void *)0);
    sel4_server_register(&srv, OP_FAULT_POLICY_SET,  h_policy_set,    (void *)0);
    sel4_server_register(&srv, 0xFFu,                h_fault_notify,  (void *)0);
    sel4_server_run(&srv);
}

void pd_main(seL4_CPtr my_ep, seL4_CPtr ns_ep) { fault_handler_main(my_ep, ns_ep); }
