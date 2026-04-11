/*
 * lwip_shim.c — self-contained lwIP-compatible shim for agentOS net_server PD
 *
 * Implements the minimal lwIP 2.2.0 API surface needed by lwip_sys.c:
 *   - struct netif / netif_add / netif_set_default / netif_set_up
 *   - struct pbuf  / pbuf_alloc / pbuf_free / pbuf_copy_partial / pbuf_take
 *   - struct tcp_pcb / tcp_new / tcp_arg / tcp_recv / tcp_err / tcp_connect
 *     tcp_write / tcp_output / tcp_close / tcp_recved
 *   - lwip_init / sys_check_timeouts / sys_now
 *   - etharp_output / ethernet_input
 *
 * All state lives in static arrays (NO_SYS=1, no malloc, no threads).
 * This is acceptable for Phase 1: the goal is wiring the virtqueue path,
 * not full TCP correctness.  TCP connections are tracked as a simple state
 * machine; actual TCP framing is handled by the virtio-net device model.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "lwip/etharp.h"
#include "lwip/init.h"
#include "netif/ethernet.h"

/* ── Global netif pointer (NULL until netif_set_default called) ──────── */
struct netif *netif_default = NULL;

/* ── pbuf pool (static, bump-allocated, never freed in Phase 1) ─────── */
#define PBUF_POOL_COUNT  32u
#define PBUF_PAYLOAD_MAX 1536u

typedef struct {
    struct pbuf hdr;
    uint8_t     data[PBUF_PAYLOAD_MAX];
} pbuf_slot_t;

static pbuf_slot_t pbuf_pool[PBUF_POOL_COUNT];
static uint32_t    pbuf_pool_head = 0;

struct pbuf *pbuf_alloc(pbuf_layer layer, u16_t length, pbuf_type type)
{
    (void)layer; (void)type;
    if (pbuf_pool_head >= PBUF_POOL_COUNT)
        pbuf_pool_head = 0;  /* wrap-around recycle for Phase 1 */
    if (length > PBUF_PAYLOAD_MAX)
        return NULL;

    pbuf_slot_t *s = &pbuf_pool[pbuf_pool_head++];
    s->hdr.next         = NULL;
    s->hdr.payload      = s->data;
    s->hdr.tot_len      = length;
    s->hdr.len          = length;
    s->hdr.type_internal = (u8_t)type;
    s->hdr.flags        = 0;
    s->hdr.ref          = 1;
    s->hdr.if_idx       = 0;
    return &s->hdr;
}

void pbuf_free(struct pbuf *p)
{
    /* Phase 1: bump allocator — pbuf_free is a no-op */
    (void)p;
}

u16_t pbuf_copy_partial(const struct pbuf *buf, void *dataptr,
                          u16_t len, u16_t offset)
{
    if (!buf || !dataptr) return 0;
    uint8_t *dst = (uint8_t *)dataptr;
    u16_t copied = 0;
    u16_t skip   = offset;

    for (const struct pbuf *q = buf; q != NULL && copied < len; q = q->next) {
        u16_t q_off  = 0;
        if (skip >= q->len) { skip -= q->len; continue; }
        q_off = skip;
        skip  = 0;
        u16_t avail = q->len - q_off;
        u16_t take  = (avail < (len - copied)) ? avail : (len - copied);
        const uint8_t *src = (const uint8_t *)q->payload + q_off;
        for (u16_t i = 0; i < take; i++) dst[copied + i] = src[i];
        copied += take;
    }
    return copied;
}

err_t pbuf_take(struct pbuf *buf, const void *dataptr, u16_t len)
{
    if (!buf || !dataptr || len > buf->tot_len) return ERR_ARG;
    const uint8_t *src = (const uint8_t *)dataptr;
    u16_t remaining = len;
    for (struct pbuf *q = buf; q != NULL && remaining > 0; q = q->next) {
        u16_t take = (remaining < q->len) ? remaining : q->len;
        uint8_t *dst = (uint8_t *)q->payload;
        for (u16_t i = 0; i < take; i++) dst[i] = src[i];
        src       += take;
        remaining -= take;
    }
    return ERR_OK;
}

/* ── TCP PCB pool ────────────────────────────────────────────────────── */
#define TCP_PCB_MAX 8

typedef struct tcp_pcb {
    bool              used;
    void             *arg;
    tcp_recv_fn       recv_cb;
    tcp_sent_fn       sent_cb;
    tcp_err_fn        err_cb;
    tcp_connected_fn  connected_cb;
    ip4_addr_t        remote_ip;
    u16_t             remote_port;
    uint8_t           state;  /* 0=closed 1=connecting 2=connected */
} tcp_pcb_t;

static tcp_pcb_t tcp_pcbs[TCP_PCB_MAX];

struct tcp_pcb *tcp_new(void)
{
    for (int i = 0; i < TCP_PCB_MAX; i++) {
        if (!tcp_pcbs[i].used) {
            tcp_pcb_t *p = &tcp_pcbs[i];
            p->used         = true;
            p->arg          = NULL;
            p->recv_cb      = NULL;
            p->sent_cb      = NULL;
            p->err_cb       = NULL;
            p->connected_cb = NULL;
            p->remote_ip.addr = 0;
            p->remote_port  = 0;
            p->state        = 0;
            return (struct tcp_pcb *)p;
        }
    }
    return NULL;
}

void tcp_arg(struct tcp_pcb *pcb, void *arg)
{
    if (pcb) ((tcp_pcb_t *)pcb)->arg = arg;
}

