#include <platform/guest_execution.h>
#include <platform/guest_paging.h>
#include "contracts/guest_execution_caps.h"
#include "contracts/guest_ram_caps.h"
#include "contracts/guest_scheduling_caps.h"
#include "contracts/guest_gic_caps.h"
#include <sel4/sel4.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned calls, fail_at;
static bool allocated[5], mapped, configured, bound, exported[4];
static bool gic_tables, temporary_gic_mapping;
static bool fail(void) { return ++calls == fail_at; }
seL4_Error seL4_CNode_Revoke(seL4_CPtr root, seL4_Word slot, uint8_t depth)
{
    assert(root == AOS_GUEST_RAM_SELF_CNODE && slot == AOS_GUEST_EXECUTION_POOL_CAP);
    assert(depth == AOS_GUEST_RAM_CNODE_BITS);
    if (fail()) return 1;
    memset(allocated, 0, sizeof(allocated));
    mapped = configured = bound = temporary_gic_mapping = false;
    exported[0] = exported[1] = false;
    return 0;
}
seL4_Error seL4_Untyped_Retype(seL4_CPtr pool, seL4_Word type, seL4_Word bits,
    seL4_CPtr root, seL4_Word node, seL4_Word depth, seL4_Word slot, seL4_Word count)
{
    static const unsigned slots[] = {0, AOS_GUEST_TCB_CAP_BASE, AOS_GUEST_VCPU_CAP_BASE,
        AOS_GUEST_SC_CAP_BASE, AOS_GUEST_IPC_FRAME_CAP};
    assert(pool == AOS_GUEST_EXECUTION_POOL_CAP && root == AOS_GUEST_RAM_SELF_CNODE);
    assert(!node && !depth && count == 1 && type >= 1 && type <= 4);
    assert(slot == slots[type] && !allocated[type]);
    assert(bits == (type == seL4_SchedContextObject ? seL4_MinSchedContextBits : 0));
    if (fail()) return 1;
    allocated[type] = true;
    return 0;
}
bool aos_vmm_guest_page_map(uintptr_t frame, uintptr_t address)
{
    assert(allocated[4] && frame == AOS_GUEST_IPC_FRAME_CAP && !mapped);
    assert(address == AOS_GUEST_GIC_IPA || address == AOS_GUEST_IPC_BUFFER_VA);
    assert(!temporary_gic_mapping);
    if (address == AOS_GUEST_IPC_BUFFER_VA) assert(gic_tables);
    if (fail()) return false;
    if (address == AOS_GUEST_GIC_IPA) gic_tables = temporary_gic_mapping = true;
    else mapped = true;
    return true;
}
seL4_Error seL4_ARM_Page_Unmap(seL4_CPtr frame)
{
    assert(frame == AOS_GUEST_IPC_FRAME_CAP && temporary_gic_mapping && !mapped);
    if (fail()) return 1;
    temporary_gic_mapping = false;
    return 0;
}
seL4_Error seL4_TCB_Configure(seL4_CPtr tcb, seL4_CPtr cnode, seL4_Word guard,
    seL4_CPtr space, seL4_Word data, seL4_Word ipc, seL4_CPtr frame)
{
    assert(allocated[1] && mapped && gic_tables && !temporary_gic_mapping && tcb == AOS_GUEST_TCB_CAP_BASE);
    assert(cnode == AOS_GUEST_RAM_SELF_CNODE && guard == 64 - AOS_GUEST_RAM_CNODE_BITS);
    assert(space == AOS_GUEST_RAM_GUEST_VSPACE && !data);
    assert(ipc == AOS_GUEST_IPC_BUFFER_VA && frame == AOS_GUEST_IPC_FRAME_CAP);
    if (fail()) return 1;
    configured = true;
    return 0;
}
seL4_Error seL4_ARM_VCPU_SetTCB(seL4_CPtr vcpu, seL4_CPtr tcb)
{
    assert(configured && allocated[2] && vcpu == AOS_GUEST_VCPU_CAP_BASE && tcb == AOS_GUEST_TCB_CAP_BASE);
    if (fail()) return 1;
    bound = true;
    return 0;
}
seL4_Error seL4_CNode_Copy(seL4_CPtr dest, seL4_Word slot, uint8_t dd,
    seL4_CPtr src, seL4_Word object, uint8_t sd, seL4_Word rights)
{
    assert(bound && dest == AOS_GUEST_SCHED_EXCHANGE_CAP && dd == AOS_GUEST_SCHED_EXCHANGE_BITS);
    assert(src == AOS_GUEST_RAM_SELF_CNODE && sd == AOS_GUEST_RAM_CNODE_BITS && rights == seL4_AllRights);
    assert(slot == 0 || slot == 1 || slot == 3);
    assert(object == (slot == 0 ? AOS_GUEST_TCB_CAP_BASE : slot == 1 ? AOS_GUEST_SC_CAP_BASE : AOS_GUEST_RAM_GUEST_VSPACE));
    assert(!exported[slot] && exported[2]); /* fault endpoint retained */
    if (fail()) return 1;
    exported[slot] = true;
    return 0;
}
static void cleanup(void)
{
    fail_at = 0;
    assert(aos_vmm_guest_execution_release());
    /* The caller must also revoke paging to remove the VSpace export. */
    exported[3] = false;
    gic_tables = false;
    assert(exported[2] && !configured && !bound && !mapped);
}
int main(void)
{
    exported[2] = true;
    assert(aos_vmm_guest_execution_rebuild());
    unsigned complete_calls = calls;
    assert(configured && bound && exported[0] && exported[1] && exported[3]);
    for (unsigned failure = 1; failure <= complete_calls; ++failure) {
        cleanup();
        calls = 0; fail_at = failure;
        assert(!aos_vmm_guest_execution_rebuild());
        cleanup();
        calls = 0;
        assert(aos_vmm_guest_execution_rebuild());
        assert(configured && bound && exported[0] && exported[1] && exported[3]);
    }
    calls = 0; fail_at = 1;
    assert(!aos_vmm_guest_execution_release() && bound && exported[0]);
    cleanup();
    puts("PASS: guest execution reconstruction, exact private authority, publication and recovery from every failure");
}
