#ifndef AOS_PLATFORM_GUEST_GIC_MAPPING_H
#define AOS_PLATFORM_GUEST_GIC_MAPPING_H
#include <stdbool.h>
#include <sel4/sel4.h>
#include "contracts/guest_gic_caps.h"

/* The serial manager loop owns these scratch slots. Only called after a
 * successful CREATE: the guest is READY and has never started this generation.
 * Unmap affects one guest's cap, never the shared source or a peer mapping. */
static inline bool aos_guest_gic_prepare(unsigned client)
{
    if (client >= AOS_GUEST_SCHED_CLIENTS) return false;
    if (seL4_CNode_Copy(AOS_GUEST_SCHED_MANAGER_CNODE,
            AOS_GUEST_GIC_VSPACE_SCRATCH, AOS_GUEST_SCHED_MANAGER_BITS,
            AOS_GUEST_SCHED_EXCHANGE_BASE + client,
            AOS_GUEST_GIC_VSPACE_EXCHANGE_SLOT, AOS_GUEST_SCHED_EXCHANGE_BITS,
            seL4_AllRights) != seL4_NoError) return false;
    seL4_CPtr frame = AOS_GUEST_GIC_FRAME_BASE + client;
    bool ok = seL4_ARM_Page_Unmap(frame) == seL4_NoError;
    if (ok) ok = seL4_ARM_Page_Map(frame, AOS_GUEST_GIC_VSPACE_SCRATCH,
        AOS_GUEST_GIC_IPA, seL4_AllRights, seL4_ARM_Default_VMAttributes) == seL4_NoError;
    if (seL4_CNode_Delete(AOS_GUEST_SCHED_MANAGER_CNODE,
            AOS_GUEST_GIC_VSPACE_SCRATCH, AOS_GUEST_SCHED_MANAGER_BITS) != seL4_NoError)
        ok = false;
    return ok;
}
#endif
