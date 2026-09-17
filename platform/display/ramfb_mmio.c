#include <platform/ramfb_mmio.h>

static uint16_t big16(uint16_t value)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap16(value);
#else
    return value;
#endif
}
static uint32_t big32(uint32_t value)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return __builtin_bswap32(value);
#else
    return value;
#endif
}
static void device_barrier(void)
{
#if defined(__aarch64__)
    __asm__ volatile("dsb sy" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}
static void store32(volatile uint8_t *p, uint32_t value)
{
    *(volatile uint32_t *)p = big32(value);
}
static int select_item(void *arg, uint16_t selector)
{
    aos_ramfb_mmio_t *c = arg;
    if (c->state != AOS_RAMFB_READY) return -1;
    *(volatile uint16_t *)(c->registers + 8) = big16(selector);
    device_barrier();
    return 0;
}
static int read_item(void *arg, uint8_t *out, size_t count)
{
    aos_ramfb_mmio_t *c = arg;
    if (c->state != AOS_RAMFB_READY || !out || count > 64) return -1;
    for (size_t i = 0; i < count; ++i) out[i] = c->registers[0];
    device_barrier();
    return 0;
}
int aos_ramfb_dma_begin(aos_ramfb_mmio_t *c, uint16_t selector,
                        const uint8_t *bytes, size_t count)
{
    if (!c || c->state != AOS_RAMFB_READY || !bytes ||
        count != AOS_RAMFB_CONFIG_BYTES || selector < 0x20 || selector >= 0x4000)
        return -1;
    /* Copy first: the caller may provide stack storage. Only the private
     * uncached DMA window is ever visible to the device. */
    for (size_t i = 0; i < count; ++i) c->dma[16+i] = bytes[i];
    store32(c->dma + 4, (uint32_t)count);
    store32(c->dma + 8, (uint32_t)((c->dma_physical + 16) >> 32));
    store32(c->dma + 12, (uint32_t)(c->dma_physical + 16));
    store32(c->dma, ((uint32_t)selector << 16) | 0x18u); /* SELECT | WRITE */
    c->polls_left = c->poll_limit;
    c->state = AOS_RAMFB_INFLIGHT;
    device_barrier();
    /* Low half triggers the operation; always write the high half first. */
    store32(c->registers + 16, (uint32_t)(c->dma_physical >> 32));
    store32(c->registers + 20, (uint32_t)c->dma_physical);
    device_barrier();
    return 0;
}
int aos_ramfb_dma_poll(aos_ramfb_mmio_t *c)
{
    if (!c || c->state != AOS_RAMFB_INFLIGHT) return -1;
    device_barrier();
    const uint32_t control = big32(*(volatile uint32_t *)c->dma);
    if (control == 0) {
        c->state = AOS_RAMFB_READY;
        return 0;
    }
    if ((control & 1u) || --c->polls_left == 0) {
        c->state = AOS_RAMFB_FAILED;
        return -1;
    }
    return 1;
}
static int write_item(void *arg, uint16_t selector, const uint8_t *p, size_t n)
{
    aos_ramfb_mmio_t *c = arg;
    if (aos_ramfb_dma_begin(c, selector, p, n) != 0) return -1;
    int result;
    do { result = aos_ramfb_dma_poll(c); } while (result > 0);
    return result;
}
int aos_ramfb_mmio_init(aos_ramfb_mmio_t *c, volatile void *registers,
                        volatile void *dma, uint64_t physical,
                        uint32_t poll_limit, aos_ramfb_io_t *io)
{
    if (!c || c->state != AOS_RAMFB_UNINITIALIZED || !registers || !dma ||
        !io || ((uintptr_t)registers & 7u) || ((uintptr_t)dma & 63u) ||
        !physical || (physical & 63u) || physical > UINT64_MAX - AOS_RAMFB_DMA_BYTES ||
        !poll_limit || poll_limit > 1000000u) return -1;
    c->registers = registers; c->dma = dma; c->dma_physical = physical;
    c->poll_limit = poll_limit; c->polls_left = 0;
    c->state = AOS_RAMFB_READY;
    *io = (aos_ramfb_io_t){c, select_item, read_item, write_item};
    return 0;
}
