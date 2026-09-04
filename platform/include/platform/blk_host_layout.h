/*
 * Host virtio-blk ownership and shared-DMA ABI.
 *
 * QEMU bus.8 models host hardware. Only the agentOS virtio_blk PD receives
 * its MMIO capability; linux_vmm reaches it through IPC and this shared frame.
 */
#ifndef AOS_PLATFORM_BLK_HOST_LAYOUT_H
#define AOS_PLATFORM_BLK_HOST_LAYOUT_H

#include <stdint.h>

#define AGENTOS_HOST_BLK_MMIO_PA         0x0A001000UL
#define AGENTOS_HOST_BLK_MMIO_VA         0x06000000UL

#define AGENTOS_BLK_SHARED_VA            0x22000000UL
#define AGENTOS_BLK_SHARED_SIZE          0x00200000UL
#define AGENTOS_BLK_SHARED_MAGIC         0x414F5342u /* "AOSB" */
#define AGENTOS_BLK_SHARED_META_OFF      0x0000u
#define AGENTOS_BLK_SHARED_QUEUE_OFF     0x1000u
#define AGENTOS_BLK_SHARED_DMA_OFF       0x2000u
#define AGENTOS_BLK_SHARED_DMA_SIZE      0x8000u
#define AGENTOS_BLK_SHARED_DMA_DATA_OFF  16u
#define AGENTOS_BLK_SHARED_DMA_MAX_SECTORS 63u
#define AGENTOS_BLK_SHARED_DMA_DATA_SIZE \
    (AGENTOS_BLK_SHARED_DMA_MAX_SECTORS * 512u)

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint64_t paddr;
    uint64_t size;
} agentos_blk_shared_meta_t;

#define AOS_HOST_BLK_SECTOR_SIZE         512u
#define AOS_HOST_BLK_OP_READ             0xF0u
#define AOS_HOST_BLK_OP_WRITE            0xF1u
#define AOS_HOST_BLK_OP_FLUSH            0xF2u
#define AOS_HOST_BLK_OP_INFO             0xF3u
#define AOS_HOST_BLK_OP_HEALTH           0xF4u

#define AOS_HOST_BLK_OK                  0u
#define AOS_HOST_BLK_ERR_IO              1u
#define AOS_HOST_BLK_ERR_OOB             2u
#define AOS_HOST_BLK_ERR_NODEV           3u

#endif /* AOS_PLATFORM_BLK_HOST_LAYOUT_H */
