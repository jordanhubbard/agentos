#include <assert.h>
#include <stdio.h>
#include "x86_guest_objects.h"

static unsigned calls, fail_at;
static seL4_CPtr pool;
static int frame_test, rebuild_test;
static int cpu_rebuild_test, cpu_present, old_alias_present;
static const seL4_CPtr slots[] = {80u, 91u, 102u, 117u, 133u, 149u};
static const seL4_CPtr rebuild_slots[] = {
    AOS_GUEST_VCPU_CAP_BASE, AOS_GUEST_RAM_GUEST_VSPACE,
    AOS_X86_GUEST_EPT_PDPT_CAP, AOS_X86_GUEST_EPT_LOW_PD_CAP,
    AOS_X86_GUEST_EPT_HIGH_PD_CAP,
    AOS_X86_GUEST_EPT_SECOND_RAM_PD_CAP,
};
static const seL4_Word expected_types[] = {
    seL4_X86_VCPUObject, seL4_X86_EPTPML4Object,
    seL4_X86_EPTPDPTObject, seL4_X86_EPTPDObject, seL4_X86_EPTPDObject,
    seL4_X86_EPTPDObject,
};

seL4_Error seL4_Untyped_Retype(seL4_CPtr source, seL4_Word type,
    seL4_Word bits, seL4_CPtr root, seL4_Word node,
    seL4_Word depth, seL4_Word offset, seL4_Word count)
{
    if (cpu_rebuild_test) {
        assert(calls==1 && !cpu_present && !old_alias_present);
        assert(source==AOS_X86_VCPU_POOL_CAP && type==seL4_X86_VCPUObject);
        assert(root==AOS_GUEST_RAM_SELF_CNODE && offset==AOS_GUEST_VCPU_CAP_BASE);
        assert(!bits && !node && !depth && count==1);
        calls++;
        if (calls==fail_at) return 19;
        cpu_present=1;
        return seL4_NoError;
    }
    seL4_CPtr child=rebuild_test ? AOS_X86_VCPU_POOL_CAP : 200u;
    assert(calls < AOS_X86_GUEST_OBJECT_COUNT+1u);
    assert(source == (!frame_test && calls==1u ? child : pool));
    assert(root == (rebuild_test ? AOS_GUEST_RAM_SELF_CNODE : 7u));
    if (!frame_test && calls==0u) {
        assert(type==seL4_UntypedObject && bits==AOS_X86_VCPU_POOL_BITS && offset==child);
    } else {
        unsigned index=frame_test ? 0u : calls-1u;
        assert(type == (frame_test ? seL4_X86_LargePageObject : expected_types[index]));
        assert(offset == (rebuild_test ? rebuild_slots[index] : slots[index]));
        assert(bits==0u);
    }
    assert(node == 0u && depth == 0u && count == 1u);
    calls++;
    return calls == fail_at ? 19 : seL4_NoError;
}

seL4_Error seL4_CNode_Revoke(seL4_CPtr root,seL4_Word slot,uint8_t depth)
{
    assert(cpu_rebuild_test && calls==0);
    assert(root==AOS_GUEST_RAM_SELF_CNODE && slot==AOS_X86_VCPU_POOL_CAP && depth==AOS_GUEST_RAM_CNODE_BITS);
    if (++calls==fail_at) return 19;
    cpu_present=old_alias_present=0;
    return seL4_NoError;
}

seL4_Error seL4_X86_ASIDPool_Assign(seL4_CPtr asid, seL4_CPtr ept)
{
    assert(rebuild_test && calls == 7u);
    assert(asid == AOS_X86_GUEST_ASID_POOL_CAP);
    assert(ept == AOS_GUEST_RAM_GUEST_VSPACE);
    return ++calls == fail_at ? 19 : seL4_NoError;
}
seL4_Error seL4_X86_EPTPDPT_Map(seL4_CPtr table, seL4_CPtr ept,
                              seL4_Word gpa, seL4_Word attr)
{
    assert(calls == 8u && table == AOS_X86_GUEST_EPT_PDPT_CAP);
    assert(ept == AOS_GUEST_RAM_GUEST_VSPACE && gpa == 0u);
    assert(attr == seL4_X86_EPT_Default_VMAttributes);
    return ++calls == fail_at ? 19 : seL4_NoError;
}
seL4_Error seL4_X86_EPTPD_Map(seL4_CPtr table, seL4_CPtr ept,
                            seL4_Word gpa, seL4_Word attr)
{
    assert(calls >= 9u && calls <= 11u);
    const seL4_CPtr directories[] = {AOS_X86_GUEST_EPT_LOW_PD_CAP,
        AOS_X86_GUEST_EPT_HIGH_PD_CAP, AOS_X86_GUEST_EPT_SECOND_RAM_PD_CAP};
    const seL4_Word addresses[] = {0u, 0xc0000000u, 0x40000000u};
    assert(table == directories[calls - 9u]);
    assert(gpa == addresses[calls - 9u]);
    assert(ept == AOS_GUEST_RAM_GUEST_VSPACE);
    assert(attr == seL4_X86_EPT_Default_VMAttributes);
    return ++calls == fail_at ? 19 : seL4_NoError;
}

