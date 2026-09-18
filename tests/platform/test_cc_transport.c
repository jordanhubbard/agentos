#include <assert.h>
#include <stdio.h>
#include "contracts/cc_transport.h"

static cc_virtio_startup_t dma(uint32_t version)
{
    return (cc_virtio_startup_t){CC_VIRTIO_STARTUP_MAGIC, version,
        UINT64_C(0x100000000), UINT64_C(0x100001000), UINT64_C(0x100002000)};
}
static cc_virtio_pci_startup_t pci(void)
{
    return (cc_virtio_pci_startup_t){.dma = dma(CC_VIRTIO_STARTUP_PCI_VERSION),
        .offset = {0x100u, 0x200u, 0x300u}, .length = {56u, 8u, 12u},
        .notify_multiplier = 4u};
}
int main(void)
{
    cc_virtio_startup_t s = dma(CC_VIRTIO_STARTUP_VERSION);
    assert(cc_virtio_startup_valid(&s, CC_VIRTIO_STARTUP_VERSION));
    assert(!cc_virtio_startup_valid(NULL, CC_VIRTIO_STARTUP_VERSION));
    assert(!cc_virtio_startup_valid(&s, 99u));
    assert(!cc_virtio_startup_valid(&s, CC_VIRTIO_STARTUP_PCI_VERSION));
    s.magic ^= 1u;
    assert(!cc_virtio_startup_valid(&s, CC_VIRTIO_STARTUP_VERSION));
    /* Each alias or malformed page must fail, including >32-bit addresses. */
    for (unsigned i = 0; i < 3u; i++) {
        const uint64_t bad[] = {0u, UINT64_MAX, UINT64_C(0x100000001),
            UINT64_C(0x100000000), UINT64_C(0x100001000), UINT64_C(0x100002000)};
        for (unsigned j = 0; j < sizeof(bad) / sizeof(bad[0]); j++) {
            s = dma(CC_VIRTIO_STARTUP_VERSION);
            uint64_t original = i == 0 ? s.queue_pa : i == 1 ? s.tx_buffer_pa : s.rx_buffer_pa;
            if (bad[j] == original) continue;
            if (i == 0) s.queue_pa = bad[j];
            if (i == 1) s.tx_buffer_pa = bad[j];
            if (i == 2) s.rx_buffer_pa = bad[j];
            assert(!cc_virtio_startup_valid(&s, CC_VIRTIO_STARTUP_VERSION));
        }
    }
    cc_virtio_pci_startup_t p = pci();
    assert(cc_virtio_pci_startup_valid(&p));
    for (unsigned region = 0; region < CC_VIRTIO_PCI_REGIONS; region++) {
        p = pci(); p.offset[region]++;
        assert(!cc_virtio_pci_startup_valid(&p));
        p = pci(); p.offset[region] = 4096u;
        assert(!cc_virtio_pci_startup_valid(&p));
        const uint32_t minimum[] = {56u, 2u, 12u};
        p = pci(); p.length[region] = minimum[region] - 1u;
        assert(!cc_virtio_pci_startup_valid(&p));
        p = pci(); p.length[region] = 4096u - p.offset[region];
        assert(cc_virtio_pci_startup_valid(&p));
        p.length[region]++;
        assert(!cc_virtio_pci_startup_valid(&p));
    }
    p = pci(); p.length[CC_VIRTIO_PCI_NOTIFY] = 1u;
    assert(!cc_virtio_pci_startup_valid(&p));
    p = pci(); p.reserved = 1u;
    assert(!cc_virtio_pci_startup_valid(&p));
    p = pci(); p.notify_multiplier = 3u;
    assert(!cc_virtio_pci_startup_valid(&p));
    p = pci(); p.notify_multiplier = 0u;
    assert(cc_virtio_pci_startup_valid(&p));
    p.notify_multiplier = UINT32_MAX - 1u;
    assert(cc_virtio_pci_startup_valid(&p));
    puts("PASS: CC startup ABI, DMA page isolation and PCI mapped spans");
}
