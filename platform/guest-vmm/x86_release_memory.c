#include <platform/guest_ram.h>
#include <libvmm/virtio/gpa.h>
#include "contracts/x86_guest_memory_caps.h"
#include <sel4/sel4.h>

/* Called only after execution has stopped and every device has detached.
 * Empty pools remain management authority. A partial failure is terminal for
 * execution, but retrying revocation of an already empty pool is safe. */
bool aos_vmm_guest_ram_release(size_t size)
{
    const size_t frame_size = (size_t)1u << AOS_GUEST_RAM_FRAME_BITS;
    if (!size || (size & (frame_size - 1u)) ||
        size / frame_size > AOS_GUEST_RAM_MAX_FRAMES) return false;
    virtio_gpa_set_translate(NULL);
    for (size_t i = 0; i < size / frame_size; i++) {
        if (seL4_CNode_Revoke(AOS_GUEST_RAM_SELF_CNODE,
                AOS_GUEST_RAM_POOL_BASE + i, AOS_GUEST_RAM_CNODE_BITS)
                != seL4_NoError) return false;
    }
    for (unsigned i = 0; i < AOS_X86_GUEST_ROM_FRAMES; i++) {
        if (seL4_CNode_Revoke(AOS_GUEST_RAM_SELF_CNODE,
                AOS_X86_GUEST_ROM_POOL_BASE + i, AOS_GUEST_RAM_CNODE_BITS)
                != seL4_NoError) return false;
    }
    return true;
}
