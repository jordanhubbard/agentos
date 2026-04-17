/*
 * agentOS Network Device Protection Domain
 *
 * OS-neutral network service PD that moves lwIP behind a proper PD boundary.
 * Previously, lwIP was embedded directly in net_server.c.  net_pd owns the
 * virtio-net hardware exclusively via seL4 device frame capabilities.
 *
 * API surface (see net_contract.h):
 *   Packet I/O:  MSG_NET_OPEN/CLOSE/SEND/RECV/DEV_STATUS/CONFIGURE/
 *                MSG_NET_FILTER_ADD/FILTER_REMOVE
 *   Sockets:     MSG_NET_SOCKET_OPEN/CLOSE/CONNECT/BIND/LISTEN/ACCEPT/SET_OPT
 *
 * Invariants:
 *   - Every client opens a handle via MSG_NET_OPEN or MSG_NET_SOCKET_OPEN.
 *   - NIC handles get a dedicated shmem slot (ring header + frame data).
 *   - Socket handles share the same shmem space; each gets a 16KB slot.
 *   - lwIP runs internally; callers never touch it directly.
 *   - WireGuard integration is marked WG_INTEGRATION_POINT.
 *   - Guest OSes get a handle via device binding, not direct lwIP access.
 *   - Multiple virtual NICs per guest slot are supported via the handle table.
 *
 * Channel: CH_NET_PD (68) — controller PPCs in
 * Priority: 160 (passive)
 * Shmem: net_pd_shmem (256KB at 0x5000000, rw)
 * MMIO: virtio_net_mmio (0x6000000, rw, uncached)
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "contracts/net_contract.h"
#include "lwip_sys.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"

/* ─── virtio-MMIO register offsets ──────────────────────────────────────────── */
#define VIRTIO_MMIO_MAGIC_VALUE  0x000u
#define VIRTIO_MMIO_VERSION      0x004u
#define VIRTIO_MMIO_DEVICE_ID    0x008u
#define VIRTIO_MMIO_STATUS       0x070u
#define VIRTIO_MMIO_MAGIC        0x74726976u  /* "virt" */
#define VIRTIO_STATUS_ACKNOWLEDGE (1u << 0)
#define VIRTIO_STATUS_DRIVER      (1u << 1)
#define VIRTIO_NET_DEVICE_ID     1u

/* ─── Shared memory ─────────────────────────────────────────────────────────── */
uintptr_t net_pd_shmem_vaddr;
uintptr_t net_pd_mmio_vaddr;
uintptr_t log_drain_rings_vaddr;

#define NET_PD_SHMEM ((volatile uint8_t *)net_pd_shmem_vaddr)

/* ─── Shmem layout (mirrors net_server.h geometry) ─────────────────────────── */
#define NETPD_SHMEM_TOTAL      0x40000u   /* 256KB */
#define NETPD_SLOT_SIZE        0x4000u    /* 16KB per handle slot */
#define NETPD_SLOT_HDR_SIZE    1024u      /* ring header within slot */
#define NETPD_SLOT_DATA_SIZE   (NETPD_SLOT_SIZE - NETPD_SLOT_HDR_SIZE)
#define NETPD_SLOT_OFFSET(n)   ((n) * NETPD_SLOT_SIZE)

/* ─── Handle type discriminator ─────────────────────────────────────────────── */
#define HANDLE_TYPE_INVALID  0u
#define HANDLE_TYPE_NIC      1u   /* MSG_NET_OPEN: raw Ethernet frame client */
#define HANDLE_TYPE_SOCKET   2u   /* MSG_NET_SOCKET_OPEN: TCP/UDP socket client */

/* ─── Socket states ──────────────────────────────────────────────────────────── */
#define SOCK_STATE_FREE        0u
#define SOCK_STATE_BOUND       1u
#define SOCK_STATE_CONNECTING  2u
#define SOCK_STATE_CONNECTED   3u
#define SOCK_STATE_LISTENING   4u
#define SOCK_STATE_CLOSED      5u

/* ─── Socket options (MSG_NET_SOCKET_SET_OPT option IDs) ────────────────────── */
#define NET_SOCKOPT_REUSEADDR  0u
#define NET_SOCKOPT_NODELAY    1u
#define NET_SOCKOPT_KEEPALIVE  2u

/* ─── NIC ring header at slot offset 0 ─────────────────────────────────────── */
#define NETPD_RING_MAGIC  0x4E455450u   /* "NETP" */

typedef struct {
    uint32_t magic;
    uint32_t handle;
    uint32_t tx_head;
    uint32_t tx_tail;
    uint32_t rx_head;
    uint32_t rx_tail;
    uint32_t tx_drops;
    uint32_t rx_drops;
} netpd_ring_t;

/* ─── Per-client handle entry ───────────────────────────────────────────────── */
typedef struct {
    bool     active;
    uint32_t type;         /* HANDLE_TYPE_NIC or HANDLE_TYPE_SOCKET */
    uint32_t shmem_slot;   /* index 0-15 into net_pd_shmem */

    /* NIC handle state */
    uint32_t iface_id;
    uint32_t mtu;
    uint32_t cfg_flags;    /* NET_CFG_FLAG_* bitmask */
    uint64_t rx_pkts;
    uint64_t tx_pkts;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint32_t rx_errors;
    uint32_t tx_errors;
    uint8_t  mac[6];
    uint8_t  _mac_pad[2];
    uint32_t filter_count;
    uint32_t next_filter_id;
    /* filter_id[n] pairs with filter in shmem (recorded for removal) */
    uint32_t filter_ids[NET_MAX_FILTERS];
    uint32_t filter_lens[NET_MAX_FILTERS];   /* byte lengths of installed rules */

    /* Socket handle state (valid when type == HANDLE_TYPE_SOCKET) */
    uint8_t  proto;          /* NET_PROTO_TCP or NET_PROTO_UDP */
    uint8_t  sock_state;     /* SOCK_STATE_* */
    uint32_t local_port;
    uint32_t remote_ip;
    uint32_t remote_port;
    /* lwIP TCP PCB is tracked in lwip_sys g_conns[shmem_slot] */
} net_pd_client_t;

