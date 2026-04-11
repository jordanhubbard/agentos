/*
 * lwip/netif.h — network interface shim for agentOS lwIP-compatible layer
 */
#ifndef LWIP_NETIF_H
#define LWIP_NETIF_H

#include "lwip/arch.h"
#include "lwip/err.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"

#define NETIF_MAX_HWADDR_LEN  6u

#define NETIF_FLAG_BROADCAST  (0x01u)
#define NETIF_FLAG_ETHARP     (0x08u)
#define NETIF_FLAG_LINK_UP    (0x04u)
#define NETIF_FLAG_UP         (0x02u)

struct netif;

typedef err_t (*netif_init_fn)(struct netif *netif);
typedef err_t (*netif_input_fn)(struct pbuf *p, struct netif *inp);
typedef err_t (*netif_output_fn)(struct netif *netif, struct pbuf *p,
                                  const ip4_addr_t *ipaddr);
typedef err_t (*netif_linkoutput_fn)(struct netif *netif, struct pbuf *p);

struct netif {
    struct netif          *next;
    ip4_addr_t             ip_addr;
    ip4_addr_t             netmask;
    ip4_addr_t             gw;
    netif_input_fn         input;
    netif_output_fn        output;
    netif_linkoutput_fn    linkoutput;
    u8_t                   hwaddr[NETIF_MAX_HWADDR_LEN];
    u8_t                   hwaddr_len;
    u8_t                   flags;
    u16_t                  mtu;
    char                   name[2];
    u8_t                   num;
};

struct netif *netif_add(struct netif *netif,
                         const ip4_addr_t *ipaddr,
                         const ip4_addr_t *netmask,
                         const ip4_addr_t *gw,
                         void *state,
                         netif_init_fn init,
                         netif_input_fn input);

void netif_set_default(struct netif *netif);
void netif_set_up(struct netif *netif);
void netif_set_link_up(struct netif *netif);

extern struct netif *netif_default;

#endif /* LWIP_NETIF_H */
