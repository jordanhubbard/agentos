#ifndef AOS_PLATFORM_RAMFB_MMIO_H
#define AOS_PLATFORM_RAMFB_MMIO_H
#include <platform/ramfb.h>

#define AOS_RAMFB_DMA_BYTES 64u
enum aos_ramfb_transport_state {
    AOS_RAMFB_UNINITIALIZED, AOS_RAMFB_READY,
    AOS_RAMFB_INFLIGHT, AOS_RAMFB_FAILED
};
typedef struct aos_ramfb_mmio {
    volatile uint8_t *registers;
    volatile uint8_t *dma;
    uint64_t dma_physical;
    uint32_t poll_limit, polls_left;
    enum aos_ramfb_transport_state state;
} aos_ramfb_mmio_t;

/* Zero-initialize context once. Both mappings must be uncached and owned by
 * this driver. DMA storage must remain mapped and allocated for the device's
 * lifetime, including after timeout. No reset/reuse operation is provided.
 * The DMA region is contiguous, 64-byte aligned and at least DMA_BYTES long.
 * Register mapping covers the standard 24-byte fw_cfg MMIO window. */
int aos_ramfb_mmio_init(aos_ramfb_mmio_t *context, volatile void *registers,
                        volatile void *dma, uint64_t dma_physical,
                        uint32_t poll_limit, aos_ramfb_io_t *io);
/* Driver-only bounded DMA state machine. Poll returns 0 complete, 1 pending,
 * -1 failed. Timeout and device error permanently poison the transport. */
int aos_ramfb_dma_begin(aos_ramfb_mmio_t *context, uint16_t selector,
                        const uint8_t *bytes, size_t count);
int aos_ramfb_dma_poll(aos_ramfb_mmio_t *context);
#endif
