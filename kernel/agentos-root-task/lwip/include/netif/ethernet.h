/*
 * netif/ethernet.h — Ethernet frame input handler shim
 */
#ifndef NETIF_ETHERNET_H
#define NETIF_ETHERNET_H

#include "lwip/err.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"

/* Demultiplex an incoming Ethernet frame.  Registered as netif->input. */
err_t ethernet_input(struct pbuf *p, struct netif *netif);

#endif /* NETIF_ETHERNET_H */
