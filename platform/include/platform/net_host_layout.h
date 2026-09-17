/*
 * Host virtio-net ownership and agentOS network bridge layout.
 *
 * QEMU bus.16 is host hardware. Only net_pd receives its MMIO and DMA
 * mappings. Each VMM maps only its own queue page. net_pd maps only the driver
 * transfer page; net_virt alone maps both tiers. VMMs never map NET_HOST_DMA.
 */
#ifndef AOS_PLATFORM_NET_HOST_LAYOUT_H
#define AOS_PLATFORM_NET_HOST_LAYOUT_H

#include <stdint.h>
#include <platform/net_layout.h>

#define AGENTOS_HOST_NET_MMIO_PA          0x0A002000UL
#define AGENTOS_HOST_NET_MMIO_VA          0x06200000UL

/*
 * Keep this above the secondary VMM image reservation (0x20000000-0x22000000),
 * block-service shared window (0x22000000-0x22200000), and net_pd's private
 * host-DMA window (0x24000000-0x24200000). Frames retain their region-relative
 * VA in every authorized VSpace; unauthorized pages remain unmapped.
 */
#define AGENTOS_NET_SHARED_VA             0x26000000UL
#define AGENTOS_NET_SHARED_SIZE           AOS_NET_SHMEM_SIZE

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

/* Root-provisioned metadata version 2 selects modern PCI. Version 1 retains
 * the ARM MMIO binding. This record is outside the DMA queue/data windows. */
#define AOS_NET_PCI_INFO_OFF           0x40u
#define AOS_NET_PCI_INFO_MAGIC         0x504e4f41u
#define AOS_NET_PCI_REGION_VA(index)   (0x06200000UL + (index) * 0x1000UL)
typedef struct {
    uint32_t magic, version;
    uint32_t offset[3]; /* common, notification, device configuration */
    uint32_t length[3];
    uint32_t notify_multiplier;
    uint32_t reserved;
} aos_net_pci_info_t;
_Static_assert(AOS_NET_PCI_INFO_OFF + sizeof(aos_net_pci_info_t) <
               AGENTOS_NET_HOST_RX_DESC_OFF, "PCI metadata precedes queues");

#endif /* AOS_PLATFORM_NET_HOST_LAYOUT_H */
