#include <platform/guest_scheduling.h>
#include <assert.h>
#include <stdio.h>

static unsigned client, calls, fail_at, live, configured, scheduled, deletes;
static bool fail(void) { return ++calls == fail_at; }
seL4_Error seL4_CNode_Copy(seL4_CPtr root, seL4_Word slot, uint8_t depth,
    seL4_CPtr source, seL4_Word index, uint8_t source_depth, seL4_Word rights)
{
    assert(root == AOS_GUEST_SCHED_MANAGER_CNODE && depth == AOS_GUEST_SCHED_MANAGER_BITS);
    assert(source == AOS_GUEST_SCHED_EXCHANGE_BASE + client);
    assert(index < AOS_GUEST_SCHED_OBJECTS && slot == AOS_GUEST_SCHED_SCRATCH_BASE + index);
    assert(source_depth == AOS_GUEST_SCHED_EXCHANGE_BITS && rights == seL4_AllRights);
    assert(!configured && !scheduled && !deletes && !(live & (1u << index)));
    if (fail()) return 1;
    live |= 1u << index;
    return seL4_NoError;
}
seL4_Error seL4_SchedControl_ConfigureFlags(seL4_CPtr control, seL4_CPtr sc,
    seL4_Word budget, seL4_Word period, seL4_Word extra,
    seL4_Word badge, seL4_Word flags)
{
    assert(live == 7 && !scheduled && !deletes);
    assert(control == AOS_GUEST_SCHED_CONTROL_BASE + client);
    assert(sc == AOS_GUEST_SCHED_SCRATCH_BASE + AOS_GUEST_SCHED_SC);
    assert(budget == 25000 && period == 100000 && !extra && !badge && !flags);
    configured++;
    return fail() ? 1 : seL4_NoError;
}
seL4_Error seL4_TCB_SetSchedParams(seL4_CPtr tcb, seL4_CPtr authority,
    seL4_Word mcp, seL4_Word priority, seL4_CPtr sc, seL4_CPtr fault)
{
    assert(live == 7 && configured == 1 && !deletes);
    assert(tcb == AOS_GUEST_SCHED_SCRATCH_BASE + AOS_GUEST_SCHED_TCB);
    assert(authority == AOS_GUEST_SCHED_AUTHORITY && mcp == 150 && priority == 150);
    assert(sc == AOS_GUEST_SCHED_SCRATCH_BASE + AOS_GUEST_SCHED_SC);
    assert(fault == AOS_GUEST_SCHED_SCRATCH_BASE + AOS_GUEST_SCHED_FAULT_EP);
    scheduled++;
    return fail() ? 1 : seL4_NoError;
}
seL4_Error seL4_CNode_Delete(seL4_CPtr root, seL4_Word slot, uint8_t depth)
{
    assert(root == AOS_GUEST_SCHED_MANAGER_CNODE && depth == AOS_GUEST_SCHED_MANAGER_BITS);
    unsigned index = slot - AOS_GUEST_SCHED_SCRATCH_BASE;
    assert(index < AOS_GUEST_SCHED_OBJECTS && (live & (1u << index)));
    deletes++;
    if (fail()) return 1;
    live &= ~(1u << index);
    return seL4_NoError;
}
int main(void)
{
    assert(!aos_guest_scheduling_configure(AOS_GUEST_SCHED_CLIENTS) && !calls);
    for (client = 0; client < AOS_GUEST_SCHED_CLIENTS; client++) {
        for (fail_at = 0; fail_at <= 8; fail_at++) {
            calls = live = configured = scheduled = deletes = 0;
            assert(aos_guest_scheduling_configure(client) == (fail_at == 0));
            if (fail_at <= 3 && fail_at) {
                assert(!configured && !scheduled && deletes == fail_at - 1);
            } else {
                assert(configured == 1 && deletes == 3);
                assert(scheduled == (fail_at == 4 ? 0u : 1u));
            }
            assert((live != 0) == (fail_at >= 6));
        }
    }
    puts("PASS: guest scheduling uses owner-specific caps and fixed policy, fails closed and cleans scratch copies");
}
