#ifndef AOS_PLATFORM_GUEST_SCHEDULING_H
#define AOS_PLATFORM_GUEST_SCHEDULING_H
#include <stdbool.h>
#include <sel4/sel4.h>
#include "contracts/guest_scheduling_caps.h"

/* Manager's serial request loop owns scratch slots. Always delete temporary
 * copies, including after failed configuration; exchange caps stay private
 * to the corresponding VMM and disappear when its execution pool is revoked. */
static inline bool aos_guest_scheduling_configure(unsigned client)
{
    if (client >= AOS_GUEST_SCHED_CLIENTS) return false;
    bool ok = true;
    unsigned copied = 0;
    for (; copied < AOS_GUEST_SCHED_OBJECTS; copied++) {
        if (seL4_CNode_Copy(AOS_GUEST_SCHED_MANAGER_CNODE,
                AOS_GUEST_SCHED_SCRATCH_BASE + copied, AOS_GUEST_SCHED_MANAGER_BITS,
                AOS_GUEST_SCHED_EXCHANGE_BASE + client, copied,
                AOS_GUEST_SCHED_EXCHANGE_BITS, seL4_AllRights) != seL4_NoError) {
            ok = false;
            break;
        }
    }
    if (ok) ok = seL4_SchedControl_ConfigureFlags(AOS_GUEST_SCHED_CONTROL_BASE + client,
        AOS_GUEST_SCHED_SCRATCH_BASE + AOS_GUEST_SCHED_SC,
        AOS_GUEST_SCHED_BUDGET_US, AOS_GUEST_SCHED_PERIOD_US, 0u, 0u, 0u) == seL4_NoError;
    if (ok) ok = seL4_TCB_SetSchedParams(AOS_GUEST_SCHED_SCRATCH_BASE + AOS_GUEST_SCHED_TCB,
        AOS_GUEST_SCHED_AUTHORITY, AOS_GUEST_SCHED_PRIORITY, AOS_GUEST_SCHED_PRIORITY,
        AOS_GUEST_SCHED_SCRATCH_BASE + AOS_GUEST_SCHED_SC,
        AOS_GUEST_SCHED_SCRATCH_BASE + AOS_GUEST_SCHED_FAULT_EP) == seL4_NoError;
    while (copied) {
        --copied;
        if (seL4_CNode_Delete(AOS_GUEST_SCHED_MANAGER_CNODE,
                AOS_GUEST_SCHED_SCRATCH_BASE + copied,
                AOS_GUEST_SCHED_MANAGER_BITS) != seL4_NoError) ok = false;
    }
    return ok;
}
#endif
