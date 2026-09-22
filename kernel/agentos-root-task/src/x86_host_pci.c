#include "x86_host_pci.h"
#include "ut_alloc.h"

#if defined(__x86_64__) && defined(AGENTOS_X86_FIRMWARE_RESET)
#define SELECT(off) (select | (off))
static bool device_config(aos_x86_host_device_t device, uint32_t *select,
                          uint32_t *expected)
{
    switch (device) {
    case AOS_X86_HOST_BLOCK:
        *select = UINT32_C(0x80002800); /* 00:05.0 */
        *expected = UINT32_C(0x10421af4);
        return true;
    case AOS_X86_HOST_NET:
        *select = UINT32_C(0x80003000); /* 00:06.0 */
        *expected = UINT32_C(0x10411af4);
        return true;
    case AOS_X86_HOST_CONSOLE:
        *select = UINT32_C(0x80003800); /* 00:07.0 */
        *expected = UINT32_C(0x10431af4);
        return true;
    case AOS_X86_HOST_SECONDARY_BLOCK:
        *select = UINT32_C(0x80004000); /* 00:08.0 */
        *expected = UINT32_C(0x10421af4);
        return true;
    default:
        return false;
    }
}
static bool read32(seL4_CPtr cap, uint32_t select, unsigned offset, uint32_t *value)
{
    if (seL4_X86_IOPort_Out32(cap, 0xcf8u, SELECT(offset)) != seL4_NoError) return false;
    seL4_X86_IOPort_In32_t r = seL4_X86_IOPort_In32(cap, 0xcfcu);
    if (r.error != seL4_NoError) return false;
    *value = r.result;
    return true;
}
static bool write32(seL4_CPtr cap, uint32_t select, unsigned offset, uint32_t value)
{
    return seL4_X86_IOPort_Out32(cap, 0xcf8u, SELECT(offset)) == seL4_NoError &&
           seL4_X86_IOPort_Out32(cap, 0xcfcu, value) == seL4_NoError;
}
static bool command(seL4_CPtr cap, uint32_t select, uint16_t value)
{
    /* A halfword write avoids clearing PCI status write-one-to-clear bits. */
    return seL4_X86_IOPort_Out32(cap, 0xcf8u, SELECT(4u)) == seL4_NoError &&
           seL4_X86_IOPort_Out16(cap, 0xcfcu, value) == seL4_NoError;
}
unsigned aos_x86_host_pci_discover(aos_x86_host_device_t device,
                                  aos_virtio_pci_layout_t *layout)
{
    if (!layout) return 1u;
    *layout = (aos_virtio_pci_layout_t){0};
    uint32_t select, expected;
    if (!device_config(device, &select, &expected)) return 1u;
    seL4_CPtr cap = ut_alloc_slot();
    if (!cap || seL4_X86_IOPortControl_Issue(seL4_CapIOPortControl,
            0xcf8u, 0xcffu, seL4_CapInitThreadCNode, cap, 64u) != seL4_NoError) return 1u;
    uint32_t snapshot[64] = {0};
    uint64_t sizes[6] = {0};
    unsigned status = 2u;
    bool disabled = false;
    bool restore_command = true;
    for (unsigned i = 0; i < 64; i++) {
        if (!read32(cap, select, i * 4u, &snapshot[i])) goto out;
    }
    status = 3u;
    if (snapshot[0] != expected) goto out;
    status = 4u;
    disabled = true;
    if (!command(cap, select, (uint16_t)snapshot[1] & ~7u)) goto out;
    uint32_t current_command;
    if (!read32(cap, select, 4u, &current_command) || (current_command & 7u)) goto out;
    for (unsigned bar = 0; bar < 6; bar++) {
        uint32_t low = snapshot[4u + bar];
        if (low & 1u) continue;
        unsigned type = (low >> 1) & 3u;
        if (type != 0u && type != 2u) continue;
        bool wide = type == 2u;
        if (wide && bar == 5u) goto out;
        uint32_t mask_low = 0, mask_high = 0;
        unsigned offset = 0x10u + bar * 4u;
        bool ok = write32(cap, select, offset, UINT32_MAX);
        if (wide) ok = write32(cap, select, offset + 4u, UINT32_MAX) && ok;
        ok = read32(cap, select, offset, &mask_low) && ok;
        if (wide) ok = read32(cap, select, offset + 4u, &mask_high) && ok;
        /* Restore both halves even when a probe failed. */
        bool restored = write32(cap, select, offset, low);
        if (wide) restored = write32(cap, select, offset + 4u, snapshot[5u + bar]) && restored;
        if (!restored) restore_command = false;
        if (!ok || !restored) goto out;
        if (wide) {
            uint64_t mask = ((uint64_t)mask_high << 32) | (mask_low & ~UINT32_C(15));
            if (mask) sizes[bar] = ~mask + 1u;
            bar++;
        } else if (mask_low & ~UINT32_C(15)) {
            sizes[bar] = (uint32_t)(~(mask_low & ~UINT32_C(15)) + 1u);
        }
    }
    status = aos_virtio_pci_decode((const uint8_t *)snapshot, sizes, (uint16_t)(expected >> 16), layout) ? 0u : 5u;
out:
    if (disabled) {
        /* Never re-enable decode or DMA after an unverified BAR restore. */
        for (unsigned bar = 0; bar < 6; bar++) {
            uint32_t value;
            if (!read32(cap, select, 0x10u + bar * 4u, &value) || value != snapshot[4u + bar])
                restore_command = false;
        }
        uint32_t restored_command;
        if (!restore_command || !command(cap, select, (uint16_t)snapshot[1]) ||
            !read32(cap, select, 4u, &restored_command) ||
            (uint16_t)restored_command != (uint16_t)snapshot[1]) status = 6u;
    }
    if (seL4_CNode_Delete(seL4_CapInitThreadCNode, cap, 64u) != seL4_NoError) status = 7u;
    if (status) *layout = (aos_virtio_pci_layout_t){0};
    return status;
}

bool aos_x86_host_pci_enable(aos_x86_host_device_t device)
{
    uint32_t select, expected;
    if (!device_config(device, &select, &expected)) return false;
    seL4_CPtr cap = ut_alloc_slot();
    if (!cap || seL4_X86_IOPortControl_Issue(seL4_CapIOPortControl,
            0xcf8u, 0xcffu, seL4_CapInitThreadCNode, cap, 64u) != seL4_NoError) return false;
    uint32_t identity, current = 0, confirmed;
    bool identified = read32(cap, select, 0u, &identity) && identity == expected &&
                      read32(cap, select, 4u, &current);
    bool ok = identified;
    uint16_t wanted = ((uint16_t)current & ~1u) | 0x406u;
    if (ok) ok = command(cap, select, wanted) && read32(cap, select, 4u, &confirmed) &&
                 (uint16_t)confirmed == wanted;
    if (!ok && identified) (void)command(cap, select, (uint16_t)current & ~7u);
    return seL4_CNode_Delete(seL4_CapInitThreadCNode, cap, 64u) == seL4_NoError && ok;
}
#endif