static unsigned bind_calls, bind_failure;
seL4_Error seL4_TCB_SetEPTRoot(seL4_CPtr tcb, seL4_CPtr ept)
{
    assert(bind_calls == 0u && tcb == AOS_X86_VMM_SELF_TCB_CAP &&
           ept == AOS_GUEST_RAM_GUEST_VSPACE);
    return ++bind_calls == bind_failure ? 19 : seL4_NoError;
}
seL4_Error seL4_X86_VCPU_SetTCB(seL4_CPtr vcpu, seL4_CPtr tcb)
{
    assert(bind_calls == 1u && tcb == AOS_X86_VMM_SELF_TCB_CAP &&
           vcpu == AOS_GUEST_VCPU_CAP_BASE);
    return ++bind_calls == bind_failure ? 19 : seL4_NoError;
}

int main(void)
{
    /* Each VMM's supplied pool is the only permitted allocation source.
     * Every failure must propagate without allocating later objects. */
    for (pool = 50u; pool <= 51u; pool++) {
        for (fail_at = 0; fail_at <= AOS_X86_GUEST_OBJECT_COUNT+1u; fail_at++) {
            calls = 0;
            seL4_Error err = aos_x86_guest_objects_retype(pool, 7u, slots, 200u);
            assert(err == (fail_at ? 19 : seL4_NoError));
            assert(calls == (fail_at ? fail_at : AOS_X86_GUEST_OBJECT_COUNT+1u));
        }
    }
    /* Every supported RAM reservation has distinct in-range pool grants;
     * adding ROM must never consume the frame/alias reconstruction ranges. */
    for (unsigned count = 1; count <= AOS_GUEST_RAM_MAX_FRAMES; count++) {
        unsigned char seen[1u << AOS_GUEST_RAM_CNODE_BITS] = {0};
        for (unsigned i = 0; i < count + AOS_X86_GUEST_ROM_FRAMES; i++) {
            seL4_CPtr slot = aos_x86_guest_memory_pool_slot(count, i);
            assert(slot && slot < AOS_GUEST_RAM_FRAME_BASE && !seen[slot]);
            seen[slot] = 1;
            if (i < count) assert(slot == AOS_GUEST_RAM_POOL_BASE + i);
            else assert(slot == AOS_X86_GUEST_ROM_POOL_BASE + i - count);
        }
        assert(!aos_x86_guest_memory_pool_slot(count, count + AOS_X86_GUEST_ROM_FRAMES));
        assert(!aos_x86_guest_memory_pool_slot(count, UINT32_MAX));
    }
    assert(!aos_x86_guest_memory_pool_slot(0, 0));
    assert(!aos_x86_guest_memory_pool_slot(AOS_GUEST_RAM_MAX_FRAMES + 1u, 0));
    assert(!aos_x86_guest_memory_pool_slot(UINT32_MAX, 0));
    frame_test = 1;
    for (pool = 50; pool <= 51; pool++) {
        for (fail_at = 0; fail_at <= 1; fail_at++) {
            calls = 0;
            assert(aos_x86_guest_frame_retype(pool, 7u, slots[0]) ==
                   (fail_at ? 19 : seL4_NoError));
            assert(calls == 1);
        }
    }
    frame_test = 0;
    rebuild_test = 1;
    pool = AOS_X86_GUEST_OBJECT_POOL_CAP;
    for (fail_at = 0; fail_at <= 12u; fail_at++) {
        calls = 0;
        assert(aos_x86_guest_objects_rebuild() == (fail_at ? 19 : seL4_NoError));
        assert(calls == (fail_at ? fail_at : 12u));
    }
    for (bind_failure = 0; bind_failure <= 2; bind_failure++) {
        bind_calls = 0;
        assert(aos_x86_guest_objects_bind() == (bind_failure ? 19 : seL4_NoError));
        assert(bind_calls == (bind_failure ? bind_failure : 2u));
    }
    cpu_rebuild_test=1;
    for (fail_at=0; fail_at<=2; fail_at++) {
        calls=0; cpu_present=old_alias_present=1;
        assert(aos_x86_guest_vcpu_rebuild(AOS_X86_VCPU_POOL_CAP,AOS_GUEST_VCPU_CAP_BASE)==
            (fail_at ? 19 : seL4_NoError));
        assert(calls==(fail_at ? fail_at : 2u));
        assert(cpu_present==(fail_at!=2));
        assert(old_alias_present==(fail_at==1));
    }
    calls=0;
    assert(aos_x86_guest_vcpu_rebuild(0,1)==seL4_InvalidArgument);
    assert(aos_x86_guest_vcpu_rebuild(1,0)==seL4_InvalidArgument);
    assert(aos_x86_guest_vcpu_rebuild(1,1)==seL4_InvalidArgument);
    assert(!calls);
    puts("PASS: x86 private objects, EPT reconstruction/binding failure propagation and bounded RAM/ROM grants");
}
