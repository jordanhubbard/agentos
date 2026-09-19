/*
 * Host virtio-blk ownership and shared-DMA ABI.
 *
 * QEMU buses 8 and 31 model host hardware. Only the canonical agentOS
 * virtio_blk PD receives their MMIO capabilities; VMMs reach selected media
 * through IPC and this shared frame.
 */
#ifndef AOS_PLATFORM_BLK_HOST_LAYOUT_H
#define AOS_PLATFORM_BLK_HOST_LAYOUT_H

#include <stdint.h>
#include <stdbool.h>

#define AGENTOS_HOST_BLK_MMIO_PA         0x0A001000UL
#define AGENTOS_HOST_BLK_MMIO_VA         0x06000000UL
#define AGENTOS_HOST_SECONDARY_BLK_PAGE_PA 0x0A003000UL
#define AGENTOS_HOST_SECONDARY_BLK_PAGE_VA 0x06001000UL
#define AGENTOS_HOST_SECONDARY_BLK_PAGE_OFF 0x00000E00UL

#define AGENTOS_BLK_SHARED_VA            0x22000000UL
#define AGENTOS_BLK_SHARED_SIZE          0x00200000UL
#define AGENTOS_BLK_SHARED_MAGIC         0x414F5342u /* "AOSB" */
#define AGENTOS_BLK_SHARED_META_OFF      0x0000u
#define AGENTOS_BLK_MEDIA_QUEUE_OFF(id)  \
    ((uint32_t)(id) == AOS_HOST_BLK_MEDIA_PRIMARY ? 0x1000u : 0x11000u)
#define AGENTOS_BLK_MEDIA_DMA_OFF(id)    \
    ((uint32_t)(id) == AOS_HOST_BLK_MEDIA_PRIMARY ? 0x100000u : 0x12000u)
#define AGENTOS_BLK_MEDIA_DMA_SIZE(id)   \
    ((uint32_t)(id) == AOS_HOST_BLK_MEDIA_PRIMARY ? 0x100000u : 0x8000u)
#define AGENTOS_BLK_MEDIA_DMA_MAX_SECTORS(id) \
    ((uint32_t)(id) == AOS_HOST_BLK_MEDIA_PRIMARY ? 2047u : 63u)
#define AGENTOS_BLK_SHARED_QUEUE_OFF     AGENTOS_BLK_MEDIA_QUEUE_OFF(0u)
#define AGENTOS_BLK_SHARED_DMA_OFF       AGENTOS_BLK_MEDIA_DMA_OFF(0u)
#define AGENTOS_BLK_SHARED_DMA_SIZE      AGENTOS_BLK_MEDIA_DMA_SIZE(0u)
#define AGENTOS_BLK_SHARED_DMA_DATA_OFF  16u
#define AGENTOS_BLK_SHARED_DMA_MAX_SECTORS \
    AGENTOS_BLK_MEDIA_DMA_MAX_SECTORS(0u)
#define AGENTOS_BLK_SHARED_DMA_DATA_SIZE \
    (AGENTOS_BLK_SHARED_DMA_MAX_SECTORS * 512u)

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint64_t paddr;
    uint64_t size;
} agentos_blk_shared_meta_t;

/* Metadata version 2 selects modern PCI for primary media. Version 1 keeps
 * the ARM MMIO layout. Root alone provisions this boot description; both
 * trusted storage PDs share the containing DMA frame. */
#define AOS_BLK_PCI_INFO_OFF             0x40u
#define AOS_BLK_PCI_INFO_MAGIC           0x50424f41u
#define AOS_BLK_PCI_REGION_VA(index)     (0x06000000UL + (index) * 0x1000UL)
#define AOS_BLK_PCI_MEDIA_REGION_VA(media, index) \
    (AOS_BLK_PCI_REGION_VA(index) + (media) * 0x4000UL)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t offset[3]; /* common, notification, device configuration */
    uint32_t length[3];
    uint32_t notify_multiplier;
    uint32_t reserved;
} aos_blk_pci_info_t;
/* Shared metadata version 3 carries one or two independent PCI media. The
 * count describes mapped hardware, not a guest-selected media identifier. */
typedef struct {
    uint32_t magic, version, count, reserved;
    aos_blk_pci_info_t media[2];
} aos_blk_pci_set_t;
static inline bool aos_blk_pci_info_valid(const aos_blk_pci_info_t *info)
{
    if (!info || info->magic != AOS_BLK_PCI_INFO_MAGIC ||
        info->version != 1u || info->reserved) return false;
    for (unsigned r = 0; r < 3; r++)
        if (info->offset[r] >= 4096u || !info->length[r] ||
            info->length[r] > 4096u - info->offset[r]) return false;
    return true;
}
static inline bool aos_blk_pci_set_valid(const aos_blk_pci_set_t *set)
{
    if (!set || set->magic != AOS_BLK_PCI_INFO_MAGIC || set->version != 2u ||
        !set->count || set->count > 2u || set->reserved) return false;
    for (unsigned i = 0; i < set->count; i++)
        if (!aos_blk_pci_info_valid(&set->media[i])) return false;
    return true;
}
_Static_assert(AOS_BLK_PCI_INFO_OFF + sizeof(aos_blk_pci_set_t) < 0x1000u,
               "PCI media descriptions precede queue memory");
_Static_assert(AOS_BLK_PCI_INFO_OFF + sizeof(aos_blk_pci_info_t) < 0x1000u,
               "PCI boot description precedes queue memory");

#define AOS_HOST_BLK_SECTOR_SIZE         512u
#define AOS_HOST_BLK_OP_READ             0xF0u
#define AOS_HOST_BLK_OP_WRITE            0xF1u
#define AOS_HOST_BLK_OP_FLUSH            0xF2u
#define AOS_HOST_BLK_OP_INFO             0xF3u
#define AOS_HOST_BLK_OP_HEALTH           0xF5u

/* OP_INFO flags returned after capacity and block size. */
#define AOS_HOST_BLK_INFO_READ_ONLY      (1u << 0)

#define AOS_HOST_BLK_MEDIA_PRIMARY        0u
#define AOS_HOST_BLK_MEDIA_SECONDARY       1u
#define AOS_HOST_BLK_MEDIA_COUNT         2u

#define AOS_HOST_BLK_OK                  0u
#define AOS_HOST_BLK_ERR_IO              1u
#define AOS_HOST_BLK_ERR_OOB             2u
#define AOS_HOST_BLK_ERR_NODEV           3u

#endif /* AOS_PLATFORM_BLK_HOST_LAYOUT_H */
