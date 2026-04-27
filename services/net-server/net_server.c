/*
 * agentOS NetServer Protection Domain — E5-S4: raw seL4 IPC
 *
 * Manages per-application virtual NICs and provides the network stack
 * interface for all agents running under agentOS.
 *
 * Migration notes (E5-S4):
 *   - Priority ordering constraint ELIMINATED: net_server_main() runs its
 *     own sel4_server_t loop and can be at any priority.  Callers block on
 *     seL4 endpoint IPC rather than relying on Microkit PPC priority ordering.
 *   - All microkit_ppcall() outbound calls replaced with sel4_call().
 *   - All microkit_notify(ch) replaced with seL4_Signal(ntfn_cap).
 *   - lwIP timer tick: arrives as seL4 notification on timer_ntfn_cap (no PPC).
 *   - CH_CONTROLLER / CH_NET_SERVER channel constants are gone; the nameserver
 *     provides endpoint caps at runtime.
 *   - net_server registers itself as "net" with the nameserver at startup.
 *
 * TCP/IP stack:
 *   lwIP integration via lwip_sys.c.  net_server_lwip_init() sets up the
 *   netif and DHCP client.  Packet Rx/Tx is handled by lwIP pbuf chains.
 *
 * virtio-net:
 *   net_server_main() probes the MMIO region (net_mmio_vaddr).  If
 *   magic/version/device_id match, net_hw_present is set true and TX is
 *   attempted through the stub.  If not found, stub mode is used throughout.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

/* ── Conditional compilation ─────────────────────────────────────────────── */

#ifdef AGENTOS_TEST_HOST
/*
 * Host-side test build: minimal type stubs so this file compiles without
 * seL4 or Microkit headers.  framework.h (included by the test file before
 * this unit) supplies microkit_mr_set/get stubs.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

typedef unsigned long      seL4_CPtr;
typedef unsigned long long sel4_badge_t;

typedef struct {
    uint32_t opcode;
    uint32_t length;
    uint8_t  data[48];
} sel4_msg_t;

#define SEL4_ERR_OK          0u
#define SEL4_ERR_INVALID_OP  1u
#define SEL4_ERR_NOT_FOUND   2u
#define SEL4_ERR_PERM        3u
#define SEL4_ERR_BAD_ARG     4u
#define SEL4_ERR_NO_MEM      5u

typedef uint32_t (*sel4_handler_fn)(sel4_badge_t badge,
                                     const sel4_msg_t *req,
                                     sel4_msg_t *rep,
                                     void *ctx);
#define SEL4_SERVER_MAX_HANDLERS 32u
typedef struct {
    struct {
        uint32_t        opcode;
        sel4_handler_fn fn;
        void           *ctx;
    } handlers[SEL4_SERVER_MAX_HANDLERS];
    uint32_t  handler_count;
    seL4_CPtr ep;
} sel4_server_t;

static inline void sel4_server_init(sel4_server_t *srv, seL4_CPtr ep)
{
    srv->handler_count = 0;
    srv->ep            = ep;
    for (uint32_t i = 0; i < SEL4_SERVER_MAX_HANDLERS; i++) {
        srv->handlers[i].opcode = 0;
        srv->handlers[i].fn     = (sel4_handler_fn)0;
        srv->handlers[i].ctx    = (void *)0;
    }
}
static inline int sel4_server_register(sel4_server_t *srv, uint32_t opcode,
                                        sel4_handler_fn fn, void *ctx)
{
    if (srv->handler_count >= SEL4_SERVER_MAX_HANDLERS) return -1;
    srv->handlers[srv->handler_count].opcode = opcode;
    srv->handlers[srv->handler_count].fn     = fn;
    srv->handlers[srv->handler_count].ctx    = ctx;
    srv->handler_count++;
    return 0;
}
static inline uint32_t sel4_server_dispatch(sel4_server_t *srv,
                                             sel4_badge_t badge,
                                             const sel4_msg_t *req,
                                             sel4_msg_t *rep)
{
    for (uint32_t i = 0; i < srv->handler_count; i++) {
        if (srv->handlers[i].opcode == req->opcode) {
            uint32_t rc = srv->handlers[i].fn(badge, req, rep,
                                               srv->handlers[i].ctx);
            rep->opcode = rc;
            return rc;
        }
    }
    rep->opcode = SEL4_ERR_INVALID_OP;
    rep->length = 0;
    return SEL4_ERR_INVALID_OP;
}

/* data_rd32 / data_wr32 helpers (little-endian) */
static inline uint32_t data_rd32(const uint8_t *d, int off)
{
    return (uint32_t)d[off]
         | ((uint32_t)d[off+1] <<  8)
         | ((uint32_t)d[off+2] << 16)
         | ((uint32_t)d[off+3] << 24);
}
static inline void data_wr32(uint8_t *d, int off, uint32_t v)
{
    d[off  ] = (uint8_t)(v      );
    d[off+1] = (uint8_t)(v >>  8);
    d[off+2] = (uint8_t)(v >> 16);
    d[off+3] = (uint8_t)(v >> 24);
}

/* seL4_Signal stub — no-op in test mode */
static inline void seL4_Signal(seL4_CPtr cap) { (void)cap; }

/* lwIP stubs */
struct netif { int (*linkoutput)(struct netif *, void *); };
static struct netif g_netif;
typedef void* pbuf;
#define PBUF_LINK  0
#define PBUF_RAM   0
static inline pbuf *pbuf_alloc(int l, unsigned short s, int t) { (void)l;(void)s;(void)t; return (void*)0; }
static inline void pbuf_free(pbuf *p) { (void)p; }
static inline void net_server_lwip_init(uintptr_t m, uint32_t a, uint32_t b) { (void)m;(void)a;(void)b; }
static inline void lwip_tick(void) {}
static inline void lwip_virtio_rx_poll(void) {}
static inline void sys_check_timeouts(void) {}
static inline int  lwip_net_recv(uint8_t id, uint8_t *buf, uint32_t len) { (void)id;(void)buf;(void)len; return 0; }
static inline int  lwip_net_connect(uint8_t id, uint32_t ip, uint16_t port) { (void)id;(void)ip;(void)port; return -1; }
static inline int  lwip_net_send(uint8_t id, const uint8_t *d, uint16_t l) { (void)id;(void)d;(void)l; return -1; }
static inline int  lwip_conn_state(uint8_t id) { (void)id; return 0; }
struct lwip_conn { void *tcp; uint32_t state; uint32_t rx_buf_len; uint8_t rx_buf[256]; };
static struct lwip_conn g_conns[16];
struct tcp_pcb;
static inline int tcp_close(struct tcp_pcb *p) { (void)p; return 0; }

