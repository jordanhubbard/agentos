#include <platform/guest_paging.h>
#include <stddef.h>
#include "contracts/guest_paging_caps.h"
#include <sel4/sel4.h>
_Static_assert(((size_t)1u << seL4_VSpaceBits) +
               AOS_GUEST_PAGING_TABLE_COUNT * ((size_t)1u << seL4_PageTableBits) <=
               ((size_t)1u << AOS_GUEST_PAGING_POOL_BITS),
               "bounded guest paging objects must fit their private pool");

static unsigned next_table;

bool aos_vmm_guest_paging_release(void)
{
    if (seL4_CNode_Revoke(AOS_GUEST_RAM_SELF_CNODE, AOS_GUEST_PAGING_POOL_CAP,
            AOS_GUEST_RAM_CNODE_BITS) != seL4_NoError) return false;
    next_table = 0;
    return true;
}

bool aos_vmm_guest_paging_rebuild(void)
{
    if (seL4_Untyped_Retype(AOS_GUEST_PAGING_POOL_CAP, seL4_ARM_VSpaceObject,
            0u, AOS_GUEST_RAM_SELF_CNODE, 0u, 0u, AOS_GUEST_RAM_GUEST_VSPACE,
            1u) != seL4_NoError) return false;
    return seL4_ARM_ASIDPool_Assign(AOS_GUEST_ASID_POOL_CAP,
        AOS_GUEST_RAM_GUEST_VSPACE) == seL4_NoError;
}

bool aos_vmm_guest_page_map(uintptr_t frame, uintptr_t guest_address)
{
    /* ARM has at most three intermediate tables below the VSpace root. */
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        seL4_Error err = seL4_ARM_Page_Map(frame, AOS_GUEST_RAM_GUEST_VSPACE,
            guest_address, seL4_AllRights, seL4_ARM_Default_VMAttributes);
        if (err == seL4_NoError) return true;
        if (err != seL4_FailedLookup || attempt == 3 ||
            next_table >= AOS_GUEST_PAGING_TABLE_COUNT) return false;
        seL4_CPtr table = AOS_GUEST_PAGING_TABLE_BASE + next_table;
        if (seL4_Untyped_Retype(AOS_GUEST_PAGING_POOL_CAP, seL4_ARM_PageTableObject,
                0u, AOS_GUEST_RAM_SELF_CNODE, 0u, 0u, table, 1u) != seL4_NoError)
            return false;
        ++next_table; /* A failed map still owns this newly retyped object. */
        if (seL4_ARM_PageTable_Map(table, AOS_GUEST_RAM_GUEST_VSPACE,
                guest_address, seL4_ARM_Default_VMAttributes) != seL4_NoError)
            return false;
    }
    return false;
}
