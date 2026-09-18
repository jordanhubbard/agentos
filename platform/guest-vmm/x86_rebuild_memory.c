#include <platform/x86_memory_rebuild.h>
#include <libvmm/virtio/gpa.h>
#include "contracts/x86_guest_memory_caps.h"
#include "contracts/x86_guest_object_caps.h"
#include "contracts/x86_vtx_proof.h"
#include <sel4/sel4.h>

_Static_assert(AOS_X86_GUEST_ROM_FRAME_BASE > AOS_X86_GUEST_EPT_HIGH_PD_CAP &&
    AOS_X86_GUEST_ROM_FRAME_BASE + AOS_X86_GUEST_ROM_FRAMES <= AOS_X86_GUEST_ROM_ALIAS_BASE &&
    AOS_X86_GUEST_ROM_ALIAS_BASE + AOS_X86_GUEST_ROM_FRAMES <= AOS_GUEST_RAM_POOL_BASE,
    "ROM reconstruction caps must not overlap EPT or RAM pools");

bool aos_x86_guest_memory_rebuild(const uint8_t *image, size_t image_size,
                                  size_t ram_size)
{
    const size_t frame_bytes = (size_t)1u << AOS_GUEST_RAM_FRAME_BITS;
    uintptr_t source = (uintptr_t)image;
    if (!image || image_size != AOS_X86_FIRMWARE_BYTES ||
        ram_size != AOS_X86_FIRMWARE_RAM || source > UINTPTR_MAX - image_size)
        return false;
    /* Restoring ROM must never read from a mapping this operation replaces. */
    if ((source < AOS_X86_FIRMWARE_RAM_VA + ram_size &&
         source + image_size > AOS_X86_FIRMWARE_RAM_VA) ||
        (source < AOS_X86_FIRMWARE_ROM_VA + image_size &&
         source + image_size > AOS_X86_FIRMWARE_ROM_VA)) return false;
    virtio_gpa_set_translate(NULL);
    for (unsigned i = 0; i < ram_size / frame_bytes + AOS_X86_GUEST_ROM_FRAMES; i++) {
        bool rom = i >= ram_size / frame_bytes;
        unsigned index = rom ? i - ram_size / frame_bytes : i;
        seL4_CPtr pool = (rom ? AOS_X86_GUEST_ROM_POOL_BASE : AOS_GUEST_RAM_POOL_BASE) + index;
        seL4_CPtr frame = (rom ? AOS_X86_GUEST_ROM_FRAME_BASE : AOS_GUEST_RAM_FRAME_BASE) + index;
        seL4_CPtr alias = (rom ? AOS_X86_GUEST_ROM_ALIAS_BASE : AOS_GUEST_RAM_ALIAS_BASE) + index;
        uintptr_t offset = index * frame_bytes;
        uintptr_t hva = (rom ? AOS_X86_FIRMWARE_ROM_VA : AOS_X86_FIRMWARE_RAM_VA) + offset;
        uintptr_t gpa = rom ? AOS_X86_FIRMWARE_BASE + offset : offset;
        seL4_CapRights_t rights = seL4_CapRights_new(0u, 0u, 1u, !rom);
        if (seL4_Untyped_Retype(pool, seL4_X86_LargePageObject, 0u,
                AOS_GUEST_RAM_SELF_CNODE, 0u, 0u, frame, 1u) != seL4_NoError)
            return false;
        if (rom) {
            /* Only initialization uses a writable native mapping. Remove it
             * before publishing either read-only ROM mapping. */
            if (seL4_X86_Page_Map(frame, AOS_GUEST_RAM_VMM_VSPACE, hva,
                    seL4_AllRights, seL4_X86_Default_VMAttributes) != seL4_NoError)
                return false;
            volatile uint8_t *destination = (volatile uint8_t *)hva;
            for (size_t n = 0; n < frame_bytes; n++) destination[n] = image[offset + n];
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            if (seL4_X86_Page_Unmap(frame) != seL4_NoError) return false;
        }
        if (seL4_CNode_Copy(AOS_GUEST_RAM_SELF_CNODE, alias, AOS_GUEST_RAM_CNODE_BITS,
                AOS_GUEST_RAM_SELF_CNODE, frame, AOS_GUEST_RAM_CNODE_BITS, rights)
                != seL4_NoError) return false;
        if (seL4_X86_Page_Map(alias, AOS_GUEST_RAM_VMM_VSPACE, hva, rights,
                seL4_X86_Default_VMAttributes) != seL4_NoError) return false;
        if (seL4_X86_Page_MapEPT(frame, AOS_GUEST_RAM_GUEST_VSPACE, gpa, rights,
                seL4_X86_EPT_Default_VMAttributes) != seL4_NoError) return false;
    }
    return true;
}
