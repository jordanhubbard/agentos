#include <platform/guest_execution.h>
#include <platform/guest_paging.h>
#include "contracts/guest_execution_caps.h"
#include "contracts/guest_ram_caps.h"
#include "contracts/guest_scheduling_caps.h"
#include "contracts/guest_gic_caps.h"
#include <sel4/sel4.h>

#ifndef CONFIG_KERNEL_MCS
#error Guest execution reconstruction requires the manager scheduling broker
#endif

/* Allow one alignment-sized gap for each object, not just its payload. */
_Static_assert(2u * ((1u << seL4_TCBBits) + (1u << seL4_VCPUBits) +
               (1u << seL4_PageBits) + (1u << seL4_MinSchedContextBits)) <=
               (1u << AOS_GUEST_EXECUTION_POOL_BITS),
               "guest execution objects must fit their private pool");

bool aos_vmm_guest_execution_release(void)
{
    return seL4_CNode_Revoke(AOS_GUEST_RAM_SELF_CNODE,
        AOS_GUEST_EXECUTION_POOL_CAP, AOS_GUEST_RAM_CNODE_BITS) == seL4_NoError;
}

static bool allocate(seL4_Word type, seL4_Word bits, seL4_CPtr slot)
{
    return seL4_Untyped_Retype(AOS_GUEST_EXECUTION_POOL_CAP, type, bits,
        AOS_GUEST_RAM_SELF_CNODE, 0, 0, slot, 1) == seL4_NoError;
}

static bool publish(seL4_CPtr object, seL4_Word slot)
{
    return seL4_CNode_Copy(AOS_GUEST_SCHED_EXCHANGE_CAP, slot,
        AOS_GUEST_SCHED_EXCHANGE_BITS, AOS_GUEST_RAM_SELF_CNODE,
        object, AOS_GUEST_RAM_CNODE_BITS, seL4_AllRights) == seL4_NoError;
}

bool aos_vmm_guest_execution_rebuild(void)
{
    if (!allocate(seL4_TCBObject, 0, AOS_GUEST_TCB_CAP_BASE) ||
        !allocate(seL4_ARM_VCPUObject, 0, AOS_GUEST_VCPU_CAP_BASE) ||
        !allocate(seL4_SchedContextObject, seL4_MinSchedContextBits, AOS_GUEST_SC_CAP_BASE) ||
        !allocate(seL4_ARM_SmallPageObject, 0, AOS_GUEST_IPC_FRAME_CAP)) return false;
    /* Populate the GIC's intermediate tables before exporting this VSpace.
     * The manager owns the device frame and only performs the final mapping.
     * Borrow the fresh IPC frame while execution is stopped, then leave the
     * GIC leaf empty. No device capability enters the VMM. */
    if (!aos_vmm_guest_page_map(AOS_GUEST_IPC_FRAME_CAP, AOS_GUEST_GIC_IPA) ||
        seL4_ARM_Page_Unmap(AOS_GUEST_IPC_FRAME_CAP) != seL4_NoError ||
        !aos_vmm_guest_page_map(AOS_GUEST_IPC_FRAME_CAP, AOS_GUEST_IPC_BUFFER_VA)) return false;
    if (seL4_TCB_Configure(AOS_GUEST_TCB_CAP_BASE, AOS_GUEST_RAM_SELF_CNODE,
            seL4_WordBits - AOS_GUEST_RAM_CNODE_BITS, AOS_GUEST_RAM_GUEST_VSPACE,
            0, AOS_GUEST_IPC_BUFFER_VA, AOS_GUEST_IPC_FRAME_CAP) != seL4_NoError ||
        seL4_ARM_VCPU_SetTCB(AOS_GUEST_VCPU_CAP_BASE, AOS_GUEST_TCB_CAP_BASE)
            != seL4_NoError) return false;
    return publish(AOS_GUEST_TCB_CAP_BASE, AOS_GUEST_SCHED_TCB) &&
        publish(AOS_GUEST_SC_CAP_BASE, AOS_GUEST_SCHED_SC) &&
        publish(AOS_GUEST_RAM_GUEST_VSPACE, AOS_GUEST_GIC_VSPACE_EXCHANGE_SLOT);
}