/* ─── Module state ──────────────────────────────────────────────────────────── */
static net_pd_client_t clients[NET_MAX_CLIENTS];
static uint32_t        active_clients = 0;
static uint32_t        slot_bitmap    = 0u;   /* bit N set = shmem slot N in use */
static bool            hw_present     = false;

/* ─── Module-level iface counters (link-level) ──────────────────────────────── */
static uint64_t iface_rx_pkts = 0, iface_tx_pkts = 0;
static uint64_t iface_rx_bytes = 0, iface_tx_bytes = 0;
static uint32_t iface_rx_errors = 0, iface_tx_errors = 0;
static bool     iface_link_up = false;

/* ─── Minimal string/number log helpers (no libc) ──────────────────────────── */

static void log_dec(uint32_t v)
{
    if (v == 0) { log_drain_write(17, 17, "0"); return; }
    char buf[12];
    int  i = 11;
    buf[i] = '\0';
    while (v > 0 && i > 0) { buf[--i] = '0' + (char)(v % 10u); v /= 10u; }
    log_drain_write(17, 17, &buf[i]);
}

static void log_hex(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";
    char buf[11];
    buf[0]  = '0'; buf[1]  = 'x';
    buf[2]  = hex[(v >> 28) & 0xf]; buf[3]  = hex[(v >> 24) & 0xf];
    buf[4]  = hex[(v >> 20) & 0xf]; buf[5]  = hex[(v >> 16) & 0xf];
    buf[6]  = hex[(v >> 12) & 0xf]; buf[7]  = hex[(v >>  8) & 0xf];
    buf[8]  = hex[(v >>  4) & 0xf]; buf[9]  = hex[ v        & 0xf];
    buf[10] = '\0';
    log_drain_write(17, 17, buf);
}

/* ─── MMIO helpers ─────────────────────────────────────────────────────────── */

static inline uint32_t mmio_read32(uintptr_t base, uint32_t off)
{
    return *(volatile uint32_t *)(base + off);
}

static inline void mmio_write32(uintptr_t base, uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(base + off) = val;
}

/* ─── Shmem slot allocation ─────────────────────────────────────────────────── */

static int alloc_slot(void)
{
    for (int i = 0; i < (int)NET_MAX_CLIENTS; i++) {
        if (!(slot_bitmap & (1u << i))) {
            slot_bitmap |= (1u << i);
            return i;
        }
    }
    return -1;
}

static void free_slot(uint32_t slot)
{
    if (slot < NET_MAX_CLIENTS)
        slot_bitmap &= ~(1u << slot);
}

/* ─── Handle lookup helpers ─────────────────────────────────────────────────── */

static net_pd_client_t *find_client(uint32_t handle)
{
    if (handle >= NET_MAX_CLIENTS) return NULL;
    net_pd_client_t *c = &clients[handle];
    return (c->active) ? c : NULL;
}

static net_pd_client_t *find_nic(uint32_t handle)
{
    net_pd_client_t *c = find_client(handle);
    return (c && c->type == HANDLE_TYPE_NIC) ? c : NULL;
}

static net_pd_client_t *find_sock(uint32_t handle)
{
    net_pd_client_t *c = find_client(handle);
    return (c && c->type == HANDLE_TYPE_SOCKET) ? c : NULL;
}

/* ─── Ring accessor ─────────────────────────────────────────────────────────── */

static volatile netpd_ring_t *slot_ring(uint32_t slot)
{
    return (volatile netpd_ring_t *)(net_pd_shmem_vaddr + NETPD_SLOT_OFFSET(slot));
}

static void init_ring(uint32_t slot, uint32_t handle)
{
    volatile netpd_ring_t *r = slot_ring(slot);
    r->magic    = NETPD_RING_MAGIC;
    r->handle   = handle;
    r->tx_head  = 0; r->tx_tail = 0;
    r->rx_head  = 0; r->rx_tail = 0;
    r->tx_drops = 0; r->rx_drops = 0;
    __asm__ volatile("" ::: "memory");
}

static void clear_ring(uint32_t slot)
{
    volatile netpd_ring_t *r = slot_ring(slot);
    r->magic = 0;
    __asm__ volatile("" ::: "memory");
}

/* ─── virtio-net probe ─────────────────────────────────────────────────────── */

