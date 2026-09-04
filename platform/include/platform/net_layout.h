/*
 * agentOS net_virt queue ABI
 *
 * Binary-compatible with sDDF net_queue_t / net_buff_desc_t (capacity 32,
 * 2048-byte buffers). This header must not include seL4 or sddf headers so
 * host tests can compile it.
 *
 * Guest IPA 0x0A010000 is the emulated virtio-mmio net device (faults to the
 * VMM). QEMU's virtio-mmio page at 0x0A000000 is a kill-dated crutch.
 */

#ifndef AOS_PLATFORM_NET_LAYOUT_H
#define AOS_PLATFORM_NET_LAYOUT_H

#include <stdint.h>

#define AOS_NET_BUFFER_SIZE          2048u
#define AOS_NET_CAPACITY             32u
#define AOS_NET_QUEUE_BYTES          0x1000u
#define AOS_NET_MAX_CLIENTS          4u
#define AOS_NET_GUEST_CLIENTS        2u
#define AOS_NET_CLIENT_STRIDE        0x80000u   /* 512 KB per client */
#define AOS_NET_SHMEM_SIZE           0x200000u  /* 2 MB */
#define AOS_NET_SHMEM_VA             0x20000000UL

#define AOS_NET_RX_FREE_OFF          0x0000u
#define AOS_NET_RX_ACTIVE_OFF        0x1000u
#define AOS_NET_TX_FREE_OFF          0x2000u
#define AOS_NET_TX_ACTIVE_OFF        0x3000u
#define AOS_NET_RX_DATA_OFF          0x4000u
#define AOS_NET_RX_DATA_BYTES        (AOS_NET_CAPACITY * AOS_NET_BUFFER_SIZE)
#define AOS_NET_TX_DATA_OFF          (AOS_NET_RX_DATA_OFF + AOS_NET_RX_DATA_BYTES)
#define AOS_NET_TX_DATA_BYTES        (AOS_NET_CAPACITY * AOS_NET_BUFFER_SIZE)

/* Emulated virtio-mmio net — must NOT overlap QEMU's 0x0A000000 page. */
#define AOS_VIRTIO_NET_GUEST_IPA     0x0A010000UL
#define AOS_VIRTIO_NET_MMIO_SIZE     0x1000UL
#define AOS_VIRTIO_NET_VIRQ          50u        /* GIC SPI 18 */
#define AOS_VIRTIO_NET_DTB_SPI       18u

#define AOS_VIRTIO_NET_MAC0          0x02u
#define AOS_VIRTIO_NET_MAC1          0x00u
#define AOS_VIRTIO_NET_MAC2          0x00u
#define AOS_VIRTIO_NET_MAC3          0x00u
#define AOS_VIRTIO_NET_MAC4          0x00u
#define AOS_VIRTIO_NET_MAC5          0x01u

/*
 * Match sDDF net_buff_desc_t: uint64 offset + uint16 len, 8-byte aligned
 * (sizeof 16). Do not pack.
 */
typedef struct aos_net_buff_desc {
    uint64_t io_or_offset;
    uint16_t len;
    uint16_t _pad0;
    uint32_t _pad1;
} aos_net_buff_desc_t;

typedef struct aos_net_queue {
    uint16_t tail;
    uint16_t head;
    uint32_t consumer_signalled;
    aos_net_buff_desc_t buffers[];
} aos_net_queue_t;

typedef struct aos_net_virt_client {
    aos_net_queue_t *rx_free;
    aos_net_queue_t *rx_active;
    aos_net_queue_t *tx_free;
    aos_net_queue_t *tx_active;
    uint8_t         *rx_data;
    uint8_t         *tx_data;
    uint32_t         capacity;
} aos_net_virt_client_t;

typedef struct aos_net_virt {
    aos_net_virt_client_t clients[AOS_NET_MAX_CLIENTS];
    uint32_t num_clients;
} aos_net_virt_t;

#endif /* AOS_PLATFORM_NET_LAYOUT_H */
