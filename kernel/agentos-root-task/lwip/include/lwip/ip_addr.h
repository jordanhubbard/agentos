/*
 * lwip/ip_addr.h — unified IP address type shim (IPv4 only for Phase 1)
 */
#ifndef LWIP_IP_ADDR_H
#define LWIP_IP_ADDR_H

#include "lwip/ip4_addr.h"

typedef ip4_addr_t ip_addr_t;

#define IPADDR_TYPE_V4  0
#define ip_addr_copy(dest, src)  ((dest).addr = (src).addr)

#endif /* LWIP_IP_ADDR_H */
