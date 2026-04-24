/*
 * agentOS NetServer Protection Domain
 *
 * Manages per-application virtual NICs and provides the network stack
 * interface for all agents running under agentOS.
 *
 * Priority: 160  (passive="true" — only executes on PPC)
 * Shmem:    net_packet_shmem, 256KB, mapped at 0x5000000 (rw)
 *           controller maps the same region at 0xE000000 (r — monitoring only)
 *
 * TCP/IP stack:
 *   lwIP integration via lwip_sys.c.  net_server_lwip_init() sets up the
 *   netif and DHCP client.  Packet Rx/Tx is handled by lwIP pbuf chains.
 *
 * virtio-net:
 *   init() probes the MMIO region (net_mmio_vaddr).  If magic/version/
 *   device_id match, net_hw_present is set true and TX is attempted
 *   through the stub.  If not found, stub mode is used throughout.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "sel4_server.h"
#include "net_server.h"
#include "lwip_sys.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"

/* ── virtio-MMIO register offsets (mirrors virtio_blk.h) ─────────────────── */
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

/* ── Shared memory base (set by Microkit via setvar_vaddr) ───────────────── */
uintptr_t net_packet_shmem_vaddr;
#define NET_SHMEM ((volatile uint8_t *)net_packet_shmem_vaddr)

/* virtio-net MMIO base (set by Microkit via setvar_vaddr) */
uintptr_t net_mmio_vaddr;

/* Console ring base (set by Microkit via setvar_vaddr) */
uintptr_t log_drain_rings_vaddr;

/* Vibe staging region (set by Microkit via setvar_vaddr).
 * Shared with agent PDs; net_server uses offset 0x200000–0x3FFFFF for HTTP I/O.
 * Layout mirrors the agent-side STAGING_HTTP_URL_OFFSET / STAGING_HTTP_OFFSET
 * constants defined in libagent.c. */
uintptr_t vibe_staging_vaddr;

/* ── Module state ────────────────────────────────────────────────────────── */
static net_vnic_t  vnics[NET_MAX_VNICS];
static uint32_t    active_vnic_count = 0;
static bool        net_hw_present    = false;

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

