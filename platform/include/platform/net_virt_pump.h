#ifndef AOS_PLATFORM_NET_VIRT_PUMP_H
#define AOS_PLATFORM_NET_VIRT_PUMP_H

#include <platform/net_layout.h>

void aos_net_virt_reset(aos_net_virt_t *v);

/* Bind client_index's stride inside region. Does not touch buffers. */
void aos_net_client_bind(uint8_t *region, uint32_t client_index,
                         aos_net_virt_client_t *out);

/* Zero queue pages, fill rx.free and tx.free with buffer offsets. */
void aos_net_client_init_buffers(aos_net_virt_client_t *c);

int aos_net_virt_add_client(aos_net_virt_t *v, const aos_net_virt_client_t *c);

/*
 * Move every pending TX active buffer to a destination RX:
 *   1 client  → loopback to self
 *   2+ clients → copy to every other client (hub)
 * Returns packets successfully forwarded.
 */
uint32_t aos_net_virt_pump(aos_net_virt_t *v);

#endif /* AOS_PLATFORM_NET_VIRT_PUMP_H */