/* log_drain stub */
static inline void log_drain_write(int a, int b, const char *s) { (void)a;(void)b;(void)s; }

/* agentos_log_boot stub */
static inline void agentos_log_boot(const char *s) { (void)s; }

/* CAP_CLASS_NET */
#ifndef CAP_CLASS_NET
#define CAP_CLASS_NET  (1u << 1)
#endif

#else /* !AGENTOS_TEST_HOST */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "lwip_sys.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "sel4_ipc.h"
#include "sel4_server.h"
#include "sel4_client.h"
#include "nameserver.h"

/* data_rd32 / data_wr32 helpers (little-endian) */
static inline uint32_t data_rd32(const uint8_t *d, int off)
{
    return (uint32_t)d[off]
         | ((uint32_t)d[off+1] <<  8)
         | ((uint32_t)d[off+2] << 16)
         | ((uint32_t)d[off+3] << 24);
}
static inline void data_wr32(uint8_t *d, int off, uint32_t v)
{
    d[off  ] = (uint8_t)(v      );
    d[off+1] = (uint8_t)(v >>  8);
    d[off+2] = (uint8_t)(v >> 16);
    d[off+3] = (uint8_t)(v >> 24);
}

#endif /* AGENTOS_TEST_HOST */

/* ── Contract header (opcode constants, NET_OK etc.) ─────────────────────── */
#include "net_server.h"

/* ── virtio-MMIO register offsets ───────────────────────────────────────── */
#define VIRTIO_MMIO_MAGIC_VALUE     0x000u
#define VIRTIO_MMIO_VERSION         0x004u
#define VIRTIO_MMIO_DEVICE_ID       0x008u
#define VIRTIO_MMIO_QUEUE_NOTIFY    0x050u
#define VIRTIO_MMIO_STATUS          0x070u
#define VIRTIO_MMIO_MAGIC           0x74726976u  /* "virt" */
#define VIRTIO_STATUS_ACKNOWLEDGE   (1u << 0)
#define VIRTIO_STATUS_DRIVER        (1u << 1)
#define VIRTIO_STATUS_DRIVER_OK     (1u << 2)
#define VIRTIO_STATUS_FEATURES_OK   (1u << 3)

/* ── Shared memory base addresses ───────────────────────────────────────── */
uintptr_t net_packet_shmem_vaddr;
#define NET_SHMEM ((volatile uint8_t *)net_packet_shmem_vaddr)

uintptr_t net_mmio_vaddr;
uintptr_t log_drain_rings_vaddr;
uintptr_t vibe_staging_vaddr;

/* ── Module state ────────────────────────────────────────────────────────── */
static net_vnic_t  vnics[NET_MAX_VNICS];
static uint32_t    active_vnic_count = 0;
static bool        net_hw_present    = false;

/* sel4_server_t instance for this PD */
static sel4_server_t g_srv;

/* ── Minimal string helpers (no libc) ───────────────────────────────────── */

