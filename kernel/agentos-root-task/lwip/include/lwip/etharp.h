/*
 * lwip/etharp.h — ARP shim for agentOS lwIP-compatible layer
 */
#ifndef LWIP_ETHARP_H
#define LWIP_ETHARP_H

#include "lwip/arch.h"
#include "lwip/err.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/ip4_addr.h"

/* Standard ARP output callback — registered as netif->output */
err_t etharp_output(struct netif *netif, struct pbuf *q,
                     const ip4_addr_t *ipaddr);

#endif /* LWIP_ETHARP_H */
