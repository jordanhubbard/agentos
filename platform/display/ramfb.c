#include <platform/ramfb.h>

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (24 - 8*i));
}

static int item(const aos_ramfb_io_t *io, uint16_t key, uint8_t *p, size_t n)
{
    return io->select(io->context, key) == 0 &&
           io->read(io->context, p, n) == 0;
}

int aos_ramfb_configure(const aos_ramfb_io_t *io, uint64_t physical,
                        size_t capacity, uint32_t width, uint32_t height,
                        uint32_t stride)
{
    if (!io || !io->select || !io->read || !io->write_dma ||
        !physical || (physical & 3u) ||
        width < AOS_RAMFB_MIN_DIMENSION || height < AOS_RAMFB_MIN_DIMENSION ||
        width > AOS_RAMFB_MAX_WIDTH || height > AOS_RAMFB_MAX_HEIGHT ||
        stride < width * 4u || (stride & 3u)) return AOS_RAMFB_BAD_ARGUMENT;
    const uint64_t bytes = (uint64_t)stride * height;
    if (bytes > capacity || bytes > UINT64_MAX - physical)
        return AOS_RAMFB_BAD_ARGUMENT;

    uint8_t word[4];
    if (!item(io, 0, word, sizeof word)) return AOS_RAMFB_IO_ERROR;
    if (word[0] != 'Q' || word[1] != 'E' || word[2] != 'M' || word[3] != 'U')
        return AOS_RAMFB_NO_DEVICE;
    if (!item(io, 1, word, sizeof word)) return AOS_RAMFB_IO_ERROR;
    /* FW_CFG_ID is little-endian, unlike the directory and DMA payload. */
    if (!(word[0] & 2u)) return AOS_RAMFB_NO_DMA;
    if (!item(io, 0x19, word, sizeof word)) return AOS_RAMFB_IO_ERROR;
    const uint32_t count = be32(word);
    if (count > AOS_RAMFB_MAX_FILES) return AOS_RAMFB_BAD_DIRECTORY;
    uint16_t selector = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t record[64];
        if (io->read(io->context, record, sizeof record) != 0)
            return AOS_RAMFB_IO_ERROR;
        static const uint8_t name[] = "etc/ramfb";
        unsigned j = 0;
        while (j < sizeof name && record[8+j] == name[j]) ++j;
        if (j != sizeof name) continue;
        const uint16_t key = (uint16_t)((uint16_t)record[4] << 8 | record[5]);
        if (selector || be32(record) != AOS_RAMFB_CONFIG_BYTES ||
            key < 0x20 || key >= 0x4000 || record[6] || record[7])
            return AOS_RAMFB_BAD_DIRECTORY;
        selector = key;
    }
    if (!selector) return AOS_RAMFB_NO_DEVICE;
    uint8_t config[AOS_RAMFB_CONFIG_BYTES] = {0};
    put32(config, (uint32_t)(physical >> 32));
    put32(config + 4, (uint32_t)physical);
    put32(config + 8, 0x34325258u); /* DRM_FORMAT_XRGB8888 ('XR24'). */
    put32(config + 16, width);
    put32(config + 20, height);
    put32(config + 24, stride);
    return io->write_dma(io->context, selector, config, sizeof config) == 0
         ? AOS_RAMFB_OK : AOS_RAMFB_IO_ERROR;
}
