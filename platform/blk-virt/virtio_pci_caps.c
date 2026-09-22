#include <platform/virtio_pci_caps.h>

static uint32_t u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

bool aos_virtio_pci_decode(const uint8_t config[AOS_PCI_CONFIG_BYTES],
                           const uint64_t bar_sizes[6], uint16_t expected_device,
                           aos_virtio_pci_layout_t *layout)
{
    if (!layout) return false;
    *layout = (aos_virtio_pci_layout_t){0};
    if (!config || !bar_sizes || expected_device < 0x1040u ||
        u16(config) != 0x1af4u || u16(config + 2) != expected_device ||
        (config[0x0e] & 0x7fu) || !(u16(config + 6) & 0x10u)) return false;
    uint64_t base[6] = {0};
    bool memory[6] = {false};
    for (unsigned bar = 0; bar < 6; bar++) {
        uint32_t low = u32(config + 0x10u + bar * 4u);
        if (low & 1u) continue;
        unsigned type = (low >> 1) & 3u;
        if (type != 0u && type != 2u) continue;
        base[bar] = low & ~UINT32_C(15);
        memory[bar] = bar_sizes[bar] &&
                      !(bar_sizes[bar] & (bar_sizes[bar] - 1u));
        if (type == 2u) {
            if (bar == 5u) return false;
            base[bar] |= (uint64_t)u32(config + 0x14u + bar * 4u) << 32;
            bar++;
        }
    }
    uint64_t visited = 0;
    unsigned found = 0;
    aos_virtio_pci_layout_t result = {0};
    for (unsigned off = config[0x34]; off; off = config[off + 1u]) {
        if (off < 0x40u || off > 0xfcu || (off & 3u)) return false;
        uint64_t bit = UINT64_C(1) << (off / 4u);
        if (visited & bit) return false;
        visited |= bit;
        if (config[off] != 9u) continue;
        unsigned len = config[off + 2u];
        if (len < 4u || len > AOS_PCI_CONFIG_BYTES - off) return false;
        unsigned type = config[off + 3u], index;
        if (type == 1u) index = AOS_VIRTIO_PCI_COMMON;
        else if (type == 2u) index = AOS_VIRTIO_PCI_NOTIFY;
        else if (type == 4u) index = AOS_VIRTIO_PCI_DEVICE;
        else continue;
        if (len < (type == 2u ? 20u : 16u) || (found & (1u << index))) return false;
        unsigned bar = config[off + 4u];
        uint32_t offset = u32(config + off + 8u);
        uint32_t length = u32(config + off + 12u);
        unsigned alignment = type == 2u ? 2u : 4u;
        unsigned minimum = type == 1u ? 56u : (type == 2u ? 2u : 8u);
        if (bar >= 6u || !memory[bar] || !base[bar] ||
            (base[bar] & (bar_sizes[bar] - 1u)) ||
            base[bar] > UINT64_MAX - bar_sizes[bar] ||
            length < minimum || (offset & (alignment - 1u)) ||
            offset > bar_sizes[bar] || length > bar_sizes[bar] - offset) return false;
        result.region[index] = (aos_virtio_pci_region_t){base[bar] + offset, length};
        if (type == 2u) result.notify_multiplier = u32(config + off + 16u);
        found |= 1u << index;
    }
    if (found != 7u) return false;
    /* Distinct register classes must not alias. */
    for (unsigned i = 0; i < AOS_VIRTIO_PCI_REGIONS; i++) {
        for (unsigned j = 0; j < i; j++) {
            if (result.region[i].paddr < result.region[j].paddr + result.region[j].length &&
                result.region[j].paddr < result.region[i].paddr + result.region[i].length)
                return false;
        }
    }
    *layout = result;
    return true;
}
