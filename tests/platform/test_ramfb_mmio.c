#include <platform/ramfb_mmio.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    _Alignas(64) uint8_t registers[64] = {0}, dma[64] = {0};
    aos_ramfb_mmio_t c = {0};
    aos_ramfb_io_t io;
    assert(aos_ramfb_mmio_init(&c, registers, dma, 0x12345678000ULL, 3, &io) == 0);
    assert(io.select(io.context, 0x1234) == 0);
    assert(registers[8] == 0x12 && registers[9] == 0x34);
    registers[0] = 0xa5;
    uint8_t readback[4];
    assert(io.read(io.context, readback, sizeof readback) == 0);
    for (unsigned i = 0; i < sizeof readback; ++i) assert(readback[i] == 0xa5);

    uint8_t payload[28]; memset(payload, 0xa6, sizeof payload);
    assert(aos_ramfb_dma_begin(&c, 0x42, payload, sizeof payload) == 0);
    const uint8_t descriptor[16] = {
        0,0x42,0,0x18, 0,0,0,28, 0,0,1,0x23, 0x45,0x67,0x80,0x10
    };
    const uint8_t address[8] = {0,0,1,0x23,0x45,0x67,0x80,0};
    assert(memcmp(dma, descriptor, sizeof descriptor) == 0);
    assert(memcmp(registers+16, address, sizeof address) == 0);
    memset(payload, 0, sizeof payload);
    for (unsigned i = 16; i < 44; ++i) assert(dma[i] == 0xa6);
    assert(io.select(io.context, 0) == -1);
    assert(aos_ramfb_dma_begin(&c, 0x42, payload, sizeof payload) == -1);
    assert(aos_ramfb_dma_poll(&c) == 1);
    memset(dma, 0, 4); /* simulated device completion */
    assert(aos_ramfb_dma_poll(&c) == 0 && c.state == AOS_RAMFB_READY);

    assert(aos_ramfb_dma_begin(&c, 0x42, payload, sizeof payload) == 0);
    assert(aos_ramfb_dma_poll(&c) == 1);
    assert(aos_ramfb_dma_poll(&c) == 1);
    assert(aos_ramfb_dma_poll(&c) == -1 && c.state == AOS_RAMFB_FAILED);
    uint8_t retained[64]; memcpy(retained, dma, sizeof retained);
    assert(io.write_dma(io.context, 0x42, payload, sizeof payload) == -1);
    assert(io.read(io.context, readback, sizeof readback) == -1);
    assert(io.select(io.context, 0) == -1);
    assert(aos_ramfb_mmio_init(&c, registers, dma, 0x12345678000ULL, 3, &io) == -1);
    assert(memcmp(dma, retained, sizeof retained) == 0);

    _Alignas(64) uint8_t error_regs[64] = {0}, error_dma[64] = {0};
    aos_ramfb_mmio_t error = {0};
    assert(aos_ramfb_mmio_init(&error, error_regs, error_dma, 0x40000000, 2, &io) == 0);
    assert(aos_ramfb_dma_begin(&error, 0x42, payload, sizeof payload) == 0);
    error_dma[3] |= 1; /* big-endian error bit */
    assert(aos_ramfb_dma_poll(&error) == -1 && error.state == AOS_RAMFB_FAILED);

    _Alignas(64) uint8_t bounded_regs[64] = {0}, bounded_dma[64] = {0};
    aos_ramfb_mmio_t bounded = {0};
    assert(aos_ramfb_mmio_init(&bounded, bounded_regs, bounded_dma, 0x50000000, 1, &io) == 0);
    assert(io.write_dma(io.context, 0x42, payload, sizeof payload) == -1);
    assert(bounded.state == AOS_RAMFB_FAILED);
    aos_ramfb_mmio_t bad = {0};
    assert(aos_ramfb_mmio_init(&bad, registers, dma+1, 0x40000000, 1, &io) == -1);
    assert(aos_ramfb_mmio_init(&bad, registers, dma, UINT64_MAX-63, 1, &io) == -1);
    assert(aos_ramfb_mmio_init(&bad, registers, dma, 0x40000000, 0, &io) == -1);
    assert(bad.state == AOS_RAMFB_UNINITIALIZED);
    puts("ramfb MMIO: exact DMA descriptor, completion, timeout and retained storage passed");
    return 0;
}
