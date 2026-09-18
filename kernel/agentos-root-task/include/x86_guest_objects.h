#ifndef AGENTOS_X86_GUEST_OBJECTS_H
#define AGENTOS_X86_GUEST_OBJECTS_H

#include <sel4/sel4.h>
#include "contracts/x86_guest_object_caps.h"

/* The largest object is allocated first, so these bounds include alignment.
 * There is no fallback to root's general untyped pool. */
_Static_assert(seL4_X86_VCPUBits <= 14u &&
               seL4_X86_EPTPML4Bits <= 12u &&
               seL4_X86_EPTPDPTBits <= 12u &&
               seL4_X86_EPTPDBits <= 12u,
               "x86 guest objects must fit the private 64 KiB pool");

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

#endif
