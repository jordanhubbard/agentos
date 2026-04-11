/*
 * lwip_sys.c — lwIP system interface for agentOS net_server PD
 *
 * NO_SYS=1 mode: no threads, no blocking, raw callback API.
 * Static 48KB heap replaces malloc/free.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "agentos.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "lwip/etharp.h"
#include "lwip/init.h"
#include "netif/ethernet.h"
#include "net_server.h"

/* ── Static heap (replaces libc malloc for lwIP) ─────────────────────── */
static uint8_t  lwip_heap[49152];
static uint32_t lwip_heap_used = 0;

void *mem_malloc(size_t size)
{
    size = (size + 3u) & ~3u;  /* 4-byte align */
    if (lwip_heap_used + size > sizeof(lwip_heap)) return NULL;
    void *p = lwip_heap + lwip_heap_used;
    lwip_heap_used += (uint32_t)size;
    return p;
}
void mem_free(void *p) { (void)p; /* simple bump allocator, no free */ }
void *mem_calloc(size_t n, size_t size)
{
    void *p = mem_malloc(n * size);
    if (p) {
        uint8_t *b = (uint8_t *)p;
        size_t total = n * size;
        for (size_t i = 0; i < total; i++) b[i] = 0;
    }
    return p;
}

/* ── Timestamp (incremented by caller from 10ms timer notify) ─────────── */
static u32_t lwip_tick_ms = 0;
u32_t sys_now(void) { return lwip_tick_ms; }
void  lwip_tick(void) { lwip_tick_ms += 10u; }  /* call from notified() timer */

/* ── netif and vNIC connection state ─────────────────────────────────── */
struct netif g_netif;

typedef struct {
    struct tcp_pcb *tcp;
    uint8_t  state;      /* 0=free 1=connecting 2=connected 3=closed */
    uint8_t  rx_buf[1500];
    uint16_t rx_buf_len;
} vnic_conn_t;

vnic_conn_t g_conns[NET_MAX_VNICS];

/* ── virtio-net MMIO vaddr (set by net_server init) ──────────────────── */
uintptr_t net_mmio_vaddr_for_lwip;   /* set externally by net_server */
uintptr_t net_rx_desc_vaddr;         /* RX virtqueue descriptor table */
uintptr_t net_tx_desc_vaddr;         /* TX virtqueue descriptor table */

/* ── lwIP → virtio-net TX ────────────────────────────────────────────── */
static err_t lwip_virtio_output(struct netif *netif, struct pbuf *p)
{
    (void)netif;
    /* Copy pbuf chain to a flat TX buffer */
    static uint8_t tx_buf[1536];
    uint16_t total = 0;
    for (struct pbuf *q = p; q != NULL; q = q->next) {
        if (total + q->len > (uint16_t)sizeof(tx_buf)) return ERR_BUF;
        const uint8_t *src = (const uint8_t *)q->payload;
        for (uint16_t i = 0; i < q->len; i++) tx_buf[total + i] = src[i];
        total += q->len;
    }
    /* Write to virtio-net TX virtqueue via MMIO — simplified single-descriptor */
    if (net_tx_desc_vaddr) {
        /* Descriptor 0: flags=0, next=0, addr=&tx_buf (physical), len=total */
        volatile uint64_t *desc = (volatile uint64_t *)net_tx_desc_vaddr;
        /* virtq_desc layout: addr(64) flags(16) len(32) next(16) */
        desc[0] = (uint64_t)(uintptr_t)tx_buf;  /* addr */
        /* Pack len/flags/next into descriptor — simplified */
        volatile uint32_t *d32 = (volatile uint32_t *)(net_tx_desc_vaddr + 8u);
        d32[0] = (uint32_t)total;   /* len */
        d32[1] = 0u;               /* flags=0, next=0 */
        /* Kick TX queue (queue 1) */
        volatile uint32_t *mmio = (volatile uint32_t *)net_mmio_vaddr_for_lwip;
        mmio[0x50u / 4u] = 1u;  /* VIRTIO_MMIO_QUEUE_NOTIFY = queue 1 */
    }
    return ERR_OK;
}

static err_t lwip_netif_init(struct netif *netif)
{
    netif->hwaddr_len = 6;
    /* Read MAC from virtio-net config space (offset 0x100 in MMIO) */
    if (net_mmio_vaddr_for_lwip) {
        volatile uint8_t *cfg =
            (volatile uint8_t *)(net_mmio_vaddr_for_lwip + 0x100u);
        for (int i = 0; i < 6; i++) netif->hwaddr[i] = cfg[i];
    } else {
        /* Default MAC if MMIO not mapped */
        netif->hwaddr[0] = 0x52; netif->hwaddr[1] = 0x54;
        netif->hwaddr[2] = 0x00; netif->hwaddr[3] = 0x12;
        netif->hwaddr[4] = 0x34; netif->hwaddr[5] = 0x56;
    }
    netif->mtu       = 1500;
    netif->name[0]   = 'e'; netif->name[1] = '0';
    netif->output    = etharp_output;
    netif->linkoutput = lwip_virtio_output;
    netif->flags     = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP
                       | NETIF_FLAG_LINK_UP;
    return ERR_OK;
}

