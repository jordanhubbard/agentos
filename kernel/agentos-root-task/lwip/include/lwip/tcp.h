/*
 * lwip/tcp.h — TCP PCB shim for agentOS lwIP-compatible layer
 */
#ifndef LWIP_TCP_H
#define LWIP_TCP_H

#include "lwip/arch.h"
#include "lwip/err.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"

#define TCP_WRITE_FLAG_COPY  0x01u
#define TCP_WRITE_FLAG_MORE  0x02u

struct tcp_pcb;

typedef err_t (*tcp_connected_fn)(void *arg, struct tcp_pcb *tpcb, err_t err);
typedef err_t (*tcp_recv_fn)(void *arg, struct tcp_pcb *tpcb,
                               struct pbuf *p, err_t err);
typedef err_t (*tcp_sent_fn)(void *arg, struct tcp_pcb *tpcb, u16_t len);
typedef void  (*tcp_err_fn)(void *arg, err_t err);
typedef err_t (*tcp_accept_fn)(void *arg, struct tcp_pcb *newpcb, err_t err);
typedef err_t (*tcp_poll_fn)(void *arg, struct tcp_pcb *tpcb);

struct tcp_pcb *tcp_new(void);
void            tcp_arg(struct tcp_pcb *pcb, void *arg);
void            tcp_recv(struct tcp_pcb *pcb, tcp_recv_fn recv);
void            tcp_sent(struct tcp_pcb *pcb, tcp_sent_fn sent);
void            tcp_err(struct tcp_pcb *pcb, tcp_err_fn err);
void            tcp_accept(struct tcp_pcb *pcb, tcp_accept_fn accept);
void            tcp_poll(struct tcp_pcb *pcb, tcp_poll_fn poll, u8_t interval);

err_t tcp_connect(struct tcp_pcb *pcb, const ip4_addr_t *ipaddr,
                   u16_t port, tcp_connected_fn connected);
err_t tcp_write(struct tcp_pcb *pcb, const void *arg, u16_t len, u8_t apiflags);
err_t tcp_output(struct tcp_pcb *pcb);
err_t tcp_close(struct tcp_pcb *pcb);
void  tcp_abort(struct tcp_pcb *pcb);
void  tcp_recved(struct tcp_pcb *pcb, u16_t len);

#endif /* LWIP_TCP_H */
