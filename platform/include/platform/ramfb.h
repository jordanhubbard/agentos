/* QEMU ramfb device configuration. Only the display driver may invoke this
 * interface with root-provisioned DMA memory; no guest addresses are accepted.
 * The transport owns its DMA descriptor, cache maintenance and bounded wait.
 */
#ifndef AOS_PLATFORM_RAMFB_H
#define AOS_PLATFORM_RAMFB_H
#include <stddef.h>
#include <stdint.h>

#define AOS_RAMFB_CONFIG_BYTES 28u
#define AOS_RAMFB_MAX_FILES 256u
#define AOS_RAMFB_MIN_DIMENSION 16u
#define AOS_RAMFB_MAX_WIDTH 1024u
#define AOS_RAMFB_MAX_HEIGHT 768u

typedef struct aos_ramfb_io {
    void *context;
    /* Host-endian selector; the MMIO adapter performs endian conversion. */
    int (*select)(void *context, uint16_t selector);
    int (*read)(void *context, uint8_t *bytes, size_t count);
    /* Copy bytes into private DMA storage, then select+write. Success means
     * completion. On timeout retain that storage until device shutdown;
     * never retain this caller's pointer after returning. */
    int (*write_dma)(void *context, uint16_t selector,
                     const uint8_t *bytes, size_t count);
} aos_ramfb_io_t;

enum aos_ramfb_result {
    AOS_RAMFB_OK = 0, AOS_RAMFB_BAD_ARGUMENT, AOS_RAMFB_NO_DEVICE,
    AOS_RAMFB_NO_DMA, AOS_RAMFB_BAD_DIRECTORY, AOS_RAMFB_IO_ERROR
};

/* Configure tightly bounded XRGB8888 scanout. capacity is the contiguous,
 * exclusively owned DMA allocation size, not a producer-supplied length.
 * No heap allocation; reads at most MAX_FILES directory records. */
int aos_ramfb_configure(const aos_ramfb_io_t *io, uint64_t physical,
                        size_t capacity, uint32_t width, uint32_t height,
                        uint32_t stride);
#endif
