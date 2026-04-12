/*
 * lwip_sys.h — public interface for the agentOS lwIP system glue layer
 *
 * Included by net_server.c to call the lwIP integration functions.
 * All state is hidden in lwip_sys.c.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once

#include <stdint.h>
#include "lwip/netif.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"

/*
 * Initialise the lwIP stack and bring up the net_server netif.
 * Must be called once from net_server init() after virtio-net probe.
 *
 *   mmio_vaddr — virtual address of virtio-net MMIO region
 *   rx_desc    — virtual address of RX virtqueue descriptor table (0 if not set up yet)
 *   tx_desc    — virtual address of TX virtqueue descriptor table (0 if not set up yet)
 */
void net_server_lwip_init(uintptr_t mmio_vaddr, uintptr_t rx_desc,
                           uintptr_t tx_desc);

/*
 * Advance the lwIP tick counter by 10 ms.
 * Call from the net_server notified() handler on NET_CH_TIMER.
 */
void lwip_tick(void);

/*
 * Poll the virtio-net RX virtqueue and feed any received frames into lwIP.
 * Call from the net_server notified() handler on NET_CH_TIMER.
 */
void lwip_virtio_rx_poll(void);

/*
 * Open a TCP connection for the given vnic_id slot.
 *   dest_ip_be — destination IPv4 address in network (big-endian) byte order
 *   dest_port  — destination TCP port
 * Returns 0 on success, negative error code on failure.
 */
int lwip_net_connect(uint8_t vnic_id, uint32_t dest_ip_be, uint16_t dest_port);

/*
 * Send data over the TCP connection for vnic_id.
 * Returns bytes queued, or -1 on error.
 */
int lwip_net_send(uint8_t vnic_id, const uint8_t *data, uint16_t len);

/*
 * Copy up to max bytes of received data for vnic_id into dst.
 * Returns actual bytes copied (0 if nothing available).
 */
int lwip_net_recv(uint8_t vnic_id, uint8_t *dst, uint32_t max);

/*
 * Returns the connection state for vnic_id:
 *   0 = free/closed
 *   1 = connecting
 *   2 = connected
 *   3 = error/closed by peer
 */
uint8_t lwip_conn_state(uint8_t vnic_id);

/* The global lwIP netif (used by net_server.c for linkoutput) */
extern struct netif g_netif;

/* Connection table (accessed by net_server.c for OP_NET_TCP_CLOSE) */
typedef struct {
    struct tcp_pcb *tcp;
    uint8_t  state;      /* 0=free 1=connecting 2=connected 3=closed */
    /* rx_buf is 65536 bytes — large enough for full code-gen HTTP responses.
     * The OP_NET_HTTP_POST handler uses the last conn slot (HTTP_CONN_ID) and
     * depends on this buffer being able to hold a complete HTTP response body
     * (generated C source can be 40–64 KB). */
    uint8_t  rx_buf[65536];
    uint32_t rx_buf_len;  /* uint32 to hold up to 64 KB */
} vnic_conn_t;

extern vnic_conn_t g_conns[];

/* Static heap helpers (used if any PD-local lwIP code needs them) */
void *mem_malloc(size_t size);
void  mem_free(void *p);
void *mem_calloc(size_t n, size_t size);
