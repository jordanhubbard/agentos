/*
 * Host virtio-net ownership and agentOS network bridge layout.
 *
 * QEMU bus.16 is host hardware. Only net_pd receives its MMIO and DMA
 * mappings. linux_vmm shares NET_SHARED with net_pd for contract payloads and
 * guest-facing sDDF queues, but never maps NET_HOST_DMA.
 */
#ifndef AOS_PLATFORM_NET_HOST_LAYOUT_H
#define AOS_PLATFORM_NET_HOST_LAYOUT_H

#include <stdint.h>

#define AGENTOS_HOST_NET_MMIO_PA          0x0A002000UL
#define AGENTOS_HOST_NET_MMIO_VA          0x06200000UL

#define AGENTOS_NET_SHARED_VA             0x20000000UL
#define AGENTOS_NET_SHARED_SIZE           0x00200000UL

#define AGENTOS_NET_HOST_DMA_VA           0x24000000UL
#define AGENTOS_NET_HOST_DMA_SIZE         0x00200000UL
#define AGENTOS_NET_HOST_DMA_MAGIC        0x414F534Eu /* "AOSN" */
#define AGENTOS_NET_HOST_DMA_VERSION      1u

#define AGENTOS_NET_HOST_QUEUE_SIZE       32u
#define AGENTOS_NET_HOST_HEADER_SIZE      10u
#define AGENTOS_NET_HOST_BUFFER_SIZE      2048u

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