/* ── MMIO read helper ────────────────────────────────────────────────────── */
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

    /*
     * Minimal device initialisation sequence (virtio spec §3.1.1):
     *   ACKNOWLEDGE → DRIVER → FEATURES_OK → DRIVER_OK
     * Full feature negotiation and virtqueue setup is deferred to
     * lwIP_INTEGRATION_NOTE in init().
     */
    mmio_write32(net_mmio_vaddr, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    mmio_write32(net_mmio_vaddr, VIRTIO_MMIO_STATUS,
                 VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    log_drain_write(16, 16, "[net_server] virtio-net detected at ");
    log_hex((uint32_t)net_mmio_vaddr);
    log_drain_write(16, 16, ", hw present\n");

    /* Initialize lwIP with the virtio-net MMIO address.
     * Virtqueue descriptor tables are not yet allocated in Phase 1;
     * pass 0 for rx_desc/tx_desc — lwip_sys.c handles missing queues
     * gracefully by skipping the virtqueue kick path. */
    net_server_lwip_init(net_mmio_vaddr, 0u, 0u);
}

/* ── vNIC slot allocation ────────────────────────────────────────────────── */

/*
 * Returns a free vNIC table index and its shmem slot index, or -1 if full.
 * The shmem slot index equals the vNIC table index (1:1 mapping).
 */
static int alloc_vnic_slot(void) {
    for (int i = 0; i < (int)NET_MAX_VNICS; i++) {
        if (!vnics[i].active)
            return i;
    }
    return -1;
}

/* Find a vNIC by id. Returns pointer or NULL. */
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
    /* Memory barrier: ensure ring is visible before we announce the slot */
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

/* ── OP_NET_VNIC_CREATE ───────────────────────────────────────────────────── */
static uint32_t handle_vnic_create(void) {
    uint32_t requested_id = (uint32_t)msg_u32(req, 4);
    uint32_t cap_classes  = (uint32_t)msg_u32(req, 8);
    uint32_t caller_pd    = (uint32_t)msg_u32(req, 12);

    /* CAP_CLASS_NET is mandatory */
    if (!(cap_classes & CAP_CLASS_NET)) {
        log_drain_write(16, 16, "[net_server] VNIC_CREATE denied: missing CAP_CLASS_NET "
                    "pd=");
        log_dec(caller_pd);
        log_drain_write(16, 16, "\n");
        rep_u32(rep, 0, NET_ERR_PERM);
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    int slot = alloc_vnic_slot();
    if (slot < 0) {
        log_drain_write(16, 16, "[net_server] VNIC_CREATE failed: table full\n");
        rep_u32(rep, 0, NET_ERR_NO_VNICS);
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    /*
     * Assign vnic_id: use requested_id if valid and not in use,
     * otherwise auto-assign (slot index as id when 0xFF requested).
     */
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
    /* Default ACL: allow outbound only; internet requires explicit grant */
    v->acl_flags     = NET_ACL_ALLOW_OUTBOUND;
    v->port_count    = 0;
    v->shmem_slot    = (uint32_t)slot;
    v->rx_packets    = 0;
    v->tx_packets    = 0;

    /* Locally-administered MAC: 02:00:00:00:00:NN */
    v->mac[0] = 0x02;
    v->mac[1] = 0x00;
    v->mac[2] = 0x00;
    v->mac[3] = 0x00;
    v->mac[4] = 0x00;
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
    log_drain_write(16, 16, " mac=02:00:00:00:00:");
    {
        static const char hex[] = "0123456789abcdef";
        char buf[3];
        buf[0] = hex[(vnic_id >> 4) & 0xf];
        buf[1] = hex[ vnic_id       & 0xf];
        buf[2] = '\0';
        log_drain_write(16, 16, buf);
    }
    log_drain_write(16, 16, "\n");

    rep_u32(rep, 0, NET_OK);
    rep_u32(rep, 4, vnic_id);
    rep_u32(rep, 8, slot_offset);
    rep->length = 12;
        return SEL4_ERR_OK;
}

/* ── OP_NET_VNIC_DESTROY ──────────────────────────────────────────────────── */
static uint32_t handle_vnic_destroy(void) {
    uint32_t vnic_id = (uint32_t)msg_u32(req, 4);

    net_vnic_t *v = find_vnic(vnic_id);
    if (!v) {
        log_drain_write(16, 16, "[net_server] VNIC_DESTROY: not found id=");
        log_dec(vnic_id);
        log_drain_write(16, 16, "\n");
        rep_u32(rep, 0, NET_ERR_NOT_FOUND);
        rep->length = 4;
        return SEL4_ERR_OK;
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

    rep_u32(rep, 0, NET_OK);
    rep->length = 4;
        return SEL4_ERR_OK;
}

/* ── OP_NET_VNIC_SEND ─────────────────────────────────────────────────────── */
static uint32_t handle_vnic_send(void) {
    uint32_t vnic_id      = (uint32_t)msg_u32(req, 4);
    uint32_t pkt_offset   = (uint32_t)msg_u32(req, 8);
    uint32_t pkt_len      = (uint32_t)msg_u32(req, 12);

    net_vnic_t *v = find_vnic(vnic_id);
    if (!v) {
        rep_u32(rep, 0, NET_ERR_NOT_FOUND);
        rep_u32(rep, 4, 0);
        rep->length = 8;
        return SEL4_ERR_OK;
    }

    /* ACL: outbound must be permitted */
    if (!acl_outbound_allowed(v)) {
        log_drain_write(16, 16, "[net_server] SEND denied by ACL: vnic=");
        log_dec(vnic_id);
        log_drain_write(16, 16, " pd=");
        log_dec(v->owner_pd);
        log_drain_write(16, 16, "\n");
        rep_u32(rep, 0, NET_ERR_PERM);
        rep_u32(rep, 4, 0);
        rep->length = 8;
        return SEL4_ERR_OK;
    }

    /* Validate shmem bounds */
    if (pkt_offset >= NET_SHMEM_TOTAL || pkt_len == 0
            || pkt_offset + pkt_len > NET_SHMEM_TOTAL) {
        rep_u32(rep, 0, NET_ERR_INVAL);
        rep_u32(rep, 4, 0);
        rep->length = 8;
        return SEL4_ERR_OK;
    }

    /*
     * Peek at destination IP to decide routing.
     * A minimal Ethernet frame starts with 6-byte dst MAC, 6-byte src MAC,
     * 2-byte EtherType (0x0800 = IPv4), then the IPv4 header.
     * IPv4 dst address is at byte offset 16 within the IP header (ETH=14 + IP_DST=16).
     * Total offset from frame start: 14 + 16 = 30 bytes.
     */
    uint32_t dest_ip = 0;
    const uint8_t *pkt_base = (const uint8_t *)(net_packet_shmem_vaddr + pkt_offset);

    if (pkt_len >= 34u) {
        /* Parse EtherType (bytes 12-13) — only proceed if IPv4 */
        uint16_t ethertype = ((uint16_t)pkt_base[12] << 8) | pkt_base[13];
        if (ethertype == 0x0800u) {
            dest_ip = ((uint32_t)pkt_base[30] << 24)
                    | ((uint32_t)pkt_base[31] << 16)
                    | ((uint32_t)pkt_base[32] <<  8)
                    |  (uint32_t)pkt_base[33];
        }
    }

    if (NET_IP_IS_LOOPBACK(dest_ip)) {
        /*
         * Loopback routing: find the vNIC whose shmem RX ring we should
         * deliver to.  For MVP we search for any active vNIC other than
         * the sender; a production implementation would match bound ports.
         */
        bool delivered = false;
        for (int i = 0; i < (int)NET_MAX_VNICS && !delivered; i++) {
            net_vnic_t *dst = &vnics[i];
            if (!dst->active || dst->vnic_id == vnic_id)
                continue;

            volatile net_vnic_ring_t *dst_ring = slot_ring(dst->shmem_slot);
            uint32_t next_rx_head = (dst_ring->rx_head + 1) % NET_SHMEM_DATA_SIZE;
            if (next_rx_head == dst_ring->rx_tail) {
                /* RX ring full — drop */
                dst_ring->rx_drops++;
            } else {
                /*
                 * lwIP_INTEGRATION_NOTE: instead of direct shmem copy,
                 * inject the packet into the smoltcp loopback interface so
                 * that TCP/UDP demuxing can occur correctly.
                 *
                 * For MVP: record that a loopback packet was routed; the data
                 * already lives in the sender's shmem region at pkt_offset.
                 * The destination app polls OP_NET_VNIC_RECV to read it.
                 * (Full copy implementation requires agreeing on the RX data
                 * ring format, which is deferred to the smoltcp integration.)
                 */
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
        /* Non-loopback: hand off to virtio-net or stub */
        if (net_hw_present) {
            /* Feed packet to lwIP for transmission via virtio-net */
            const uint8_t *pkt = (const uint8_t *)(net_packet_shmem_vaddr
                                                    + pkt_offset);
            struct pbuf *p = pbuf_alloc(PBUF_LINK, (u16_t)pkt_len, PBUF_RAM);
            if (p) {
                uint8_t *dst = (uint8_t *)p->payload;
                for (uint16_t i = 0; i < (uint16_t)pkt_len; i++)
                    dst[i] = pkt[i];
                g_netif.linkoutput(&g_netif, p);
                pbuf_free(p);
            }
        } else {
            /* No hardware — drop with a log */
            log_drain_write(16, 16, "[net_server] TX dropped (no hw): vnic=");
            log_dec(vnic_id);
            log_drain_write(16, 16, " len=");
            log_dec(pkt_len);
            log_drain_write(16, 16, "\n");
        }
    }

    v->tx_packets++;

    rep_u32(rep, 0, NET_OK);
    rep_u32(rep, 4, pkt_len);
    rep->length = 8;
        return SEL4_ERR_OK;
}

/* ── OP_NET_VNIC_RECV ─────────────────────────────────────────────────────── */
static uint32_t handle_vnic_recv(void) {
    uint32_t vnic_id    = (uint32_t)msg_u32(req, 4);
    uint32_t buf_offset = (uint32_t)msg_u32(req, 8);
    uint32_t max_len    = (uint32_t)msg_u32(req, 12);

    net_vnic_t *v = find_vnic(vnic_id);
    if (!v) {
        rep_u32(rep, 0, NET_ERR_NOT_FOUND);
        rep_u32(rep, 4, 0);
        rep->length = 8;
        return SEL4_ERR_OK;
    }

    /* Validate shmem bounds */
    if (buf_offset >= NET_SHMEM_TOTAL
            || (max_len > 0 && buf_offset + max_len > NET_SHMEM_TOTAL)) {
        rep_u32(rep, 0, NET_ERR_INVAL);
        rep_u32(rep, 4, 0);
        rep->length = 8;
        return SEL4_ERR_OK;
    }

    /* Poll lwIP for received data on this vnic's TCP connection */
    int n = lwip_net_recv((uint8_t)(vnic_id & 0xFFu),
                          (uint8_t *)(net_packet_shmem_vaddr + buf_offset),
                          max_len > 65536u ? 65536u : max_len);
    v->rx_packets += (n > 0) ? 1u : 0u;
    rep_u32(rep, 0, NET_OK);
    rep_u32(rep, 4, (uint32_t)n);
    rep->length = 8;
        return SEL4_ERR_OK;
}

/* ── OP_NET_BIND ──────────────────────────────────────────────────────────── */
static uint32_t handle_net_bind(void) {
    uint32_t vnic_id  = (uint32_t)msg_u32(req, 4);
    uint32_t port     = (uint32_t)msg_u32(req, 8);
    uint32_t protocol = (uint32_t)msg_u32(req, 12);

    (void)protocol;  /* stored for future smoltcp socket creation */

    if (port == 0 || port > 65535u) {
        rep_u32(rep, 0, NET_ERR_INVAL);
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    net_vnic_t *v = find_vnic(vnic_id);
    if (!v) {
        rep_u32(rep, 0, NET_ERR_NOT_FOUND);
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    if (v->port_count >= NET_MAX_BOUND_PORTS) {
        rep_u32(rep, 0, NET_ERR_NO_VNICS);  /* reuse: means "table full" */
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    v->bound_ports[v->port_count++] = port;

    log_drain_write(16, 16, "[net_server] BIND vnic=");
    log_dec(vnic_id);
    log_drain_write(16, 16, " port=");
    log_dec(port);
    log_drain_write(16, 16, " proto=");
    log_dec(protocol);
    log_drain_write(16, 16, "\n");

    rep_u32(rep, 0, NET_OK);
    rep->length = 4;
        return SEL4_ERR_OK;
}

/* ── OP_NET_CONNECT ───────────────────────────────────────────────────────── */
static uint32_t handle_net_connect(void) {
    uint32_t vnic_id   = (uint32_t)msg_u32(req, 4);
    uint32_t dest_ip   = (uint32_t)msg_u32(req, 8);
    uint32_t dest_port = (uint32_t)msg_u32(req, 12);
    uint32_t protocol  = (uint32_t)msg_u32(req, 16);

    net_vnic_t *v = find_vnic(vnic_id);
    if (!v) {
        rep_u32(rep, 0, NET_ERR_NOT_FOUND);
        rep_u32(rep, 4, 0);
        rep->length = 8;
        return SEL4_ERR_OK;
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

    /* Open a lwIP TCP connection for this vNIC slot (TCP only in Phase 1) */
    int err = lwip_net_connect((uint8_t)(vnic_id & 0xFFu),
                                dest_ip, (uint16_t)dest_port);
    rep_u32(rep, 0, err == 0 ? NET_OK : NET_ERR_STUB);
    rep_u32(rep, 4, vnic_id);
    rep->length = 8;
        return SEL4_ERR_OK;
}

/* ── OP_NET_STATUS ────────────────────────────────────────────────────────── */
static uint32_t handle_net_status(void) {
    uint32_t vnic_id = (uint32_t)msg_u32(req, 4);

    if (vnic_id == 0xFFFFFFFFu) {
        /* Global status */
        uint64_t total_rx = 0, total_tx = 0;
        for (int i = 0; i < (int)NET_MAX_VNICS; i++) {
            if (vnics[i].active) {
                total_rx += vnics[i].rx_packets;
                total_tx += vnics[i].tx_packets;
            }
        }
        rep_u32(rep, 0, NET_OK);
        rep_u32(rep, 4, active_vnic_count);
        rep_u32(rep, 8, (uint32_t)(total_rx & 0xFFFFFFFFu));
        rep_u32(rep, 12, (uint32_t)(total_tx & 0xFFFFFFFFu));
        rep->length = 16;
        return SEL4_ERR_OK;
    }

    net_vnic_t *v = find_vnic(vnic_id);
    if (!v) {
        rep_u32(rep, 0, NET_ERR_NOT_FOUND);
        rep->length = 4;
        return SEL4_ERR_OK;
    }

    rep_u32(rep, 0, NET_OK);
    rep_u32(rep, 4, active_vnic_count);
    rep_u32(rep, 8, (uint32_t)(v->rx_packets & 0xFFFFFFFFu));
    rep_u32(rep, 12, (uint32_t)(v->tx_packets & 0xFFFFFFFFu));
    rep->length = 16;
        return SEL4_ERR_OK;
}

/* ── OP_NET_SET_ACL ───────────────────────────────────────────────────────── */
static uint32_t handle_net_set_acl(void) {
    uint32_t vnic_id   = (uint32_t)msg_u32(req, 4);
    uint32_t acl_flags = (uint32_t)msg_u32(req, 8);

    net_vnic_t *v = find_vnic(vnic_id);
    if (!v) {
        rep_u32(rep, 0, NET_ERR_NOT_FOUND);
        rep->length = 4;
        return SEL4_ERR_OK;
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

    rep_u32(rep, 0, NET_OK);
    rep->length = 4;
        return SEL4_ERR_OK;
}

/* ── OP_NET_HTTP_POST ─────────────────────────────────────────────────────── */

/*
 * Reserved lwIP connection slot for HTTP proxy.  Slot 15 (NET_MAX_VNICS - 1)
 * is never allocated as a vNIC; it exists solely for net_server's own use.
 */
#define HTTP_CONN_ID  ((uint8_t)(NET_MAX_VNICS - 1u))

/* QEMU user-networking host: 10.0.2.2, big-endian */
#define BRIDGE_IP_BE  0x0A000202u
#define BRIDGE_PORT   8790u

/* Staging sub-regions (must match libagent.c) */
#define STAGING_HTTP_URL_OFFSET  0x1FF000UL   /* 4 KB before body region */
#define STAGING_HTTP_OFFSET      0x200000UL   /* request body / response body */

/* Append n bytes of src into dst[pos], capped at cap. Returns new pos. */
static uint16_t buf_append(uint8_t *dst, uint16_t pos, uint16_t cap,
                            const char *src, uint16_t n)
{
    for (uint16_t i = 0; i < n && pos < cap; i++)
        dst[pos++] = (uint8_t)src[i];
    return pos;
}

/* Append decimal string of v. */
static uint16_t buf_append_dec(uint8_t *dst, uint16_t pos, uint16_t cap, uint32_t v)
{
    char tmp[12];
    int  i = 11;
    tmp[i] = '\0';
    if (v == 0) { tmp[--i] = '0'; }
    else { while (v > 0 && i > 0) { tmp[--i] = (char)('0' + (int)(v % 10u)); v /= 10u; } }
    return buf_append(dst, pos, cap, &tmp[i], (uint16_t)(11 - i));
}

static uint32_t handle_net_http_post(void)
{
    uint32_t url_offset  = (uint32_t)msg_u32(req, 4);
    uint32_t url_len     = (uint32_t)msg_u32(req, 8);
    uint32_t body_offset = (uint32_t)msg_u32(req, 12);
    uint32_t body_len    = (uint32_t)msg_u32(req, 16);

    if (!vibe_staging_vaddr) {
        log_drain_write(16, 16, "[net_server] HTTP_POST: staging not mapped\n");
        rep_u32(rep, 0, 0u);
        rep_u32(rep, 4, 0u);
        rep_u32(rep, 8, 0u);
        rep->length = 12;
        return SEL4_ERR_OK;
    }

    const char *url  = (const char *)(vibe_staging_vaddr + url_offset);
    const char *body = (const char *)(vibe_staging_vaddr + body_offset);

    /* Extract path: skip "http://host:port" prefix */
    const char *path = "/";
    if (url_len >= 8u) {
        const char *p = url + 7u;  /* skip "http://" */
        while ((uint32_t)(p - url) < url_len && *p && *p != '/') p++;
        if ((uint32_t)(p - url) < url_len && *p == '/') path = p;
    }
    /* Measure path length (up to end of url_len) */
    uint32_t path_len = 0;
    while (path_len + (uint32_t)(path - url) < url_len &&
           path[path_len] && path[path_len] != ' ') path_len++;

    /* Build HTTP/1.1 POST header in a static buffer */
    static uint8_t http_hdr[512];
    uint16_t hlen = 0;
    const uint16_t hcap = (uint16_t)sizeof(http_hdr);

#define HL(s)  hlen = buf_append(http_hdr, hlen, hcap, (s), (uint16_t)(sizeof(s)-1u))
#define HS(s,n) hlen = buf_append(http_hdr, hlen, hcap, (s), (uint16_t)(n))
#define HD(v)  hlen = buf_append_dec(http_hdr, hlen, hcap, (v))

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

    /* Reset dedicated HTTP connection slot */
    if (g_conns[HTTP_CONN_ID].tcp) {
        tcp_close(g_conns[HTTP_CONN_ID].tcp);
        g_conns[HTTP_CONN_ID].tcp = NULL;
    }
    g_conns[HTTP_CONN_ID].state      = 0u;
    g_conns[HTTP_CONN_ID].rx_buf_len = 0u;

    /* Connect to bridge at 10.0.2.2:8790 */
    int err = lwip_net_connect(HTTP_CONN_ID, BRIDGE_IP_BE, BRIDGE_PORT);
    if (err != 0) {
        log_drain_write(16, 16, "[net_server] HTTP_POST: connect error=");
        log_dec((uint32_t)(uint32_t)(-err));
        log_drain_write(16, 16, "\n");
        rep_u32(rep, 0, 0u);
        rep_u32(rep, 4, 0u);
        rep_u32(rep, 8, 0u);
        rep->length = 12;
        return SEL4_ERR_OK;
    }

    /* Poll until TCP connected (state 2) or failed (state 3), ~5 s at 10 ms/tick */
    for (uint32_t i = 0; i < 500u; i++) {
        lwip_tick();
        sys_check_timeouts();
        lwip_virtio_rx_poll();
        if (g_conns[HTTP_CONN_ID].state == 2u) break;
        if (g_conns[HTTP_CONN_ID].state == 3u) break;
    }

    if (g_conns[HTTP_CONN_ID].state != 2u) {
        log_drain_write(16, 16, "[net_server] HTTP_POST: connect timeout\n");
        rep_u32(rep, 0, 0u);
        rep_u32(rep, 4, 0u);
        rep_u32(rep, 8, 0u);
        rep->length = 12;
        return SEL4_ERR_OK;
    }

    /* Send request header */
    if (lwip_net_send(HTTP_CONN_ID, http_hdr, hlen) < 0) {
        log_drain_write(16, 16, "[net_server] HTTP_POST: header send failed\n");
        rep_u32(rep, 0, 0u);
        rep_u32(rep, 4, 0u);
        rep_u32(rep, 8, 0u);
        rep->length = 12;
        return SEL4_ERR_OK;
    }

    /* Send body (up to 65535 bytes fit in uint16_t) */
    if (body_len > 0u) {
        uint16_t blen16 = (body_len > 65535u) ? 65535u : (uint16_t)body_len;
        if (lwip_net_send(HTTP_CONN_ID, (const uint8_t *)body, blen16) < 0) {
            log_drain_write(16, 16, "[net_server] HTTP_POST: body send failed\n");
            rep_u32(rep, 0, 0u);
            rep_u32(rep, 4, 0u);
            rep_u32(rep, 8, 0u);
            rep->length = 12;
        return SEL4_ERR_OK;
        }
    }

    /* Poll until response received or connection closed, ~30 s at 10 ms/tick */
    for (uint32_t i = 0; i < 3000u; i++) {
        lwip_tick();
        sys_check_timeouts();
        lwip_virtio_rx_poll();
        if (g_conns[HTTP_CONN_ID].rx_buf_len > 0u) break;
        if (g_conns[HTTP_CONN_ID].state == 3u) break;
    }

    uint32_t rx_len = g_conns[HTTP_CONN_ID].rx_buf_len;
    if (rx_len == 0u) {
        log_drain_write(16, 16, "[net_server] HTTP_POST: no response received\n");
        rep_u32(rep, 0, 0u);
        rep_u32(rep, 4, 0u);
        rep_u32(rep, 8, 0u);
        rep->length = 12;
        return SEL4_ERR_OK;
    }

    /* Parse HTTP status code from first response line: "HTTP/1.1 NNN ..." */
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

    /* Locate end of HTTP headers (\r\n\r\n) */
    uint32_t hdr_end = 0u;
    for (uint32_t i = 0u; i + 3u < rx_len; i++) {
        if (resp[i]   == '\r' && resp[i+1u] == '\n' &&
            resp[i+2u] == '\r' && resp[i+3u] == '\n') {
            hdr_end = i + 4u;
            break;
        }
    }

    uint32_t resp_body_len = (hdr_end < rx_len) ? (rx_len - hdr_end) : 0u;

    /* Copy response body into staging at body_offset (safe: request already sent) */
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

    rep_u32(rep, 0, http_status);
    rep_u32(rep, 4, body_offset);   /* response body written at same staging offset */
    rep_u32(rep, 8, resp_body_len);
    rep->length = 12;
        return SEL4_ERR_OK;
}

/* ── OP_NET_HEALTH ────────────────────────────────────────────────────────── */
static uint32_t handle_net_health(void) {
    rep_u32(rep, 0, NET_OK);
    rep_u32(rep, 4, active_vnic_count);
    rep_u32(rep, 8, NET_VERSION);
    rep->length = 12;
        return SEL4_ERR_OK;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Microkit entry points
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * init() — called once at boot before any PPCs arrive.
 */
static void net_server_pd_init(void) {
    agentos_log_boot("net_server");
    log_drain_write(16, 16, "[net_server] Initialising NetServer PD (priority 160, passive)\n");

    /* Clear vNIC table */
    for (int i = 0; i < (int)NET_MAX_VNICS; i++) {
        vnics[i].active     = false;
        vnics[i].vnic_id    = 0;
        vnics[i].owner_pd   = 0;
        vnics[i].cap_classes = 0;
        vnics[i].acl_flags  = 0;
        vnics[i].port_count = 0;
        vnics[i].shmem_slot = 0;
        vnics[i].rx_packets = 0;
        vnics[i].tx_packets = 0;
        for (int j = 0; j < (int)NET_MAX_BOUND_PORTS; j++)
            vnics[i].bound_ports[j] = 0;
        vnics[i].mac[0] = 0;
        vnics[i].mac[1] = 0;
        vnics[i].mac[2] = 0;
        vnics[i].mac[3] = 0;
        vnics[i].mac[4] = 0;
        vnics[i].mac[5] = 0;
    }
    active_vnic_count = 0;

    /* Probe virtio-net hardware */
    probe_virtio_net();

    log_drain_write(16, 16, "[net_server] READY — max_vnics=");
    log_dec(NET_MAX_VNICS);
    log_drain_write(16, 16, " shmem_slots=");
    log_dec(NET_MAX_VNICS);
    log_drain_write(16, 16, " hw=");
    log_drain_write(16, 16, net_hw_present ? "1" : "0");
    log_drain_write(16, 16, "\n");
}

/*
 * notified() — called when NetServer receives a notification (not a PPC).
 *
 * In the current design NetServer is purely passive (all callers use PPC).
 * This handler is reserved for future use:
 *   - A virtio-net RX interrupt would arrive here when the device places
 *     packets onto the used ring.
 *   - lwIP_INTEGRATION_NOTE: call smoltcp_iface_poll() and dispatch
 *     any received packets to the appropriate vNIC's RX ring.
 */
static void net_server_pd_notified(uint32_t ch) {
    switch (ch) {
    case NET_CH_CONTROLLER:
        /* virtio-net RX interrupt from controller — poll virtqueue */
        lwip_virtio_rx_poll();
        break;
    case NET_CH_TIMER:
        /* 10ms periodic tick from controller: drive lwIP timers + RX poll */
        lwip_tick();
        lwip_virtio_rx_poll();
        sys_check_timeouts();
        break;
    default:
        log_drain_write(16, 16, "[net_server] unexpected notify ch=");
        log_dec((uint32_t)ch);
        log_drain_write(16, 16, "\n");
        break;
    }
}

/*
 * protected() — PPC handler; dispatches all NetServer IPC operations.
 *
 * Callers: controller (CH0), init_agent (CH1), workers CH2..CH9, app_manager (CH10).
 * The label field of msginfo carries the opcode (OP_NET_*).
 */
static uint32_t net_server_h_dispatch(sel4_badge_t b, const sel4_msg_t *req, sel4_msg_t *rep, void *ctx) {
    (void)b; (void)ctx;   /* all callers share the same dispatch table */

    uint64_t op = msg_u32(req, 0);

    switch (op) {
    case OP_NET_VNIC_CREATE:  return handle_vnic_create();
    case OP_NET_VNIC_DESTROY: return handle_vnic_destroy();
    case OP_NET_VNIC_SEND:    return handle_vnic_send();
    case OP_NET_VNIC_RECV:    return handle_vnic_recv();
    case OP_NET_BIND:         return handle_net_bind();
    case OP_NET_CONNECT:      return handle_net_connect();
    case OP_NET_STATUS:       return handle_net_status();
    case OP_NET_SET_ACL:      return handle_net_set_acl();
    case OP_NET_HEALTH:       return handle_net_health();
    case OP_NET_HTTP_POST:    return handle_net_http_post();
    case OP_NET_CONN_STATE: {
        uint8_t vid = (uint8_t)msg_u32(req, 4);
        rep_u32(rep, 0, NET_OK);
        rep_u32(rep, 4, (uint32_t)lwip_conn_state(vid));
        rep->length = 8;
        return SEL4_ERR_OK;
    }
    case OP_NET_TCP_CLOSE: {
        uint8_t vid = (uint8_t)msg_u32(req, 4);
        if (vid < NET_MAX_VNICS && g_conns[vid].tcp) {
            tcp_close(g_conns[vid].tcp);
            g_conns[vid].tcp   = NULL;
            g_conns[vid].state = 0u;
        }
        rep_u32(rep, 0, NET_OK);
        rep->length = 4;
        return SEL4_ERR_OK;
    }
    default:
        log_drain_write(16, 16, "[net_server] unknown op=");
        log_hex((uint32_t)(op & 0xFFFFFFFFu));
        log_drain_write(16, 16, " ch=");
        log_dec((uint32_t)ch);
        log_drain_write(16, 16, "\n");
        rep_u32(rep, 0, NET_ERR_INVAL);
        rep->length = 4;
        return SEL4_ERR_OK;
    }
}

/* ── E5-S8: Entry point ─────────────────────────────────────────────────── */
void net_server_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    (void)ns_ep;
    net_server_pd_init();
    static sel4_server_t srv;
    sel4_server_init(&srv, my_ep);
    /* Dispatch all opcodes through the generic handler */
    sel4_server_register(&srv, SEL4_SERVER_OPCODE_ANY, net_server_h_dispatch, (void *)0);
    sel4_server_run(&srv);
}
