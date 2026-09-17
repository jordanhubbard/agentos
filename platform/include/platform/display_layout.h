#ifndef AOS_PLATFORM_DISPLAY_LAYOUT_H
#define AOS_PLATFORM_DISPLAY_LAYOUT_H
#include <stdint.h>
#define AOS_DISPLAY_QUEUE_VA 0x2e000000UL
#define AOS_DISPLAY_DMA_VA 0x2e200000UL
#define AOS_DISPLAY_MMIO_VA 0x2e400000UL
#define AOS_DISPLAY_FWCFG_PA 0x09020000UL
#define AOS_DISPLAY_BANK_VA 0x38000000UL
#define AOS_DISPLAY_BANK_STRIDE 0x400000UL
#define AOS_DISPLAY_DMA_OFFSET 64u
#define AOS_DISPLAY_META_MAGIC 0x4453504cu
typedef struct aos_display_meta {
    uint32_t magic, version;
    uint64_t dma_physical, bank_physical[2], bank_bytes;
} aos_display_meta_t;
_Static_assert(sizeof(aos_display_meta_t)<=AOS_DISPLAY_DMA_OFFSET,"display metadata bound");
#endif