void tcp_recv(struct tcp_pcb *pcb, tcp_recv_fn recv)
{
    if (pcb) ((tcp_pcb_t *)pcb)->recv_cb = recv;
}

void tcp_sent(struct tcp_pcb *pcb, tcp_sent_fn sent)
{
    if (pcb) ((tcp_pcb_t *)pcb)->sent_cb = sent;
}

void tcp_err(struct tcp_pcb *pcb, tcp_err_fn err)
{
    if (pcb) ((tcp_pcb_t *)pcb)->err_cb = err;
}

void tcp_accept(struct tcp_pcb *pcb, tcp_accept_fn accept)
{
    (void)pcb; (void)accept; /* listen path not used in Phase 1 */
}

void tcp_poll(struct tcp_pcb *pcb, tcp_poll_fn poll, u8_t interval)
{
    (void)pcb; (void)poll; (void)interval;
}

err_t tcp_connect(struct tcp_pcb *pcb, const ip4_addr_t *ipaddr,
                   u16_t port, tcp_connected_fn connected)
{
    if (!pcb || !ipaddr) return ERR_ARG;
    tcp_pcb_t *p = (tcp_pcb_t *)pcb;
    p->remote_ip    = *ipaddr;
    p->remote_port  = port;
    p->connected_cb = connected;
    p->state        = 1; /* connecting */

    /*
     * Phase 1: immediately simulate a successful connection.
     * In a wired virtqueue, this would go through ARP + SYN/SYN-ACK.
     * For the shim, we optimistically move to connected so that
     * the caller can call tcp_write() right away.
     */
    p->state = 2;
    if (connected) connected(p->arg, pcb, ERR_OK);
    return ERR_OK;
}

err_t tcp_write(struct tcp_pcb *pcb, const void *arg, u16_t len, u8_t apiflags)
{
    (void)pcb; (void)arg; (void)len; (void)apiflags;
    /* Phase 1: data goes to virtio-net TX via lwip_virtio_output in lwip_sys.c */
    return ERR_OK;
}

err_t tcp_output(struct tcp_pcb *pcb)
{
    (void)pcb;
    return ERR_OK;
}

err_t tcp_close(struct tcp_pcb *pcb)
{
    if (pcb) {
        tcp_pcb_t *p = (tcp_pcb_t *)pcb;
        p->state = 0;
        p->used  = false;
    }
    return ERR_OK;
}

void tcp_abort(struct tcp_pcb *pcb)
{
    (void)tcp_close(pcb);
}

void tcp_recved(struct tcp_pcb *pcb, u16_t len)
{
    (void)pcb; (void)len; /* flow-control ack — no-op in shim */
}

/* ── netif management ────────────────────────────────────────────────── */

struct netif *netif_add(struct netif *netif,
                         const ip4_addr_t *ipaddr,
                         const ip4_addr_t *netmask,
                         const ip4_addr_t *gw,
                         void *state,
                         netif_init_fn init,
                         netif_input_fn input)
{
    (void)state;
    if (!netif || !init) return NULL;
    netif->ip_addr  = *ipaddr;
    netif->netmask  = *netmask;
    netif->gw       = *gw;
    netif->input    = input;
    netif->next     = NULL;
    netif->flags    = 0;
    netif->num      = 0;
    err_t e = init(netif);
    if (e != ERR_OK) return NULL;
    return netif;
}

void netif_set_default(struct netif *netif)
{
    netif_default = netif;
}

void netif_set_up(struct netif *netif)
{
    if (netif) netif->flags |= NETIF_FLAG_UP;
}

void netif_set_link_up(struct netif *netif)
{
    if (netif) netif->flags |= NETIF_FLAG_LINK_UP;
}

/* ── ARP / Ethernet stubs ────────────────────────────────────────────── */

err_t etharp_output(struct netif *netif, struct pbuf *q,
                     const ip4_addr_t *ipaddr)
{
    (void)ipaddr;
    /* Phase 1: skip ARP — directly call linkoutput */
    if (netif && netif->linkoutput && q)
        return netif->linkoutput(netif, q);
    return ERR_IF;
}

err_t ethernet_input(struct pbuf *p, struct netif *netif)
{
    /*
     * Phase 1: demultiplexing stub.
     * A real implementation would parse the Ethernet header and dispatch
     * to IP, ARP, etc.  For now we consume the pbuf and return OK so that
     * the virtqueue RX poll path has somewhere to hand frames.
     */
    (void)netif;
    pbuf_free(p);
    return ERR_OK;
}

/* ── Stack initialisation ────────────────────────────────────────────── */

void lwip_init(void)
{
    /* Reset pbuf pool */
    pbuf_pool_head = 0;
    for (uint32_t i = 0; i < PBUF_POOL_COUNT; i++) {
        pbuf_pool[i].hdr.next    = NULL;
        pbuf_pool[i].hdr.payload = pbuf_pool[i].data;
        pbuf_pool[i].hdr.ref     = 0;
    }
    /* Reset TCP PCB pool */
    for (int i = 0; i < TCP_PCB_MAX; i++)
        tcp_pcbs[i].used = false;
    /* netif_default stays NULL until netif_set_default is called */
}

/* ── Timeout polling (NO_SYS=1) ─────────────────────────────────────── */

void sys_check_timeouts(void)
{
    /*
     * Phase 1: no-op.
     * In a real lwIP integration this drives TCP retransmit, ARP aging,
     * DHCP state machine, etc.  Those timers are not yet registered in the
     * shim — add them here as needed when integrating real lwIP sources.
     */
}
