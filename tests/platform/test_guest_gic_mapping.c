#include <platform/guest_gic_mapping.h>
#include <assert.h>
#include <stdio.h>

static unsigned client, calls, fail_at, deletes;
static bool scratch, mapped[2];
static bool fail(void) { return ++calls == fail_at; }
seL4_Error seL4_CNode_Copy(seL4_CPtr root, seL4_Word slot, uint8_t depth,
    seL4_CPtr source, seL4_Word index, uint8_t source_depth, seL4_Word rights)
{
    assert(root == AOS_GUEST_SCHED_MANAGER_CNODE && depth == AOS_GUEST_SCHED_MANAGER_BITS);
    assert(slot == AOS_GUEST_GIC_VSPACE_SCRATCH && !scratch);
    assert(source == AOS_GUEST_SCHED_EXCHANGE_BASE + client);
    assert(index == AOS_GUEST_GIC_VSPACE_EXCHANGE_SLOT);
    assert(source_depth == AOS_GUEST_SCHED_EXCHANGE_BITS && rights == seL4_AllRights);
    if (fail()) return 1;
    scratch = true;
    return seL4_NoError;
}
seL4_Error seL4_ARM_Page_Unmap(seL4_CPtr frame)
{
    assert(scratch && frame == AOS_GUEST_GIC_FRAME_BASE + client);
    if (fail()) return 1;
    mapped[client] = false;
    return seL4_NoError;
}
seL4_Error seL4_ARM_Page_Map(seL4_CPtr frame, seL4_CPtr vspace,
    seL4_Word ipa, seL4_Word rights, seL4_Word attributes)
{
    assert(scratch && !mapped[client]);
    assert(frame == AOS_GUEST_GIC_FRAME_BASE + client);
    assert(vspace == AOS_GUEST_GIC_VSPACE_SCRATCH && ipa == 0x08010000u);
    assert(rights == seL4_AllRights && attributes == seL4_ARM_Default_VMAttributes);
    if (fail()) return 1;
    mapped[client] = true;
    return seL4_NoError;
}
seL4_Error seL4_CNode_Delete(seL4_CPtr root, seL4_Word slot, uint8_t depth)
{
    assert(root == AOS_GUEST_SCHED_MANAGER_CNODE && depth == AOS_GUEST_SCHED_MANAGER_BITS);
    assert(slot == AOS_GUEST_GIC_VSPACE_SCRATCH && scratch);
    ++deletes;
    if (fail()) return 1;
    scratch = false;
    return seL4_NoError;
}
int main(void)
{
    assert(!aos_guest_gic_prepare(AOS_GUEST_SCHED_CLIENTS) && !calls);
    for (client = 0; client < 2; ++client) {
        for (unsigned initially_mapped = 0; initially_mapped < 2; ++initially_mapped) {
            for (fail_at = 0; fail_at <= 4; ++fail_at) {
                calls = deletes = 0;
                scratch = false;
                mapped[client] = initially_mapped;
                mapped[1u - client] = true;
                assert(aos_guest_gic_prepare(client) == (fail_at == 0));
                assert(mapped[1u - client]);
                assert(deletes == (fail_at == 1 ? 0u : 1u));
                assert(scratch == (fail_at == 4));
                if (!fail_at || fail_at == 4) assert(mapped[client]);
                else if (fail_at == 3) assert(!mapped[client]);
                else assert(mapped[client] == (bool)initially_mapped);
            }
        }
    }
    puts("PASS: fixed guest GIC mapping, peer preservation and fail-closed scratch cleanup");
}
