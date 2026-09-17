#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <platform/virtio_pci_caps.h>

static uint8_t config[256];
static uint64_t sizes[6];
static void put32(unsigned off, uint32_t value)
{
    for (unsigned i = 0; i < 4; i++) config[off + i] = (uint8_t)(value >> (i * 8u));
}
static void cap(unsigned off, unsigned next, unsigned type, unsigned offset)
{
    config[off] = 9;
    config[off + 1] = (uint8_t)next;
    config[off + 2] = type == 2u ? 20u : 16u;
    config[off + 3] = (uint8_t)type;
    config[off + 4] = 4;
    put32(off + 8, offset);
    put32(off + 12, 4096);
}
static void fixture(void)
{
    memset(config, 0, sizeof(config));
    memset(sizes, 0, sizeof(sizes));
    put32(0, 0x10421af4);
    config[6] = 0x10;
    config[0x34] = 0x40;
    put32(0x20, 0xfebc000c); /* prefetchable 64-bit BAR4 */
    sizes[4] = 0x4000;
    cap(0x40, 0x50, 1, 0);
    cap(0x50, 0x64, 2, 0x3000);
    put32(0x60, 4);
    cap(0x64, 0, 4, 0x2000);
}
static void rejected(void)
{
    aos_virtio_pci_layout_t out;
    memset(&out, 0xff, sizeof(out));
    assert(!aos_virtio_pci_decode(config, sizes, 0x1042, &out));
    for (unsigned i = 0; i < 3; i++) assert(!out.region[i].paddr && !out.region[i].length);
    assert(!out.notify_multiplier);
}
int main(void)
{
    aos_virtio_pci_layout_t out;
    fixture();
    assert(aos_virtio_pci_decode(config, sizes, 0x1042, &out));
    assert(out.region[0].paddr == 0xfebc0000 && out.region[0].length == 4096);
    assert(out.region[1].paddr == 0xfebc3000 && out.region[1].length == 4096);
    assert(out.region[2].paddr == 0xfebc2000 && out.region[2].length == 4096);
    assert(out.notify_multiplier == 4);
    put32(0, 0x10411af4);
    assert(aos_virtio_pci_decode(config, sizes, 0x1041, &out));
    assert(out.region[0].paddr == 0xfebc0000 && out.region[0].length == 4096);
    assert(out.region[1].paddr == 0xfebc3000 && out.region[1].length == 4096);
    assert(out.region[2].paddr == 0xfebc2000 && out.region[2].length == 4096);
    assert(out.notify_multiplier == 4);
    rejected(); /* A NIC must not bind as the block device. */
    fixture();
    put32(0x24, 1);
    assert(aos_virtio_pci_decode(config, sizes, 0x1042, &out));
    assert(out.region[1].paddr == UINT64_C(0x1febc3000));
    fixture(); config[0] = 0; rejected();
    fixture(); config[2] = 0x41; rejected();
    fixture(); config[0x0e] = 1; rejected();
    fixture(); config[6] = 0; rejected();
    fixture(); config[0x34] = 0x3c; rejected();
    fixture(); config[0x41] = 0x41; rejected();
    fixture(); config[0x65] = 0x40; rejected();
    fixture(); config[0x52] = 16; rejected();
    fixture(); config[0x66] = 255; rejected();
    fixture(); config[0x44] = 6; rejected();
    fixture(); config[0x44] = 5; sizes[5] = 0x4000; rejected();
    fixture(); sizes[4] = 0x2000; rejected();
    fixture(); sizes[4] = 0x3000; rejected();
    fixture(); put32(0x20, 0xfebc100c); rejected();
    fixture(); put32(0x48, 0xffffff00); rejected();
    fixture(); put32(0x4c, 0xffffffff); rejected();
    fixture(); put32(0x58, 0x3001); rejected();
    fixture(); put32(0x6c, 0x100); rejected(); /* aliases common region */
    fixture(); config[0x67] = 1; rejected(); /* duplicate common capability */
    fixture(); config[0x65] = 0xfc; config[0xfc] = 9; config[0xfe] = 16; rejected();
    fixture(); config[0x51] = 0; rejected(); /* missing device capability */
    fixture(); put32(0x20, 0xfebc0001); rejected(); /* I/O BAR */
    fixture(); put32(0x20, 0xffffc00c); put32(0x24, 0xffffffff); rejected();
    puts("PASS: modern virtio PCI capabilities, 64-bit BARs and bounded resource spans");
    return 0;
}
