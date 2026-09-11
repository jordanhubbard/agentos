#ifndef AOS_PLATFORM_NET_VIRT_PUMP_H
#define AOS_PLATFORM_NET_VIRT_PUMP_H

#include <platform/net_layout.h>

/*
 * Raw ring ops on the sDDF-compatible queues.  Safe across PDs: fenced, and
 * the head/tail indices are accessed as volatile.  Used by the VMM-side
 * emulated device (through libvmm), by net_virt, and by the host tests.
 */
uint16_t aos_net_queue_length(const aos_net_queue_t *q);
int aos_net_queue_dequeue(aos_net_queue_t *q, uint32_t capacity,
                          aos_net_buff_desc_t *out);
int aos_net_queue_enqueue(aos_net_queue_t *q, uint32_t capacity,
                          aos_net_buff_desc_t buf);

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
