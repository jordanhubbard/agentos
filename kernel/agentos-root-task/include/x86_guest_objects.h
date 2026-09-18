#ifndef AGENTOS_X86_GUEST_OBJECTS_H
#define AGENTOS_X86_GUEST_OBJECTS_H

#include <sel4/sel4.h>
#include "contracts/x86_guest_object_caps.h"
#include "contracts/x86_guest_memory_caps.h"

/* The largest object is allocated first, so these bounds include alignment.
 * There is no fallback to root's general untyped pool. */
_Static_assert(seL4_X86_VCPUBits <= 14u &&
               seL4_X86_EPTPML4Bits <= 12u &&
               seL4_X86_EPTPDPTBits <= 12u &&
               seL4_X86_EPTPDBits <= 12u,
               "x86 guest objects must fit the private 64 KiB pool");
_Static_assert(AOS_X86_GUEST_ROM_POOL_BASE > AOS_X86_GUEST_OBJECT_POOL_CAP &&
               AOS_X86_GUEST_ROM_POOL_BASE + AOS_X86_GUEST_ROM_FRAMES <= AOS_GUEST_RAM_POOL_BASE &&
               AOS_GUEST_RAM_POOL_BASE + AOS_GUEST_RAM_MAX_FRAMES <= AOS_GUEST_RAM_FRAME_BASE &&
               AOS_GUEST_RAM_ALIAS_BASE + AOS_GUEST_RAM_MAX_FRAMES <= (1u << AOS_GUEST_RAM_CNODE_BITS),
               "x86 pool grants must be disjoint and fit the private CNode");

static inline seL4_Word aos_x86_guest_object_type(unsigned index)
{
    const seL4_Word types[AOS_X86_GUEST_OBJECT_COUNT] = {
        seL4_X86_VCPUObject, seL4_X86_EPTPML4Object,
        seL4_X86_EPTPDPTObject, seL4_X86_EPTPDObject, seL4_X86_EPTPDObject,
    };
    return types[index];
}

/* Slots are reserved empty destinations in root's CNode. On failure the
 * caller must refuse guest start; partial objects remain below this pool
 * and must be revoked before any retry. */
static inline seL4_Error aos_x86_guest_objects_retype(seL4_CPtr pool,
    seL4_CPtr root, const seL4_CPtr slots[AOS_X86_GUEST_OBJECT_COUNT])
{
    for (unsigned i = 0; i < AOS_X86_GUEST_OBJECT_COUNT; i++) {
        seL4_Error err = seL4_Untyped_Retype(pool, aos_x86_guest_object_type(i),
            0u, root, 0u, 0u, slots[i], 1u);
        if (err != seL4_NoError) return err;
    }
    return seL4_NoError;
}

/* Zero denotes an invalid reservation/index, never an allocation slot. */
static inline seL4_CPtr aos_x86_guest_memory_pool_slot(unsigned ram_frames,
                                                      unsigned frame_index)
{
    if (!ram_frames || ram_frames > AOS_GUEST_RAM_MAX_FRAMES) return 0;
    if (frame_index < ram_frames) return AOS_GUEST_RAM_POOL_BASE + frame_index;
    unsigned rom_index = frame_index - ram_frames;
    return rom_index < AOS_X86_GUEST_ROM_FRAMES
        ? AOS_X86_GUEST_ROM_POOL_BASE + rom_index : 0;
}

static inline seL4_Error aos_x86_guest_frame_retype(seL4_CPtr pool,
    seL4_CPtr root, seL4_CPtr slot)
{
    return seL4_Untyped_Retype(pool, seL4_X86_LargePageObject, 0u,
        root, 0u, 0u, slot, 1u);
}

#endif