static void probe_virtio_net(void)
{
    if (!net_pd_mmio_vaddr) {
        log_drain_write(17, 17, "[net_pd] virtio-net: MMIO not mapped, stub mode\n");
        return;
    }

    uint32_t magic     = mmio_read32(net_pd_mmio_vaddr, VIRTIO_MMIO_MAGIC_VALUE);
    uint32_t version   = mmio_read32(net_pd_mmio_vaddr, VIRTIO_MMIO_VERSION);
    uint32_t device_id = mmio_read32(net_pd_mmio_vaddr, VIRTIO_MMIO_DEVICE_ID);

    if (magic != VIRTIO_MMIO_MAGIC || version != 2u
            || device_id != VIRTIO_NET_DEVICE_ID) {
        log_drain_write(17, 17, "[net_pd] virtio-net not detected (magic=");
        log_hex(magic);
        log_drain_write(17, 17, "), stub mode\n");
        return;
    }

    hw_present     = true;
    iface_link_up  = true;

    mmio_write32(net_pd_mmio_vaddr, VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    mmio_write32(net_pd_mmio_vaddr, VIRTIO_MMIO_STATUS,
                 VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    log_drain_write(17, 17, "[net_pd] virtio-net detected at ");
    log_hex((uint32_t)net_pd_mmio_vaddr);
    log_drain_write(17, 17, ", hw present\n");

    /*
     * Initialise lwIP against this virtio-net MMIO base.
     * Virtqueue descriptor tables not yet allocated; pass 0 — lwip_sys.c
     * handles missing queues by skipping the virtqueue kick path.
     */
    net_server_lwip_init(net_pd_mmio_vaddr, 0u, 0u);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MSG_NET_OPEN (0x2101) — open a raw-Ethernet NIC handle
 *
 *   MR1 = iface_id  (0 = primary interface)
 *   Reply: MR0=ok, MR1=handle, MR2=shmem_slot_offset, MR3..MR8=mac[0..5]
 * ═══════════════════════════════════════════════════════════════════════════ */
static microkit_msginfo handle_net_open(void)
{
    uint32_t iface_id = (uint32_t)microkit_mr_get(1);

    if (active_clients >= NET_MAX_CLIENTS) {
        microkit_mr_set(0, NET_ERR_NO_SLOTS);
        return microkit_msginfo_new(0, 1);
    }

    int slot = alloc_slot();
    if (slot < 0) {
        microkit_mr_set(0, NET_ERR_NO_SLOTS);
        return microkit_msginfo_new(0, 1);
    }

    /* Find an unused handle index */
    int handle = -1;
    for (int i = 0; i < (int)NET_MAX_CLIENTS; i++) {
        if (!clients[i].active) { handle = i; break; }
    }
    if (handle < 0) {
        free_slot((uint32_t)slot);
        microkit_mr_set(0, NET_ERR_NO_SLOTS);
        return microkit_msginfo_new(0, 1);
    }

    net_pd_client_t *c = &clients[handle];
    c->active        = true;
    c->type          = HANDLE_TYPE_NIC;
    c->shmem_slot    = (uint32_t)slot;
    c->iface_id      = iface_id;
    c->mtu           = 1500u;
    c->cfg_flags     = 0u;
    c->rx_pkts       = 0; c->tx_pkts = 0;
    c->rx_bytes      = 0; c->tx_bytes = 0;
    c->rx_errors     = 0; c->tx_errors = 0;
    c->filter_count  = 0; c->next_filter_id = 1u;
    for (uint32_t i = 0; i < NET_MAX_FILTERS; i++) {
        c->filter_ids[i]  = 0;
        c->filter_lens[i] = 0;
    }

    /* Locally-administered MAC: 02:00:00:00:00:NN (handle index) */
    c->mac[0] = 0x02; c->mac[1] = 0x00; c->mac[2] = 0x00;
    c->mac[3] = 0x00; c->mac[4] = 0x00; c->mac[5] = (uint8_t)(handle & 0xFFu);

    init_ring((uint32_t)slot, (uint32_t)handle);
    active_clients++;

    uint32_t slot_off = NETPD_SLOT_OFFSET((uint32_t)slot);

    log_drain_write(17, 17, "[net_pd] OPEN: handle=");
    log_dec((uint32_t)handle);
    log_drain_write(17, 17, " iface=");
    log_dec(iface_id);
    log_drain_write(17, 17, " slot=");
    log_dec((uint32_t)slot);
    log_drain_write(17, 17, "\n");

    microkit_mr_set(0, NET_OK);
    microkit_mr_set(1, (uint32_t)handle);
    microkit_mr_set(2, slot_off);
    /* Return MAC address in MR3..MR8 */
    microkit_mr_set(3, c->mac[0]);
    microkit_mr_set(4, c->mac[1]);
    microkit_mr_set(5, c->mac[2]);
    microkit_mr_set(6, c->mac[3]);
    microkit_mr_set(7, c->mac[4]);
    microkit_mr_set(8, c->mac[5]);
    return microkit_msginfo_new(0, 9);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MSG_NET_CLOSE (0x2102) — close a NIC handle
 * ═══════════════════════════════════════════════════════════════════════════ */
static microkit_msginfo handle_net_close(void)
{
    uint32_t handle = (uint32_t)microkit_mr_get(1);

    net_pd_client_t *c = find_nic(handle);
    if (!c) {
        microkit_mr_set(0, NET_ERR_BAD_HANDLE);
        return microkit_msginfo_new(0, 1);
    }

    log_drain_write(17, 17, "[net_pd] CLOSE: handle=");
    log_dec(handle);
    log_drain_write(17, 17, "\n");

    clear_ring(c->shmem_slot);
    free_slot(c->shmem_slot);
    c->active = false;
    if (active_clients > 0) active_clients--;

    microkit_mr_set(0, NET_OK);
    return microkit_msginfo_new(0, 1);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MSG_NET_SEND (0x2103) — transmit an Ethernet frame (NIC handle)
 *   OR send data bytes (socket handle — dispatched below)
 *
 *   NIC: MR1=handle, MR2=len (frame in shmem at slot_offset)
 *   Reply: MR0=ok
 * ═══════════════════════════════════════════════════════════════════════════ */
static microkit_msginfo handle_net_send_nic(net_pd_client_t *c, uint32_t handle)
{
    uint32_t frame_len = (uint32_t)microkit_mr_get(2);

    if (frame_len == 0 || frame_len > NET_MAX_FRAME_BYTES) {
        c->tx_errors++;
        microkit_mr_set(0, NET_ERR_FRAME_TOO_LARGE);
        return microkit_msginfo_new(0, 1);
    }

    uint32_t slot_off = NETPD_SLOT_OFFSET(c->shmem_slot) + NETPD_SLOT_HDR_SIZE;
    if (slot_off + frame_len > NETPD_SHMEM_TOTAL) {
        c->tx_errors++;
        microkit_mr_set(0, NET_ERR_FRAME_TOO_LARGE);
        return microkit_msginfo_new(0, 1);
    }

    const uint8_t *frame = (const uint8_t *)(net_pd_shmem_vaddr + slot_off);

    if (hw_present) {
        struct pbuf *p = pbuf_alloc(PBUF_LINK, (u16_t)frame_len, PBUF_RAM);
        if (p) {
            uint8_t *dst = (uint8_t *)p->payload;
            for (uint16_t i = 0; i < (uint16_t)frame_len; i++) dst[i] = frame[i];
            g_netif.linkoutput(&g_netif, p);
            pbuf_free(p);
        } else {
            c->tx_errors++;
        }
    } else {
        /* WG_INTEGRATION_POINT: if WireGuard is active for this handle,
         * encrypt the frame via wg_encrypt() and send over the WG tunnel
         * instead of dropping.  Wire up by calling wg_net_send() here. */
        log_drain_write(17, 17, "[net_pd] TX stub (no hw): handle=");
        log_dec(handle);
        log_drain_write(17, 17, " len=");
        log_dec(frame_len);
        log_drain_write(17, 17, "\n");
    }

    c->tx_pkts++;
    c->tx_bytes += frame_len;
    iface_tx_pkts++;
    iface_tx_bytes += frame_len;

    microkit_mr_set(0, NET_OK);
    return microkit_msginfo_new(0, 1);
}

static microkit_msginfo handle_net_send_sock(net_pd_client_t *c, uint32_t handle)
{
    uint32_t data_len = (uint32_t)microkit_mr_get(2);

    if (data_len == 0) {
        microkit_mr_set(0, NET_OK);
        return microkit_msginfo_new(0, 1);
    }

    uint32_t slot_off = NETPD_SLOT_OFFSET(c->shmem_slot) + NETPD_SLOT_HDR_SIZE;
    const uint8_t *data = (const uint8_t *)(net_pd_shmem_vaddr + slot_off);

    if (c->sock_state != SOCK_STATE_CONNECTED) {
        microkit_mr_set(0, NET_ERR_BAD_HANDLE);
        return microkit_msginfo_new(0, 1);
    }

    uint16_t len16 = (data_len > 65535u) ? 65535u : (uint16_t)data_len;
    int sent = lwip_net_send((uint8_t)(c->shmem_slot & 0xFFu), data, len16);

    if (sent < 0) {
        c->tx_errors++;
        microkit_mr_set(0, NET_ERR_BAD_HANDLE);
        return microkit_msginfo_new(0, 1);
    }

    c->tx_pkts++;
    c->tx_bytes += (uint32_t)sent;

    log_drain_write(17, 17, "[net_pd] SOCK_SEND: handle=");
    log_dec(handle);
    log_drain_write(17, 17, " sent=");
    log_dec((uint32_t)sent);
    log_drain_write(17, 17, "\n");

    microkit_mr_set(0, NET_OK);
    return microkit_msginfo_new(0, 1);
}

static microkit_msginfo handle_net_send(void)
{
    uint32_t handle = (uint32_t)microkit_mr_get(1);
    net_pd_client_t *c = find_client(handle);
    if (!c) {
        microkit_mr_set(0, NET_ERR_BAD_HANDLE);
        return microkit_msginfo_new(0, 1);
    }
    if (c->type == HANDLE_TYPE_NIC)
        return handle_net_send_nic(c, handle);
    return handle_net_send_sock(c, handle);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MSG_NET_RECV (0x2104) — receive a frame (NIC) or data bytes (socket)
 *   MR1=handle, MR2=max_bytes
 *   Reply: MR0=ok, MR1=len, MR2=shmem_offset
 * ═══════════════════════════════════════════════════════════════════════════ */
static microkit_msginfo handle_net_recv_nic(net_pd_client_t *c, uint32_t handle)
{
    uint32_t max_len = (uint32_t)microkit_mr_get(2);
    (void)handle;

    volatile netpd_ring_t *ring = slot_ring(c->shmem_slot);
    uint32_t data_off = NETPD_SLOT_OFFSET(c->shmem_slot) + NETPD_SLOT_HDR_SIZE;

    /* Simple ring: check if rx_head != rx_tail (data available) */
    if (ring->rx_head == ring->rx_tail) {
        /* WG_INTEGRATION_POINT: poll WireGuard RX tunnel here to inject
         * decrypted frames into the ring before returning empty. */
        microkit_mr_set(0, NET_OK);
        microkit_mr_set(1, 0u);
        microkit_mr_set(2, 0u);
        return microkit_msginfo_new(0, 3);
    }

    /* Frame length stored at ring->rx_tail as a 2-byte header */
    uint32_t frame_len = *(volatile uint16_t *)(net_pd_shmem_vaddr + data_off
                          + ring->rx_tail);
    if (frame_len > max_len) frame_len = max_len;

    uint32_t frame_off = data_off + ring->rx_tail + 2u;
    ring->rx_tail = (ring->rx_tail + 2u + frame_len) % NETPD_SLOT_DATA_SIZE;
    c->rx_pkts++;
    c->rx_bytes += frame_len;
    iface_rx_pkts++;
    iface_rx_bytes += frame_len;

    microkit_mr_set(0, NET_OK);
    microkit_mr_set(1, frame_len);
    microkit_mr_set(2, frame_off);
    return microkit_msginfo_new(0, 3);
}

static microkit_msginfo handle_net_recv_sock(net_pd_client_t *c, uint32_t handle)
{
    uint32_t max_len = (uint32_t)microkit_mr_get(2);
    (void)handle;

    uint32_t slot_off = NETPD_SLOT_OFFSET(c->shmem_slot) + NETPD_SLOT_HDR_SIZE;
    uint8_t *buf = (uint8_t *)(net_pd_shmem_vaddr + slot_off);

    if (max_len > NETPD_SLOT_DATA_SIZE) max_len = NETPD_SLOT_DATA_SIZE;

    int n = lwip_net_recv((uint8_t)(c->shmem_slot & 0xFFu), buf,
                          max_len > 65536u ? 65536u : max_len);
    if (n > 0) {
        c->rx_pkts++;
        c->rx_bytes += (uint32_t)n;
    }

    microkit_mr_set(0, NET_OK);
    microkit_mr_set(1, (uint32_t)(n > 0 ? n : 0));
    microkit_mr_set(2, slot_off);
    return microkit_msginfo_new(0, 3);
}

static microkit_msginfo handle_net_recv(void)
{
    uint32_t handle = (uint32_t)microkit_mr_get(1);
    net_pd_client_t *c = find_client(handle);
    if (!c) {
        microkit_mr_set(0, NET_ERR_BAD_HANDLE);
        microkit_mr_set(1, 0); microkit_mr_set(2, 0);
        return microkit_msginfo_new(0, 3);
    }
    if (c->type == HANDLE_TYPE_NIC)
        return handle_net_recv_nic(c, handle);
    return handle_net_recv_sock(c, handle);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MSG_NET_DEV_STATUS (0x2105) — query link status and packet counters
 *   MR1=handle (0xFFFFFFFF = global iface stats)
 *   Reply: MR0=ok, MR1=link_up, MR2=rx_pkts_lo, MR3=tx_pkts_lo,
 *          MR4=rx_bytes_lo, MR5=tx_bytes_lo, MR6=rx_errors, MR7=tx_errors
 * ═══════════════════════════════════════════════════════════════════════════ */
static microkit_msginfo handle_net_dev_status(void)
{
    uint32_t handle = (uint32_t)microkit_mr_get(1);

    uint64_t rx_pkts, tx_pkts, rx_bytes, tx_bytes;
    uint32_t rx_errors, tx_errors;
    uint32_t link;

    if (handle == 0xFFFFFFFFu) {
        rx_pkts  = iface_rx_pkts;  tx_pkts  = iface_tx_pkts;
        rx_bytes = iface_rx_bytes; tx_bytes = iface_tx_bytes;
        rx_errors = iface_rx_errors; tx_errors = iface_tx_errors;
        link = iface_link_up ? 1u : 0u;
    } else {
        net_pd_client_t *c = find_client(handle);
        if (!c) {
            microkit_mr_set(0, NET_ERR_BAD_HANDLE);
            return microkit_msginfo_new(0, 1);
        }
        rx_pkts  = c->rx_pkts;  tx_pkts  = c->tx_pkts;
        rx_bytes = c->rx_bytes; tx_bytes = c->tx_bytes;
        rx_errors = c->rx_errors; tx_errors = c->tx_errors;
        link = iface_link_up ? 1u : 0u;
    }

    microkit_mr_set(0, NET_OK);
    microkit_mr_set(1, link);
    microkit_mr_set(2, (uint32_t)(rx_pkts  & 0xFFFFFFFFu));
    microkit_mr_set(3, (uint32_t)(tx_pkts  & 0xFFFFFFFFu));
    microkit_mr_set(4, (uint32_t)(rx_bytes & 0xFFFFFFFFu));
    microkit_mr_set(5, (uint32_t)(tx_bytes & 0xFFFFFFFFu));
    microkit_mr_set(6, rx_errors);
    microkit_mr_set(7, tx_errors);
    return microkit_msginfo_new(0, 8);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MSG_NET_CONFIGURE (0x2106) — set MTU and flags for a NIC handle
 *   MR1=handle, MR2=mtu (0=keep current), MR3=flags
 * ═══════════════════════════════════════════════════════════════════════════ */
static microkit_msginfo handle_net_configure(void)
{
    uint32_t handle = (uint32_t)microkit_mr_get(1);
    uint32_t mtu    = (uint32_t)microkit_mr_get(2);
    uint32_t flags  = (uint32_t)microkit_mr_get(3);

    net_pd_client_t *c = find_nic(handle);
    if (!c) {
        microkit_mr_set(0, NET_ERR_BAD_HANDLE);
        return microkit_msginfo_new(0, 1);
    }

    if (mtu > 0u && mtu <= 9000u) c->mtu = mtu;
    c->cfg_flags = flags;

    log_drain_write(17, 17, "[net_pd] CONFIGURE: handle=");
    log_dec(handle);
    log_drain_write(17, 17, " mtu=");
    log_dec(c->mtu);
    log_drain_write(17, 17, " flags=");
    log_hex(flags);
    log_drain_write(17, 17, "\n");

    microkit_mr_set(0, NET_OK);
    return microkit_msginfo_new(0, 1);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MSG_NET_FILTER_ADD (0x2107) — install a BPF-style filter for a NIC handle
 *   MR1=handle, MR2=filter_len (bytecodes in shmem at slot data region)
 *   Reply: MR0=ok, MR1=filter_id
 * ═══════════════════════════════════════════════════════════════════════════ */
static microkit_msginfo handle_net_filter_add(void)
{
    uint32_t handle     = (uint32_t)microkit_mr_get(1);
    uint32_t filter_len = (uint32_t)microkit_mr_get(2);

    net_pd_client_t *c = find_nic(handle);
    if (!c) {
        microkit_mr_set(0, NET_ERR_BAD_HANDLE);
        microkit_mr_set(1, 0);
        return microkit_msginfo_new(0, 2);
    }

    if (c->filter_count >= NET_MAX_FILTERS) {
        microkit_mr_set(0, NET_ERR_FILTER_FULL);
        microkit_mr_set(1, 0);
        return microkit_msginfo_new(0, 2);
    }

    uint32_t filter_id = c->next_filter_id++;
    uint32_t slot      = c->filter_count;
    c->filter_ids[slot]  = filter_id;
    c->filter_lens[slot] = filter_len;
    c->filter_count++;

    /* BPF filter bytecodes are in shmem; real enforcement would parse them here.
     * WG_INTEGRATION_POINT: apply filter to the WireGuard RX path as well. */
    log_drain_write(17, 17, "[net_pd] FILTER_ADD: handle=");
    log_dec(handle);
    log_drain_write(17, 17, " filter_id=");
    log_dec(filter_id);
    log_drain_write(17, 17, " len=");
    log_dec(filter_len);
    log_drain_write(17, 17, "\n");

    microkit_mr_set(0, NET_OK);
    microkit_mr_set(1, filter_id);
    return microkit_msginfo_new(0, 2);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MSG_NET_FILTER_REMOVE (0x2108) — remove a BPF filter from a NIC handle
 *   MR1=handle, MR2=filter_id
 * ═══════════════════════════════════════════════════════════════════════════ */
static microkit_msginfo handle_net_filter_remove(void)
{
    uint32_t handle    = (uint32_t)microkit_mr_get(1);
    uint32_t filter_id = (uint32_t)microkit_mr_get(2);

    net_pd_client_t *c = find_nic(handle);
    if (!c) {
        microkit_mr_set(0, NET_ERR_BAD_HANDLE);
        return microkit_msginfo_new(0, 1);
    }

    for (uint32_t i = 0; i < c->filter_count; i++) {
        if (c->filter_ids[i] == filter_id) {
            /* Compact the filter table */
            for (uint32_t j = i; j + 1u < c->filter_count; j++) {
                c->filter_ids[j]  = c->filter_ids[j + 1u];
                c->filter_lens[j] = c->filter_lens[j + 1u];
            }
            c->filter_count--;
            log_drain_write(17, 17, "[net_pd] FILTER_REMOVE: handle=");
            log_dec(handle);
            log_drain_write(17, 17, " filter_id=");
            log_dec(filter_id);
            log_drain_write(17, 17, "\n");
            microkit_mr_set(0, NET_OK);
            return microkit_msginfo_new(0, 1);
        }
    }

    microkit_mr_set(0, NET_ERR_BAD_FILTER_ID);
    return microkit_msginfo_new(0, 1);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MSG_NET_SOCKET_OPEN (0x2109) — open a TCP or UDP socket
 *   MR1=proto (NET_PROTO_TCP=0, NET_PROTO_UDP=1)
 *   Reply: MR0=ok, MR1=sock_handle, MR2=shmem_slot_offset
 * ═══════════════════════════════════════════════════════════════════════════ */
static microkit_msginfo handle_socket_open(void)
{
    uint32_t proto = (uint32_t)microkit_mr_get(1);

    if (proto != NET_PROTO_TCP && proto != NET_PROTO_UDP) {
        microkit_mr_set(0, NET_ERR_BAD_HANDLE);
        return microkit_msginfo_new(0, 1);
    }

    if (active_clients >= NET_MAX_CLIENTS) {
        microkit_mr_set(0, NET_ERR_NO_SLOTS);
        return microkit_msginfo_new(0, 1);
    }

    int slot = alloc_slot();
    if (slot < 0) {
        microkit_mr_set(0, NET_ERR_NO_SLOTS);
        return microkit_msginfo_new(0, 1);
    }

    int handle = -1;
    for (int i = 0; i < (int)NET_MAX_CLIENTS; i++) {
        if (!clients[i].active) { handle = i; break; }
    }
    if (handle < 0) {
        free_slot((uint32_t)slot);
        microkit_mr_set(0, NET_ERR_NO_SLOTS);
        return microkit_msginfo_new(0, 1);
    }

    net_pd_client_t *c = &clients[handle];
    c->active      = true;
    c->type        = HANDLE_TYPE_SOCKET;
    c->shmem_slot  = (uint32_t)slot;
    c->proto       = (uint8_t)proto;
    c->sock_state  = SOCK_STATE_FREE;
    c->local_port  = 0;
    c->remote_ip   = 0;
    c->remote_port = 0;
    c->rx_pkts  = 0; c->tx_pkts  = 0;
    c->rx_bytes = 0; c->tx_bytes = 0;
    c->rx_errors = 0; c->tx_errors = 0;

    init_ring((uint32_t)slot, (uint32_t)handle);
    active_clients++;

    log_drain_write(17, 17, "[net_pd] SOCKET_OPEN: handle=");
    log_dec((uint32_t)handle);
    log_drain_write(17, 17, " proto=");
    log_dec(proto);
    log_drain_write(17, 17, "\n");

    microkit_mr_set(0, NET_OK);
    microkit_mr_set(1, (uint32_t)handle);
    microkit_mr_set(2, NETPD_SLOT_OFFSET((uint32_t)slot));
    return microkit_msginfo_new(0, 3);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MSG_NET_SOCKET_CLOSE (0x210A) — close a socket
 *   MR1=sock_handle
 * ═══════════════════════════════════════════════════════════════════════════ */
static microkit_msginfo handle_socket_close(void)
{
    uint32_t handle = (uint32_t)microkit_mr_get(1);

    net_pd_client_t *c = find_sock(handle);
    if (!c) {
        microkit_mr_set(0, NET_ERR_BAD_HANDLE);
        return microkit_msginfo_new(0, 1);
    }

    /* Close underlying lwIP TCP PCB if connected */
    uint32_t slot = c->shmem_slot;
    if (c->proto == NET_PROTO_TCP && g_conns[slot].tcp) {
        tcp_close(g_conns[slot].tcp);
        g_conns[slot].tcp   = NULL;
        g_conns[slot].state = 0u;
    }

    log_drain_write(17, 17, "[net_pd] SOCKET_CLOSE: handle=");
    log_dec(handle);
    log_drain_write(17, 17, "\n");

    clear_ring(c->shmem_slot);
    free_slot(c->shmem_slot);
    c->active = false;
    if (active_clients > 0) active_clients--;

    microkit_mr_set(0, NET_OK);
    return microkit_msginfo_new(0, 1);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MSG_NET_SOCKET_CONNECT (0x210B) — TCP connect to remote endpoint
 *   MR1=sock_handle, MR2=dest_ip (u32 big-endian), MR3=dest_port
 *   Reply: MR0=ok (connection initiated; poll DEV_STATUS for completion)
 * ═══════════════════════════════════════════════════════════════════════════ */
static microkit_msginfo handle_socket_connect(void)
{
    uint32_t handle    = (uint32_t)microkit_mr_get(1);
    uint32_t dest_ip   = (uint32_t)microkit_mr_get(2);
    uint32_t dest_port = (uint32_t)microkit_mr_get(3);

    net_pd_client_t *c = find_sock(handle);
    if (!c) {
        microkit_mr_set(0, NET_ERR_BAD_HANDLE);
        return microkit_msginfo_new(0, 1);
    }
    if (c->proto != NET_PROTO_TCP) {
        microkit_mr_set(0, NET_ERR_BAD_HANDLE);
        return microkit_msginfo_new(0, 1);
    }

    int err = lwip_net_connect((uint8_t)(c->shmem_slot & 0xFFu),
                               dest_ip, (uint16_t)(dest_port & 0xFFFFu));

    if (err == 0) {
        c->sock_state  = SOCK_STATE_CONNECTING;
        c->remote_ip   = dest_ip;
        c->remote_port = dest_port;
    }

    log_drain_write(17, 17, "[net_pd] SOCKET_CONNECT: handle=");
    log_dec(handle);
    log_drain_write(17, 17, " dst_ip=");
    log_hex(dest_ip);
    log_drain_write(17, 17, " port=");
    log_dec(dest_port);
    log_drain_write(17, 17, " err=");
    log_dec((uint32_t)(err < 0 ? (uint32_t)(-err) : 0u));
    log_drain_write(17, 17, "\n");

    microkit_mr_set(0, err == 0 ? NET_OK : NET_ERR_BAD_HANDLE);
    return microkit_msginfo_new(0, 1);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MSG_NET_SOCKET_BIND (0x210C) — bind socket to a local port
 *   MR1=sock_handle, MR2=port
 * ═══════════════════════════════════════════════════════════════════════════ */
static microkit_msginfo handle_socket_bind(void)
{
    uint32_t handle = (uint32_t)microkit_mr_get(1);
    uint32_t port   = (uint32_t)microkit_mr_get(2);

    net_pd_client_t *c = find_sock(handle);
    if (!c) {
        microkit_mr_set(0, NET_ERR_BAD_HANDLE);
        return microkit_msginfo_new(0, 1);
    }

    if (port == 0 || port > 65535u) {
        microkit_mr_set(0, NET_ERR_BAD_HANDLE);
        return microkit_msginfo_new(0, 1);
    }

    c->local_port = port;
    c->sock_state = SOCK_STATE_BOUND;

    log_drain_write(17, 17, "[net_pd] SOCKET_BIND: handle=");
    log_dec(handle);
    log_drain_write(17, 17, " port=");
    log_dec(port);
    log_drain_write(17, 17, "\n");

    /* WG_INTEGRATION_POINT: for WireGuard-tunnelled sockets, bind the UDP
     * underlay port here so keepalives can reach the remote peer. */

    microkit_mr_set(0, NET_OK);
    return microkit_msginfo_new(0, 1);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MSG_NET_SOCKET_LISTEN (0x210D) — begin accepting incoming connections
 *   MR1=sock_handle
 * ═══════════════════════════════════════════════════════════════════════════ */
static microkit_msginfo handle_socket_listen(void)
{
    uint32_t handle = (uint32_t)microkit_mr_get(1);

    net_pd_client_t *c = find_sock(handle);
    if (!c) {
        microkit_mr_set(0, NET_ERR_BAD_HANDLE);
        return microkit_msginfo_new(0, 1);
    }
    if (c->proto != NET_PROTO_TCP || c->sock_state != SOCK_STATE_BOUND) {
        microkit_mr_set(0, NET_ERR_BAD_HANDLE);
        return microkit_msginfo_new(0, 1);
    }

    c->sock_state = SOCK_STATE_LISTENING;

    log_drain_write(17, 17, "[net_pd] SOCKET_LISTEN: handle=");
    log_dec(handle);
    log_drain_write(17, 17, " port=");
    log_dec(c->local_port);
    log_drain_write(17, 17, "\n");

    /* LWIP_INTEGRATION_POINT: call tcp_listen() on a pre-bound PCB.
     * Accepted PCBs are queued in g_conns[shmem_slot].accept_queue (future). */

    microkit_mr_set(0, NET_OK);
    return microkit_msginfo_new(0, 1);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MSG_NET_SOCKET_ACCEPT (0x210E) — accept a pending incoming connection
 *   MR1=sock_handle (the listening socket)
 *   Reply: MR0=ok, MR1=new_sock_handle (0 = none pending), MR2=peer_ip, MR3=peer_port
 * ═══════════════════════════════════════════════════════════════════════════ */
static microkit_msginfo handle_socket_accept(void)
{
    uint32_t handle = (uint32_t)microkit_mr_get(1);

    net_pd_client_t *c = find_sock(handle);
    if (!c) {
        microkit_mr_set(0, NET_ERR_BAD_HANDLE);
        microkit_mr_set(1, 0); microkit_mr_set(2, 0); microkit_mr_set(3, 0);
        return microkit_msginfo_new(0, 4);
    }
    if (c->sock_state != SOCK_STATE_LISTENING) {
        microkit_mr_set(0, NET_ERR_BAD_HANDLE);
        microkit_mr_set(1, 0); microkit_mr_set(2, 0); microkit_mr_set(3, 0);
        return microkit_msginfo_new(0, 4);
    }

    /* LWIP_INTEGRATION_POINT: dequeue an accepted PCB from g_conns accept_queue.
     * Allocate a new client handle, wire up the accepted PCB to g_conns[new_slot],
     * and return new_handle + peer address. */
    log_drain_write(17, 17, "[net_pd] SOCKET_ACCEPT: handle=");
    log_dec(handle);
    log_drain_write(17, 17, " (no pending connection)\n");

    microkit_mr_set(0, NET_OK);
    microkit_mr_set(1, 0u);   /* no connection pending yet */
    microkit_mr_set(2, 0u);
    microkit_mr_set(3, 0u);
    return microkit_msginfo_new(0, 4);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * MSG_NET_SOCKET_SET_OPT (0x210F) — set socket option
 *   MR1=sock_handle, MR2=option_id, MR3=value
 * ═══════════════════════════════════════════════════════════════════════════ */
static microkit_msginfo handle_socket_set_opt(void)
{
    uint32_t handle = (uint32_t)microkit_mr_get(1);
    uint32_t opt    = (uint32_t)microkit_mr_get(2);
    uint32_t val    = (uint32_t)microkit_mr_get(3);

    net_pd_client_t *c = find_sock(handle);
    if (!c) {
        microkit_mr_set(0, NET_ERR_BAD_HANDLE);
        return microkit_msginfo_new(0, 1);
    }

    switch (opt) {
    case NET_SOCKOPT_REUSEADDR:
        /* LWIP_INTEGRATION_POINT: set ip_set_option(pcb, SOF_REUSEADDR) */
        break;
    case NET_SOCKOPT_NODELAY:
        /* LWIP_INTEGRATION_POINT: set tcp_set_flags(pcb, TF_NODELAY) */
        break;
    case NET_SOCKOPT_KEEPALIVE:
        /* LWIP_INTEGRATION_POINT: set ip_set_option(pcb, SOF_KEEPALIVE) */
        break;
    default:
        microkit_mr_set(0, NET_ERR_BAD_HANDLE);
        return microkit_msginfo_new(0, 1);
    }

    log_drain_write(17, 17, "[net_pd] SOCKET_SET_OPT: handle=");
    log_dec(handle);
    log_drain_write(17, 17, " opt=");
    log_dec(opt);
    log_drain_write(17, 17, " val=");
    log_dec(val);
    log_drain_write(17, 17, "\n");

    microkit_mr_set(0, NET_OK);
    return microkit_msginfo_new(0, 1);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Microkit entry points
 * ═══════════════════════════════════════════════════════════════════════════ */

void init(void)
{
    agentos_log_boot("net_pd");
    log_drain_write(17, 17,
        "[net_pd] Initialising net_pd (priority 160, passive)\n");

    for (int i = 0; i < (int)NET_MAX_CLIENTS; i++) {
        clients[i].active     = false;
        clients[i].type       = HANDLE_TYPE_INVALID;
        clients[i].shmem_slot = 0;
    }
    active_clients = 0;
    slot_bitmap    = 0u;

    probe_virtio_net();

    log_drain_write(17, 17, "[net_pd] READY — max_clients=");
    log_dec(NET_MAX_CLIENTS);
    log_drain_write(17, 17, " hw=");
    log_drain_write(17, 17, hw_present ? "1" : "0");
    log_drain_write(17, 17, "\n");
}

/*
 * notified() — virtio-net RX interrupt or periodic lwIP timer tick.
 *
 *   ch=0: virtio-net RX interrupt from the controller (device ring has frames)
 *   ch=1: 10 ms periodic tick for lwIP timer processing
 *
 * WG_INTEGRATION_POINT: on the timer channel, also call wg_keepalive_tick()
 * to send WireGuard keepalives to peers that have gone quiet.
 */
void notified(microkit_channel ch)
{
    switch (ch) {
    case 0:
        lwip_virtio_rx_poll();
        break;
    case 1:
        lwip_tick();
        lwip_virtio_rx_poll();
        sys_check_timeouts();
        break;
    default:
        log_drain_write(17, 17, "[net_pd] unexpected notify ch=");
        log_dec((uint32_t)ch);
        log_drain_write(17, 17, "\n");
        break;
    }
}

/*
 * protected() — IPC dispatch for all net_pd operations.
 *
 * The label field of msginfo carries the opcode (MSG_NET_*).
 * All channels share a single dispatch table.
 */
microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo)
{
    (void)ch;

    uint64_t op = microkit_msginfo_get_label(msginfo);

    switch (op) {
    /* ── Packet I/O interface (net_contract.h) ─────────────────────────────── */
    case MSG_NET_OPEN:           return handle_net_open();
    case MSG_NET_CLOSE:          return handle_net_close();
    case MSG_NET_SEND:           return handle_net_send();
    case MSG_NET_RECV:           return handle_net_recv();
    case MSG_NET_DEV_STATUS:     return handle_net_dev_status();
    case MSG_NET_CONFIGURE:      return handle_net_configure();
    case MSG_NET_FILTER_ADD:     return handle_net_filter_add();
    case MSG_NET_FILTER_REMOVE:  return handle_net_filter_remove();

    /* ── Socket interface (net_contract.h socket extension) ─────────────────── */
    case MSG_NET_SOCKET_OPEN:    return handle_socket_open();
    case MSG_NET_SOCKET_CLOSE:   return handle_socket_close();
    case MSG_NET_SOCKET_CONNECT: return handle_socket_connect();
    case MSG_NET_SOCKET_BIND:    return handle_socket_bind();
    case MSG_NET_SOCKET_LISTEN:  return handle_socket_listen();
    case MSG_NET_SOCKET_ACCEPT:  return handle_socket_accept();
    case MSG_NET_SOCKET_SET_OPT: return handle_socket_set_opt();

    default:
        log_drain_write(17, 17, "[net_pd] unknown op=");
        log_hex((uint32_t)(op & 0xFFFFFFFFu));
        log_drain_write(17, 17, "\n");
        microkit_mr_set(0, NET_ERR_BAD_HANDLE);
        return microkit_msginfo_new(0, 1);
    }
}