static uint32_t ns_strlen(const char *s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

/* ── Decimal formatting helper ───────────────────────────────────────────── */
static void log_dec(uint32_t v) {
    if (v == 0) { log_drain_write(16, 16, "0"); return; }
    char buf[12];
    int  i = 11;
    buf[i] = '\0';
    while (v > 0 && i > 0) { buf[--i] = '0' + (char)(v % 10); v /= 10; }
    log_drain_write(16, 16, &buf[i]);
}

/* ── Hex formatting helper ───────────────────────────────────────────────── */
static void log_hex(uint32_t v) {
    static const char hex[] = "0123456789abcdef";
    char buf[11];
    buf[0]  = '0';
    buf[1]  = 'x';
    buf[2]  = hex[(v >> 28) & 0xf];
    buf[3]  = hex[(v >> 24) & 0xf];
    buf[4]  = hex[(v >> 20) & 0xf];
    buf[5]  = hex[(v >> 16) & 0xf];
    buf[6]  = hex[(v >> 12) & 0xf];
    buf[7]  = hex[(v >>  8) & 0xf];
    buf[8]  = hex[(v >>  4) & 0xf];
    buf[9]  = hex[ v        & 0xf];
    buf[10] = '\0';
    log_drain_write(16, 16, buf);
}

/* ── MMIO read/write helpers ──────────────────────────────────────────────── */
static inline uint32_t mmio_read32(uintptr_t base, uint32_t offset) {
    return *(volatile uint32_t *)(base + offset);
}
static inline void mmio_write32(uintptr_t base, uint32_t offset, uint32_t val) {
    *(volatile uint32_t *)(base + offset) = val;
}

/* ── vNIC ring accessor ──────────────────────────────────────────────────── */
static volatile net_vnic_ring_t *slot_ring(uint32_t shmem_slot) {
    return (volatile net_vnic_ring_t *)(net_packet_shmem_vaddr
                                        + NET_SLOT_OFFSET(shmem_slot));
}

/* ── virtio-net probe ────────────────────────────────────────────────────── */
static void probe_virtio_net(void) {
    if (!net_mmio_vaddr) {
        log_drain_write(16, 16, "[net_server] virtio-net: MMIO vaddr not mapped, stub mode\n");
        return;
    }

    uint32_t magic     = mmio_read32(net_mmio_vaddr, VIRTIO_MMIO_MAGIC_VALUE);
    uint32_t version   = mmio_read32(net_mmio_vaddr, VIRTIO_MMIO_VERSION);
    uint32_t device_id = mmio_read32(net_mmio_vaddr, VIRTIO_MMIO_DEVICE_ID);

    if (magic != VIRTIO_MMIO_MAGIC || version != 2u
            || device_id != VIRTIO_NET_DEVICE_ID) {
        log_drain_write(16, 16, "[net_server] virtio-net not detected (magic=");
        log_hex(magic);
        log_drain_write(16, 16, " ver=");
        log_dec(version);
        log_drain_write(16, 16, " dev=");
        log_dec(device_id);
        log_drain_write(16, 16, "), stub mode\n");
        return;
    }

    net_hw_present = true;

    mmio_write32(net_mmio_vaddr, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    mmio_write32(net_mmio_vaddr, VIRTIO_MMIO_STATUS,
                 VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    log_drain_write(16, 16, "[net_server] virtio-net detected at ");
    log_hex((uint32_t)net_mmio_vaddr);
    log_drain_write(16, 16, ", hw present\n");

    net_server_lwip_init(net_mmio_vaddr, 0u, 0u);
}

/* ── vNIC slot allocation ────────────────────────────────────────────────── */

static int alloc_vnic_slot(void) {
    for (int i = 0; i < (int)NET_MAX_VNICS; i++) {
        if (!vnics[i].active)
            return i;
    }
    return -1;
}

static net_vnic_t *find_vnic(uint32_t vnic_id) {
    for (int i = 0; i < (int)NET_MAX_VNICS; i++) {
        if (vnics[i].active && vnics[i].vnic_id == vnic_id)
            return &vnics[i];
    }
    return NULL;
}

/* ── shmem ring initialisation ───────────────────────────────────────────── */
static void init_vnic_ring(uint32_t shmem_slot, uint32_t vnic_id) {
    volatile net_vnic_ring_t *r = slot_ring(shmem_slot);
    r->magic    = NET_VNIC_MAGIC;
    r->vnic_id  = vnic_id;
    r->tx_head  = 0;
    r->tx_tail  = 0;
    r->rx_head  = 0;
    r->rx_tail  = 0;
    r->tx_drops = 0;
    r->rx_drops = 0;
    __asm__ volatile("" ::: "memory");
}

static void clear_vnic_ring(uint32_t shmem_slot) {
    volatile net_vnic_ring_t *r = slot_ring(shmem_slot);
    r->magic = 0;
    __asm__ volatile("" ::: "memory");
}

/* ── ACL enforcement ─────────────────────────────────────────────────────── */
static bool acl_outbound_allowed(const net_vnic_t *v) {
    return (v->acl_flags & NET_ACL_ALLOW_OUTBOUND) != 0;
}

/* ── Nameserver registration ─────────────────────────────────────────────── */
static void register_with_nameserver(seL4_CPtr ns_ep) {
    if (!ns_ep) return;
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = (uint32_t)OP_NS_REGISTER;
    data_wr32(req.data,  0, 0u);          /* channel_id: 0 (resolved by ns) */
    data_wr32(req.data,  4, 0u);          /* pd_id: let nameserver assign    */
    data_wr32(req.data,  8, CAP_CLASS_NET);
    data_wr32(req.data, 12, NET_VERSION);
    /* Name "net" packed at data[16..] */
    req.data[16] = 'n'; req.data[17] = 'e'; req.data[18] = 't'; req.data[19] = '\0';
    req.length = 20;
#ifndef AGENTOS_TEST_HOST
    sel4_call(ns_ep, &req, &rep);
#else
    (void)rep;
#endif
    log_drain_write(16, 16, "[net_server] registered with nameserver as 'net'\n");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Handler: OP_NET_VNIC_CREATE
 *   req.data[0..3]  = requested_vnic_id
 *   req.data[4..7]  = cap_classes
 *   req.data[8..11] = caller_pd
 * ═══════════════════════════════════════════════════════════════════════════ */
static uint32_t handle_vnic_create(sel4_badge_t badge __attribute__((unused)),
                                    const sel4_msg_t *req,
                                    sel4_msg_t *rep,
                                    void *ctx __attribute__((unused)))
{
    uint32_t requested_id = data_rd32(req->data, 0);
    uint32_t cap_classes  = data_rd32(req->data, 4);
    uint32_t caller_pd    = data_rd32(req->data, 8);

    if (!(cap_classes & CAP_CLASS_NET)) {
        log_drain_write(16, 16, "[net_server] VNIC_CREATE denied: missing CAP_CLASS_NET pd=");
        log_dec(caller_pd);
        log_drain_write(16, 16, "\n");
        data_wr32(rep->data, 0, NET_ERR_PERM);
        rep->length = 4;
        return SEL4_ERR_PERM;
    }

    int slot = alloc_vnic_slot();
    if (slot < 0) {
        log_drain_write(16, 16, "[net_server] VNIC_CREATE failed: table full\n");
        data_wr32(rep->data, 0, NET_ERR_NO_VNICS);
        rep->length = 4;
        return SEL4_ERR_NO_MEM;
    }

    uint32_t vnic_id;
    if (requested_id == 0xFFu || requested_id >= NET_MAX_VNICS
            || find_vnic(requested_id) != NULL) {
        vnic_id = (uint32_t)slot;
    } else {
        vnic_id = requested_id;
    }

    net_vnic_t *v    = &vnics[slot];
    v->active        = true;
    v->vnic_id       = vnic_id;
    v->owner_pd      = caller_pd;
    v->cap_classes   = cap_classes;
    v->acl_flags     = NET_ACL_ALLOW_OUTBOUND;
    v->port_count    = 0;
    v->shmem_slot    = (uint32_t)slot;
    v->rx_packets    = 0;
    v->tx_packets    = 0;

    v->mac[0] = 0x02; v->mac[1] = 0x00; v->mac[2] = 0x00;
    v->mac[3] = 0x00; v->mac[4] = 0x00;
    v->mac[5] = (uint8_t)(vnic_id & 0xFFu);

    for (uint32_t i = 0; i < NET_MAX_BOUND_PORTS; i++)
        v->bound_ports[i] = 0;

    init_vnic_ring((uint32_t)slot, vnic_id);
    active_vnic_count++;

    uint32_t slot_offset = NET_SLOT_OFFSET((uint32_t)slot);

    log_drain_write(16, 16, "[net_server] VNIC created: id=");
    log_dec(vnic_id);
    log_drain_write(16, 16, " pd=");
    log_dec(caller_pd);
    log_drain_write(16, 16, " slot=");
    log_dec((uint32_t)slot);
    log_drain_write(16, 16, "\n");

    data_wr32(rep->data, 0, NET_OK);
    data_wr32(rep->data, 4, vnic_id);
    data_wr32(rep->data, 8, slot_offset);
    rep->length = 12;
    return SEL4_ERR_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Handler: OP_NET_VNIC_DESTROY
 *   req.data[0..3] = vnic_id
 * ═══════════════════════════════════════════════════════════════════════════ */
static uint32_t handle_vnic_destroy(sel4_badge_t badge __attribute__((unused)),
                                     const sel4_msg_t *req,
                                     sel4_msg_t *rep,
                                     void *ctx __attribute__((unused)))
{
    uint32_t vnic_id = data_rd32(req->data, 0);

    net_vnic_t *v = find_vnic(vnic_id);
    if (!v) {
        log_drain_write(16, 16, "[net_server] VNIC_DESTROY: not found id=");
        log_dec(vnic_id);
        log_drain_write(16, 16, "\n");
        data_wr32(rep->data, 0, NET_ERR_NOT_FOUND);
        rep->length = 4;
        return SEL4_ERR_NOT_FOUND;
    }

    log_drain_write(16, 16, "[net_server] VNIC destroyed: id=");
    log_dec(vnic_id);
    log_drain_write(16, 16, " pd=");
    log_dec(v->owner_pd);
    log_drain_write(16, 16, "\n");

    clear_vnic_ring(v->shmem_slot);
    v->active = false;
    if (active_vnic_count > 0)
        active_vnic_count--;

    data_wr32(rep->data, 0, NET_OK);
    rep->length = 4;
    return SEL4_ERR_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Handler: OP_NET_VNIC_SEND
 *   req.data[0..3]  = vnic_id
 *   req.data[4..7]  = pkt_offset
 *   req.data[8..11] = pkt_len
 * ═══════════════════════════════════════════════════════════════════════════ */
static uint32_t handle_vnic_send(sel4_badge_t badge __attribute__((unused)),
                                  const sel4_msg_t *req,
                                  sel4_msg_t *rep,
                                  void *ctx __attribute__((unused)))
{
    uint32_t vnic_id    = data_rd32(req->data, 0);
    uint32_t pkt_offset = data_rd32(req->data, 4);
    uint32_t pkt_len    = data_rd32(req->data, 8);

    net_vnic_t *v = find_vnic(vnic_id);
    if (!v) {
        data_wr32(rep->data, 0, NET_ERR_NOT_FOUND);
        data_wr32(rep->data, 4, 0);
        rep->length = 8;
        return SEL4_ERR_NOT_FOUND;
    }

    if (!acl_outbound_allowed(v)) {
        log_drain_write(16, 16, "[net_server] SEND denied by ACL: vnic=");
        log_dec(vnic_id);
        log_drain_write(16, 16, " pd=");
        log_dec(v->owner_pd);
        log_drain_write(16, 16, "\n");
        data_wr32(rep->data, 0, NET_ERR_PERM);
        data_wr32(rep->data, 4, 0);
        rep->length = 8;
        return SEL4_ERR_PERM;
    }

    if (pkt_offset >= NET_SHMEM_TOTAL || pkt_len == 0
            || pkt_offset + pkt_len > NET_SHMEM_TOTAL) {
        data_wr32(rep->data, 0, NET_ERR_INVAL);
        data_wr32(rep->data, 4, 0);
        rep->length = 8;
        return SEL4_ERR_BAD_ARG;
    }

    uint32_t dest_ip = 0;
    const uint8_t *pkt_base = (const uint8_t *)(net_packet_shmem_vaddr + pkt_offset);

    if (pkt_len >= 34u) {
        uint16_t ethertype = ((uint16_t)pkt_base[12] << 8) | pkt_base[13];
        if (ethertype == 0x0800u) {
            dest_ip = ((uint32_t)pkt_base[30] << 24)
                    | ((uint32_t)pkt_base[31] << 16)
                    | ((uint32_t)pkt_base[32] <<  8)
                    |  (uint32_t)pkt_base[33];
        }
    }

    if (NET_IP_IS_LOOPBACK(dest_ip)) {
        bool delivered = false;
        for (int i = 0; i < (int)NET_MAX_VNICS && !delivered; i++) {
            net_vnic_t *dst = &vnics[i];
            if (!dst->active || dst->vnic_id == vnic_id)
                continue;

            volatile net_vnic_ring_t *dst_ring = slot_ring(dst->shmem_slot);
            uint32_t next_rx_head = (dst_ring->rx_head + 1) % NET_SHMEM_DATA_SIZE;
            if (next_rx_head == dst_ring->rx_tail) {
                dst_ring->rx_drops++;
            } else {
                dst->rx_packets++;
                delivered = true;
                log_drain_write(16, 16, "[net_server] loopback routed: src_vnic=");
                log_dec(vnic_id);
                log_drain_write(16, 16, " dst_vnic=");
                log_dec(dst->vnic_id);
                log_drain_write(16, 16, "\n");
            }
        }
    } else {
        if (net_hw_present) {
#ifndef AGENTOS_TEST_HOST
            const uint8_t *pkt = (const uint8_t *)(net_packet_shmem_vaddr + pkt_offset);
            struct pbuf *p = pbuf_alloc(PBUF_LINK, (u16_t)pkt_len, PBUF_RAM);
            if (p) {
                uint8_t *dst = (uint8_t *)p->payload;
                for (uint16_t i = 0; i < (uint16_t)pkt_len; i++)
                    dst[i] = pkt[i];
                g_netif.linkoutput(&g_netif, p);
                pbuf_free(p);
            }
#endif
        } else {
            log_drain_write(16, 16, "[net_server] TX dropped (no hw): vnic=");
            log_dec(vnic_id);
            log_drain_write(16, 16, " len=");
            log_dec(pkt_len);
            log_drain_write(16, 16, "\n");
        }
    }

    v->tx_packets++;

    data_wr32(rep->data, 0, NET_OK);
    data_wr32(rep->data, 4, pkt_len);
    rep->length = 8;
    return SEL4_ERR_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Handler: OP_NET_VNIC_RECV
 *   req.data[0..3]  = vnic_id
 *   req.data[4..7]  = buf_offset
 *   req.data[8..11] = max_len
 * ═══════════════════════════════════════════════════════════════════════════ */
static uint32_t handle_vnic_recv(sel4_badge_t badge __attribute__((unused)),
                                  const sel4_msg_t *req,
                                  sel4_msg_t *rep,
                                  void *ctx __attribute__((unused)))
{
    uint32_t vnic_id    = data_rd32(req->data, 0);
    uint32_t buf_offset = data_rd32(req->data, 4);
    uint32_t max_len    = data_rd32(req->data, 8);

    net_vnic_t *v = find_vnic(vnic_id);
    if (!v) {
        data_wr32(rep->data, 0, NET_ERR_NOT_FOUND);
        data_wr32(rep->data, 4, 0);
        rep->length = 8;
        return SEL4_ERR_NOT_FOUND;
    }

    if (buf_offset >= NET_SHMEM_TOTAL
            || (max_len > 0 && buf_offset + max_len > NET_SHMEM_TOTAL)) {
        data_wr32(rep->data, 0, NET_ERR_INVAL);
        data_wr32(rep->data, 4, 0);
        rep->length = 8;
        return SEL4_ERR_BAD_ARG;
    }

    int n = lwip_net_recv((uint8_t)(vnic_id & 0xFFu),
                          (uint8_t *)(net_packet_shmem_vaddr + buf_offset),
                          max_len > 65536u ? 65536u : max_len);
    v->rx_packets += (n > 0) ? 1u : 0u;

    data_wr32(rep->data, 0, NET_OK);
    data_wr32(rep->data, 4, (uint32_t)(n > 0 ? n : 0));
    rep->length = 8;
    return SEL4_ERR_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Handler: OP_NET_BIND
 *   req.data[0..3]  = vnic_id
 *   req.data[4..7]  = port
 *   req.data[8..11] = protocol
 * ═══════════════════════════════════════════════════════════════════════════ */
static uint32_t handle_net_bind(sel4_badge_t badge __attribute__((unused)),
                                 const sel4_msg_t *req,
                                 sel4_msg_t *rep,
                                 void *ctx __attribute__((unused)))
{
    uint32_t vnic_id  = data_rd32(req->data, 0);
    uint32_t port     = data_rd32(req->data, 4);
    uint32_t protocol = data_rd32(req->data, 8);
    (void)protocol;

    if (port == 0 || port > 65535u) {
        data_wr32(rep->data, 0, NET_ERR_INVAL);
        rep->length = 4;
        return SEL4_ERR_BAD_ARG;
    }

    net_vnic_t *v = find_vnic(vnic_id);
    if (!v) {
        data_wr32(rep->data, 0, NET_ERR_NOT_FOUND);
        rep->length = 4;
        return SEL4_ERR_NOT_FOUND;
    }

    if (v->port_count >= NET_MAX_BOUND_PORTS) {
        data_wr32(rep->data, 0, NET_ERR_NO_VNICS);
        rep->length = 4;
        return SEL4_ERR_NO_MEM;
    }

    v->bound_ports[v->port_count++] = port;

    log_drain_write(16, 16, "[net_server] BIND vnic=");
    log_dec(vnic_id);
    log_drain_write(16, 16, " port=");
    log_dec(port);
    log_drain_write(16, 16, " proto=");
    log_dec(protocol);
    log_drain_write(16, 16, "\n");

    data_wr32(rep->data, 0, NET_OK);
    rep->length = 4;
    return SEL4_ERR_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Handler: OP_NET_CONNECT
 *   req.data[0..3]  = vnic_id
 *   req.data[4..7]  = dest_ip
 *   req.data[8..11] = dest_port
 *   req.data[12..15]= protocol
 * ═══════════════════════════════════════════════════════════════════════════ */
static uint32_t handle_net_connect(sel4_badge_t badge __attribute__((unused)),
                                    const sel4_msg_t *req,
                                    sel4_msg_t *rep,
                                    void *ctx __attribute__((unused)))
{
    uint32_t vnic_id   = data_rd32(req->data,  0);
    uint32_t dest_ip   = data_rd32(req->data,  4);
    uint32_t dest_port = data_rd32(req->data,  8);
    uint32_t protocol  = data_rd32(req->data, 12);

    net_vnic_t *v = find_vnic(vnic_id);
    if (!v) {
        data_wr32(rep->data, 0, NET_ERR_NOT_FOUND);
        data_wr32(rep->data, 4, 0);
        rep->length = 8;
        return SEL4_ERR_NOT_FOUND;
    }

    log_drain_write(16, 16, "[net_server] CONNECT: vnic=");
    log_dec(vnic_id);
    log_drain_write(16, 16, " dst_ip=");
    log_hex(dest_ip);
    log_drain_write(16, 16, " dst_port=");
    log_dec(dest_port);
    log_drain_write(16, 16, " proto=");
    log_dec(protocol);
    log_drain_write(16, 16, "\n");

    int err = lwip_net_connect((uint8_t)(vnic_id & 0xFFu),
                                dest_ip, (uint16_t)dest_port);
    data_wr32(rep->data, 0, err == 0 ? NET_OK : NET_ERR_STUB);
    data_wr32(rep->data, 4, vnic_id);
    rep->length = 8;
    return SEL4_ERR_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Handler: OP_NET_STATUS
 *   req.data[0..3] = vnic_id  (0xFFFFFFFF = global)
 * ═══════════════════════════════════════════════════════════════════════════ */
static uint32_t handle_net_status(sel4_badge_t badge __attribute__((unused)),
                                   const sel4_msg_t *req,
                                   sel4_msg_t *rep,
                                   void *ctx __attribute__((unused)))
{
    uint32_t vnic_id = data_rd32(req->data, 0);

    if (vnic_id == 0xFFFFFFFFu) {
        uint64_t total_rx = 0, total_tx = 0;
        for (int i = 0; i < (int)NET_MAX_VNICS; i++) {
            if (vnics[i].active) {
                total_rx += vnics[i].rx_packets;
                total_tx += vnics[i].tx_packets;
            }
        }
        data_wr32(rep->data,  0, NET_OK);
        data_wr32(rep->data,  4, active_vnic_count);
        data_wr32(rep->data,  8, (uint32_t)(total_rx & 0xFFFFFFFFu));
        data_wr32(rep->data, 12, (uint32_t)(total_tx & 0xFFFFFFFFu));
        rep->length = 16;
        return SEL4_ERR_OK;
    }

    net_vnic_t *v = find_vnic(vnic_id);
    if (!v) {
        data_wr32(rep->data, 0, NET_ERR_NOT_FOUND);
        rep->length = 4;
        return SEL4_ERR_NOT_FOUND;
    }

    data_wr32(rep->data,  0, NET_OK);
    data_wr32(rep->data,  4, active_vnic_count);
    data_wr32(rep->data,  8, (uint32_t)(v->rx_packets & 0xFFFFFFFFu));
    data_wr32(rep->data, 12, (uint32_t)(v->tx_packets & 0xFFFFFFFFu));
    rep->length = 16;
    return SEL4_ERR_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Handler: OP_NET_SET_ACL
 *   req.data[0..3] = vnic_id
 *   req.data[4..7] = acl_flags
 * ═══════════════════════════════════════════════════════════════════════════ */
static uint32_t handle_net_set_acl(sel4_badge_t badge __attribute__((unused)),
                                    const sel4_msg_t *req,
                                    sel4_msg_t *rep,
                                    void *ctx __attribute__((unused)))
{
    uint32_t vnic_id   = data_rd32(req->data, 0);
    uint32_t acl_flags = data_rd32(req->data, 4);

    net_vnic_t *v = find_vnic(vnic_id);
    if (!v) {
        data_wr32(rep->data, 0, NET_ERR_NOT_FOUND);
        rep->length = 4;
        return SEL4_ERR_NOT_FOUND;
    }

    uint32_t old_flags = v->acl_flags;
    v->acl_flags = acl_flags;

    log_drain_write(16, 16, "[net_server] SET_ACL vnic=");
    log_dec(vnic_id);
    log_drain_write(16, 16, " old_flags=");
    log_hex(old_flags);
    log_drain_write(16, 16, " new_flags=");
    log_hex(acl_flags);
    log_drain_write(16, 16, "\n");

    data_wr32(rep->data, 0, NET_OK);
    rep->length = 4;
    return SEL4_ERR_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Handler: OP_NET_HEALTH
 * ═══════════════════════════════════════════════════════════════════════════ */
static uint32_t handle_net_health(sel4_badge_t badge __attribute__((unused)),
                                   const sel4_msg_t *req __attribute__((unused)),
                                   sel4_msg_t *rep,
                                   void *ctx __attribute__((unused)))
{
    data_wr32(rep->data, 0, NET_OK);
    data_wr32(rep->data, 4, active_vnic_count);
    data_wr32(rep->data, 8, NET_VERSION);
    rep->length = 12;
    return SEL4_ERR_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Handler: OP_NET_CONN_STATE
 *   req.data[0] = vnic_id (uint8)
 * ═══════════════════════════════════════════════════════════════════════════ */
static uint32_t handle_net_conn_state(sel4_badge_t badge __attribute__((unused)),
                                       const sel4_msg_t *req,
                                       sel4_msg_t *rep,
                                       void *ctx __attribute__((unused)))
{
    uint8_t vid = (uint8_t)data_rd32(req->data, 0);
    data_wr32(rep->data, 0, NET_OK);
    data_wr32(rep->data, 4, (uint32_t)lwip_conn_state(vid));
    rep->length = 8;
    return SEL4_ERR_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Handler: OP_NET_TCP_CLOSE
 *   req.data[0] = vnic_id (uint8)
 * ═══════════════════════════════════════════════════════════════════════════ */
static uint32_t handle_net_tcp_close(sel4_badge_t badge __attribute__((unused)),
                                      const sel4_msg_t *req,
                                      sel4_msg_t *rep,
                                      void *ctx __attribute__((unused)))
{
    uint8_t vid = (uint8_t)data_rd32(req->data, 0);
#ifndef AGENTOS_TEST_HOST
    if (vid < NET_MAX_VNICS && g_conns[vid].tcp) {
        tcp_close(g_conns[vid].tcp);
        g_conns[vid].tcp   = NULL;
        g_conns[vid].state = 0u;
    }
#else
    (void)vid;
#endif
    data_wr32(rep->data, 0, NET_OK);
    rep->length = 4;
    return SEL4_ERR_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Handler: OP_NET_HTTP_POST
 *   req.data[0..3]  = url_offset
 *   req.data[4..7]  = url_len
 *   req.data[8..11] = body_offset
 *   req.data[12..15]= body_len
 * ═══════════════════════════════════════════════════════════════════════════ */
#define HTTP_CONN_ID  ((uint8_t)(NET_MAX_VNICS - 1u))
#define BRIDGE_IP_BE  0x0A000202u
#define BRIDGE_PORT   8790u
#define STAGING_HTTP_URL_OFFSET  0x1FF000UL
#define STAGING_HTTP_OFFSET      0x200000UL

static uint32_t buf_append_h(uint8_t *dst, uint32_t pos, uint32_t cap,
                              const char *src, uint32_t n)
{
    for (uint32_t i = 0; i < n && pos < cap; i++)
        dst[pos++] = (uint8_t)src[i];
    return pos;
}

static uint32_t buf_append_dec_h(uint8_t *dst, uint32_t pos, uint32_t cap, uint32_t v)
{
    char tmp[12];
    int  i = 11;
    tmp[i] = '\0';
    if (v == 0) { tmp[--i] = '0'; }
    else { while (v > 0 && i > 0) { tmp[--i] = (char)('0' + (int)(v % 10u)); v /= 10u; } }
    return buf_append_h(dst, pos, cap, &tmp[i], (uint32_t)(11 - i));
}

static uint32_t handle_net_http_post(sel4_badge_t badge __attribute__((unused)),
                                      const sel4_msg_t *req,
                                      sel4_msg_t *rep,
                                      void *ctx __attribute__((unused)))
{
    uint32_t url_offset  = data_rd32(req->data,  0);
    uint32_t url_len     = data_rd32(req->data,  4);
    uint32_t body_offset = data_rd32(req->data,  8);
    uint32_t body_len    = data_rd32(req->data, 12);

    if (!vibe_staging_vaddr) {
        log_drain_write(16, 16, "[net_server] HTTP_POST: staging not mapped\n");
        data_wr32(rep->data, 0, 0); data_wr32(rep->data, 4, 0); data_wr32(rep->data, 8, 0);
        rep->length = 12;
        return SEL4_ERR_OK;
    }

    const char *url  = (const char *)(vibe_staging_vaddr + url_offset);
    const char *body = (const char *)(vibe_staging_vaddr + body_offset);

    const char *path = "/";
    if (url_len >= 8u) {
        const char *p = url + 7u;
        while ((uint32_t)(p - url) < url_len && *p && *p != '/') p++;
        if ((uint32_t)(p - url) < url_len && *p == '/') path = p;
    }
    uint32_t path_len = 0;
    while (path_len + (uint32_t)(path - url) < url_len &&
           path[path_len] && path[path_len] != ' ') path_len++;

    static uint8_t http_hdr[512];
    uint32_t hlen = 0;
    const uint32_t hcap = (uint32_t)sizeof(http_hdr);

#define HL(s)   hlen = buf_append_h(http_hdr, hlen, hcap, (s), (uint32_t)(sizeof(s)-1u))
#define HS(s,n) hlen = buf_append_h(http_hdr, hlen, hcap, (s), (uint32_t)(n))
#define HD(v)   hlen = buf_append_dec_h(http_hdr, hlen, hcap, (v))

    HL("POST ");
    HS(path, path_len);
    HL(" HTTP/1.1\r\n");
    HL("Host: 10.0.2.2:8790\r\n");
    HL("Content-Type: application/json\r\n");
    HL("Content-Length: ");
    HD(body_len);
    HL("\r\nConnection: close\r\n\r\n");

#undef HL
#undef HS
#undef HD

    (void)body;   /* body used in actual lwIP send below */

#ifndef AGENTOS_TEST_HOST
    if (g_conns[HTTP_CONN_ID].tcp) {
        tcp_close(g_conns[HTTP_CONN_ID].tcp);
        g_conns[HTTP_CONN_ID].tcp = NULL;
    }
    g_conns[HTTP_CONN_ID].state      = 0u;
    g_conns[HTTP_CONN_ID].rx_buf_len = 0u;

    int err = lwip_net_connect(HTTP_CONN_ID, BRIDGE_IP_BE, BRIDGE_PORT);
    if (err != 0) {
        log_drain_write(16, 16, "[net_server] HTTP_POST: connect error\n");
        data_wr32(rep->data, 0, 0); data_wr32(rep->data, 4, 0); data_wr32(rep->data, 8, 0);
        rep->length = 12;
        return SEL4_ERR_OK;
    }

    for (uint32_t i = 0; i < 500u; i++) {
        lwip_tick(); sys_check_timeouts(); lwip_virtio_rx_poll();
        if (g_conns[HTTP_CONN_ID].state == 2u) break;
        if (g_conns[HTTP_CONN_ID].state == 3u) break;
    }

    if (g_conns[HTTP_CONN_ID].state != 2u) {
        log_drain_write(16, 16, "[net_server] HTTP_POST: connect timeout\n");
        data_wr32(rep->data, 0, 0); data_wr32(rep->data, 4, 0); data_wr32(rep->data, 8, 0);
        rep->length = 12;
        return SEL4_ERR_OK;
    }

    if (lwip_net_send(HTTP_CONN_ID, http_hdr, (uint16_t)hlen) < 0) {
        data_wr32(rep->data, 0, 0); data_wr32(rep->data, 4, 0); data_wr32(rep->data, 8, 0);
        rep->length = 12;
        return SEL4_ERR_OK;
    }

    if (body_len > 0u) {
        uint16_t blen16 = (body_len > 65535u) ? 65535u : (uint16_t)body_len;
        lwip_net_send(HTTP_CONN_ID, (const uint8_t *)body, blen16);
    }

    for (uint32_t i = 0; i < 3000u; i++) {
        lwip_tick(); sys_check_timeouts(); lwip_virtio_rx_poll();
        if (g_conns[HTTP_CONN_ID].rx_buf_len > 0u) break;
        if (g_conns[HTTP_CONN_ID].state == 3u) break;
    }

    uint32_t rx_len = g_conns[HTTP_CONN_ID].rx_buf_len;
    if (rx_len == 0u) {
        data_wr32(rep->data, 0, 0); data_wr32(rep->data, 4, 0); data_wr32(rep->data, 8, 0);
        rep->length = 12;
        return SEL4_ERR_OK;
    }

    uint32_t http_status = 0u;
    const char *resp = (const char *)g_conns[HTTP_CONN_ID].rx_buf;
    if (rx_len > 9u && resp[0] == 'H' && resp[1] == 'T' && resp[2] == 'T') {
        const char *p = resp;
        while ((uint32_t)(p - resp) < rx_len && *p != ' ') p++;
        if (*p == ' ') {
            p++;
            while ((uint32_t)(p - resp) < rx_len && *p >= '0' && *p <= '9') {
                http_status = http_status * 10u + (uint32_t)(*p - '0');
                p++;
            }
        }
    }

    uint32_t hdr_end = 0u;
    for (uint32_t i = 0u; i + 3u < rx_len; i++) {
        if (resp[i] == '\r' && resp[i+1u] == '\n' &&
            resp[i+2u] == '\r' && resp[i+3u] == '\n') {
            hdr_end = i + 4u;
            break;
        }
    }

    uint32_t resp_body_len = (hdr_end < rx_len) ? (rx_len - hdr_end) : 0u;

    if (resp_body_len > 0u) {
        uint8_t       *dst = (uint8_t *)(vibe_staging_vaddr + body_offset);
        const uint8_t *src = (const uint8_t *)(resp + hdr_end);
        for (uint32_t i = 0u; i < resp_body_len; i++) dst[i] = src[i];
    }

    log_drain_write(16, 16, "[net_server] HTTP_POST: status=");
    log_dec(http_status);
    log_drain_write(16, 16, " body_len=");
    log_dec(resp_body_len);
    log_drain_write(16, 16, "\n");

    data_wr32(rep->data,  0, http_status);
    data_wr32(rep->data,  4, body_offset);
    data_wr32(rep->data,  8, resp_body_len);
    rep->length = 12;
    return SEL4_ERR_OK;
#else
    /* Stub for host-side tests */
    (void)hlen;
    data_wr32(rep->data, 0, 200u);
    data_wr32(rep->data, 4, body_offset);
    data_wr32(rep->data, 8, 0u);
    rep->length = 12;
    return SEL4_ERR_OK;
#endif
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Timer notification handler — called when seL4 notification arrives on
 * timer_ntfn_cap.  Drives lwIP timers and virtio-net RX poll.
 *
 * This replaces the Microkit notified(NET_CH_TIMER) path.  No PPC, no
 * priority constraint.
 * ═══════════════════════════════════════════════════════════════════════════ */
static void net_server_timer_tick(void) {
    lwip_tick();
    lwip_virtio_rx_poll();
    sys_check_timeouts();
}

/* ═══════════════════════════════════════════════════════════════════════════
 * net_server_test_init() — called by tests to set up the server's dispatch
 * table without entering the blocking sel4_server_run() loop.
 * ═══════════════════════════════════════════════════════════════════════════ */
static void net_server_test_init(void) {
    /* Clear vNIC table */
    for (int i = 0; i < (int)NET_MAX_VNICS; i++) {
        vnics[i].active      = false;
        vnics[i].vnic_id     = 0;
        vnics[i].owner_pd    = 0;
        vnics[i].cap_classes = 0;
        vnics[i].acl_flags   = 0;
        vnics[i].port_count  = 0;
        vnics[i].shmem_slot  = 0;
        vnics[i].rx_packets  = 0;
        vnics[i].tx_packets  = 0;
        for (int j = 0; j < (int)NET_MAX_BOUND_PORTS; j++)
            vnics[i].bound_ports[j] = 0;
        vnics[i].mac[0] = 0; vnics[i].mac[1] = 0; vnics[i].mac[2] = 0;
        vnics[i].mac[3] = 0; vnics[i].mac[4] = 0; vnics[i].mac[5] = 0;
    }
    active_vnic_count = 0;
    net_hw_present    = false;

    sel4_server_init(&g_srv, 0u);
    sel4_server_register(&g_srv, OP_NET_VNIC_CREATE,  handle_vnic_create,    NULL);
    sel4_server_register(&g_srv, OP_NET_VNIC_DESTROY, handle_vnic_destroy,   NULL);
    sel4_server_register(&g_srv, OP_NET_VNIC_SEND,    handle_vnic_send,      NULL);
    sel4_server_register(&g_srv, OP_NET_VNIC_RECV,    handle_vnic_recv,      NULL);
    sel4_server_register(&g_srv, OP_NET_BIND,         handle_net_bind,       NULL);
    sel4_server_register(&g_srv, OP_NET_CONNECT,      handle_net_connect,    NULL);
    sel4_server_register(&g_srv, OP_NET_STATUS,       handle_net_status,     NULL);
    sel4_server_register(&g_srv, OP_NET_SET_ACL,      handle_net_set_acl,    NULL);
    sel4_server_register(&g_srv, OP_NET_HEALTH,       handle_net_health,     NULL);
    sel4_server_register(&g_srv, OP_NET_HTTP_POST,    handle_net_http_post,  NULL);
    sel4_server_register(&g_srv, OP_NET_CONN_STATE,   handle_net_conn_state, NULL);
    sel4_server_register(&g_srv, OP_NET_TCP_CLOSE,    handle_net_tcp_close,  NULL);
}

/* ── dispatch helper for tests ───────────────────────────────────────────── */
static uint32_t net_server_dispatch_one(sel4_badge_t badge,
                                         const sel4_msg_t *req,
                                         sel4_msg_t *rep)
{
    return sel4_server_dispatch(&g_srv, badge, req, rep);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * net_server_main() — raw seL4 IPC entry point (replaces init() / protected())
 *
 * Parameters:
 *   my_ep         — this PD's listen endpoint
 *   ns_ep         — nameserver endpoint (for registration and lookups)
 *   timer_ntfn_cap— seL4 notification cap for the 10ms lwIP timer tick
 *
 * E5-S4 KEY POINT: no priority ordering constraint needed.  net_server can
 * sit at any priority because seL4 endpoint IPC is blocking; callers wait
 * for a reply rather than relying on Microkit PPC priority scheduling.
 * ═══════════════════════════════════════════════════════════════════════════ */
#ifndef AGENTOS_TEST_HOST
void net_server_main(seL4_CPtr my_ep, seL4_CPtr ns_ep,
                     seL4_CPtr timer_ntfn_cap __attribute__((unused)))
{
    agentos_log_boot("net_server");
    log_drain_write(16, 16, "[net_server] Initialising NetServer PD (raw seL4 IPC)\n");
    log_drain_write(16, 16, "[net_server] priority ordering constraint ELIMINATED\n");

    net_server_test_init();       /* sets up vNIC table + server handlers    */
    probe_virtio_net();           /* detect hardware, init lwIP if present   */
    register_with_nameserver(ns_ep);

    log_drain_write(16, 16, "[net_server] READY — max_vnics=");
    log_dec(NET_MAX_VNICS);
    log_drain_write(16, 16, " hw=");
    log_drain_write(16, 16, net_hw_present ? "1" : "0");
    log_drain_write(16, 16, "\n");

    /*
     * NOTE: timer_ntfn_cap notifications drive lwIP timers.
     * In a full seL4 implementation this would be a separate notification
     * polling thread.  For E5-S4 the tick is handled by the caller signalling
     * OP_NET_VNIC_RECV which indirectly polls lwIP, or via an explicit
     * OP_NET_HEALTH poll cycle.  The timer_ntfn_cap parameter is reserved
     * for the future notification polling extension.
     */
    g_srv.ep = my_ep;
    sel4_server_run(&g_srv);   /* NEVER RETURNS */
}

void pd_main(seL4_CPtr my_ep, seL4_CPtr ns_ep) { net_server_main(my_ep, ns_ep, 0u); }

#endif /* !AGENTOS_TEST_HOST */
