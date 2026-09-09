/*
 * Host virtio-net ownership and agentOS network bridge layout.
 *
 * QEMU bus.16 is host hardware. Only net_pd receives its MMIO and DMA
 * mappings. The guest VMM shares NET_SHARED with net_pd for contract payloads and
 * guest-facing sDDF queues, but never maps NET_HOST_DMA.
 */
#ifndef AOS_PLATFORM_NET_HOST_LAYOUT_H
#define AOS_PLATFORM_NET_HOST_LAYOUT_H

#include <stdint.h>

#define AGENTOS_HOST_NET_MMIO_PA          0x0A002000UL
#define AGENTOS_HOST_NET_MMIO_VA          0x06200000UL

/*
 * Keep this above the secondary VMM image reservation (0x20000000-0x22000000),
 * block-service shared window (0x22000000-0x22200000), and net_pd's private
 * host-DMA window (0x24000000-0x24200000).  Every client maps this same frame
 * at the same VA, so a collision would otherwise degrade into private-memory
 * traffic instead of a visible link failure.
 */
#define AGENTOS_NET_SHARED_VA             0x26000000UL
#define AGENTOS_NET_SHARED_SIZE           0x00200000UL

#define AGENTOS_NET_HOST_DMA_VA           0x24000000UL
#define AGENTOS_NET_HOST_DMA_SIZE         0x00200000UL
#define AGENTOS_NET_HOST_DMA_MAGIC        0x414F534Eu /* "AOSN" */
#define AGENTOS_NET_HOST_DMA_VERSION      1u

#define AGENTOS_NET_HOST_QUEUE_SIZE       32u
/* Modern virtio (VIRTIO_F_VERSION_1) always uses virtio_net_hdr_v1. */
#define AGENTOS_NET_HOST_HEADER_SIZE      12u
#define AGENTOS_NET_HOST_BUFFER_SIZE      2048u

/*
 * QEMU's user-mode network addresses every host-forwarded Ethernet frame to
 * the single physical stand-in MAC, even when the IPv4 destination belongs
 * to a virtual client behind net_pd.  The test-board policy assigns iface N
 * to 10.0.2.(15 + N); net_pd uses that immutable assignment to select one
 * client and rewrites only its destination MAC.  A real board can replace
 * this policy with hardware MAC filters or a native routing service.
 */
#define AGENTOS_NET_HOST_IPV4_CLIENT_BASE 0x0A00020Fu

#define AGENTOS_NET_HOST_META_OFF         0x0000u
#define AGENTOS_NET_HOST_RX_DESC_OFF      0x1000u
#define AGENTOS_NET_HOST_RX_AVAIL_OFF     0x1800u
#define AGENTOS_NET_HOST_RX_USED_OFF      0x2000u
#define AGENTOS_NET_HOST_TX_DESC_OFF      0x3000u
#define AGENTOS_NET_HOST_TX_AVAIL_OFF     0x3800u
#define AGENTOS_NET_HOST_TX_USED_OFF      0x4000u
#define AGENTOS_NET_HOST_RX_DATA_OFF      0x10000u
#define AGENTOS_NET_HOST_TX_DATA_OFF      0x20000u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint64_t paddr;
    uint64_t size;
} agentos_net_host_dma_meta_t;

#endif /* AOS_PLATFORM_NET_HOST_LAYOUT_H */