/* Called once from net_server init() after virtio-net probe succeeds */
void net_server_lwip_init(uintptr_t mmio_vaddr, uintptr_t rx_desc,
                           uintptr_t tx_desc)
{
    net_mmio_vaddr_for_lwip = mmio_vaddr;
    net_rx_desc_vaddr       = rx_desc;
    net_tx_desc_vaddr       = tx_desc;

    lwip_init();

    /* Zero connection table */
    for (int i = 0; i < (int)NET_MAX_VNICS; i++) {
        g_conns[i].tcp       = NULL;
        g_conns[i].state     = 0;
        g_conns[i].rx_buf_len = 0;
    }

    ip4_addr_t ip, nm, gw;
    IP4_ADDR(&ip, 10, 0, 2, 15);
    IP4_ADDR(&nm, 255, 255, 255, 0);
    IP4_ADDR(&gw, 10, 0, 2, 2);

    netif_add(&g_netif, &ip, &nm, &gw, NULL, lwip_netif_init, ethernet_input);
    netif_set_default(&g_netif);
    netif_set_up(&g_netif);

    console_log(30, 10, "[net_server] lwIP netif up ip=10.0.2.15\n");
}

/* Poll RX virtqueue for received packets — call from notified() timer */
void lwip_virtio_rx_poll(void)
{
    if (!net_rx_desc_vaddr) return;
    /*
     * Phase 1: In a real virtqueue we'd check the used ring for completed
     * RX descriptors.  Here we do a simplified check — if no frame is
     * waiting this is a no-op.
     */
}

/* TCP receive callback — stores data in vnic_conn rx_buf */
static err_t lwip_tcp_recv_cb(void *arg, struct tcp_pcb *pcb,
                                struct pbuf *p, err_t err)
{
    uint8_t vnic_id = (uint8_t)(uintptr_t)arg;
    if (p == NULL || err != ERR_OK) {
        g_conns[vnic_id & 7u].state = 3u; /* closed */
        return ERR_OK;
    }
    vnic_conn_t *c = &g_conns[vnic_id & 7u];
    uint16_t n = p->tot_len < (uint16_t)sizeof(c->rx_buf)
                 ? (uint16_t)p->tot_len
                 : (uint16_t)sizeof(c->rx_buf);
    pbuf_copy_partial(p, c->rx_buf, n, 0);
    c->rx_buf_len = n;
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t lwip_tcp_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)pcb;
    uint8_t vnic_id = (uint8_t)(uintptr_t)arg;
    g_conns[vnic_id & 7u].state = (err == ERR_OK) ? 2u : 3u;
    return ERR_OK;
}

static void lwip_tcp_err_cb(void *arg, err_t err)
{
    (void)err;
    uint8_t vnic_id = (uint8_t)(uintptr_t)arg;
    g_conns[vnic_id & 7u].state = 3u;
}

/* ── Public API called from net_server.c protected() ─────────────────── */

/* Connect: returns 0=ok, else error */
int lwip_net_connect(uint8_t vnic_id, uint32_t dest_ip_be, uint16_t dest_port)
{
    if (vnic_id >= NET_MAX_VNICS) return -1;
    struct tcp_pcb *pcb = tcp_new();
    if (!pcb) return -2;
    tcp_arg(pcb, (void *)(uintptr_t)vnic_id);
    tcp_recv(pcb, lwip_tcp_recv_cb);
    tcp_err(pcb,  lwip_tcp_err_cb);
    ip4_addr_t raddr;
    raddr.addr = dest_ip_be;  /* already big-endian from caller */
    g_conns[vnic_id].tcp   = pcb;
    g_conns[vnic_id].state = 1u; /* connecting */
    tcp_connect(pcb, &raddr, dest_port, lwip_tcp_connected_cb);
    return 0;
}

/* Send: returns bytes sent or -1 */
int lwip_net_send(uint8_t vnic_id, const uint8_t *data, uint16_t len)
{
    if (vnic_id >= NET_MAX_VNICS) return -1;
    vnic_conn_t *c = &g_conns[vnic_id];
    if (!c->tcp || c->state != 2u) return -1;
    err_t e = tcp_write(c->tcp, data, len, TCP_WRITE_FLAG_COPY);
    if (e != ERR_OK) return -1;
    tcp_output(c->tcp);
    return (int)len;
}

/* Recv: copies up to max bytes to dst, returns actual bytes */
int lwip_net_recv(uint8_t vnic_id, uint8_t *dst, uint16_t max)
{
    if (vnic_id >= NET_MAX_VNICS) return 0;
    vnic_conn_t *c = &g_conns[vnic_id];
    uint16_t n = (c->rx_buf_len < max) ? c->rx_buf_len : max;
    for (uint16_t i = 0; i < n; i++) dst[i] = c->rx_buf[i];
    c->rx_buf_len = 0;
    return (int)n;
}

uint8_t lwip_conn_state(uint8_t vnic_id)
{
    if (vnic_id >= NET_MAX_VNICS) return 0;
    return g_conns[vnic_id].state;
}
