#include <assert.h>
#include <stdio.h>
#include "x86_guest_objects.h"

static unsigned calls, fail_at;
static seL4_CPtr pool;
static int frame_test, rebuild_test;
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
    assert(calls < AOS_X86_GUEST_OBJECT_COUNT);
    assert(source == pool && root == (rebuild_test ? AOS_GUEST_RAM_SELF_CNODE : 7u));
    assert(type == (frame_test ? seL4_X86_LargePageObject : expected_types[calls]));
    assert(offset == (rebuild_test ? rebuild_slots[calls] : slots[calls]));
    assert(bits == 0u && node == 0u && depth == 0u && count == 1u);
    calls++;
    return calls == fail_at ? 19 : seL4_NoError;
}

seL4_Error seL4_X86_ASIDPool_Assign(seL4_CPtr asid, seL4_CPtr ept)
{
    assert(rebuild_test && calls == 6u);
    assert(asid == AOS_X86_GUEST_ASID_POOL_CAP);
    assert(ept == AOS_GUEST_RAM_GUEST_VSPACE);
    return ++calls == fail_at ? 19 : seL4_NoError;
}
seL4_Error seL4_X86_EPTPDPT_Map(seL4_CPtr table, seL4_CPtr ept,
                              seL4_Word gpa, seL4_Word attr)
{
    assert(calls == 7u && table == AOS_X86_GUEST_EPT_PDPT_CAP);
    assert(ept == AOS_GUEST_RAM_GUEST_VSPACE && gpa == 0u);
    assert(attr == seL4_X86_EPT_Default_VMAttributes);
    return ++calls == fail_at ? 19 : seL4_NoError;
}
seL4_Error seL4_X86_EPTPD_Map(seL4_CPtr table, seL4_CPtr ept,
                            seL4_Word gpa, seL4_Word attr)
{
    assert(calls >= 8u && calls <= 10u);
    const seL4_CPtr directories[] = {AOS_X86_GUEST_EPT_LOW_PD_CAP,
        AOS_X86_GUEST_EPT_HIGH_PD_CAP, AOS_X86_GUEST_EPT_SECOND_RAM_PD_CAP};
    const seL4_Word addresses[] = {0u, 0xc0000000u, 0x40000000u};
    assert(table == directories[calls - 8u]);
    assert(gpa == addresses[calls - 8u]);
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
        for (fail_at = 0; fail_at <= AOS_X86_GUEST_OBJECT_COUNT; fail_at++) {
            calls = 0;
            seL4_Error err = aos_x86_guest_objects_retype(pool, 7u, slots);
            assert(err == (fail_at ? 19 : seL4_NoError));
            assert(calls == (fail_at ? fail_at : AOS_X86_GUEST_OBJECT_COUNT));
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
    for (fail_at = 0; fail_at <= 11u; fail_at++) {
        calls = 0;
        assert(aos_x86_guest_objects_rebuild() == (fail_at ? 19 : seL4_NoError));
        assert(calls == (fail_at ? fail_at : 11u));
    }
    for (bind_failure = 0; bind_failure <= 2; bind_failure++) {
        bind_calls = 0;
        assert(aos_x86_guest_objects_bind() == (bind_failure ? 19 : seL4_NoError));
        assert(bind_calls == (bind_failure ? bind_failure : 2u));
    }
    puts("PASS: x86 private objects, EPT reconstruction/binding failure propagation and bounded RAM/ROM grants");
}
