/*
 * lwip/ip4_addr.h — IPv4 address type shim
 */
#ifndef LWIP_IP4_ADDR_H
#define LWIP_IP4_ADDR_H

#include "lwip/arch.h"

typedef struct ip4_addr {
    u32_t addr;  /* stored in network byte order */
} ip4_addr_t;

#define IP4_ADDR(ipaddr, a, b, c, d) \
    (ipaddr)->addr = (u32_t)((d) | ((c) << 8) | ((b) << 16) | ((a) << 24))

#define ip4_addr_get_u32(src_ipaddr)  ((src_ipaddr)->addr)

#endif /* LWIP_IP4_ADDR_H */
