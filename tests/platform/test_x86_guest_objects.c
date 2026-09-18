#include <assert.h>
#include <stdio.h>
#include "x86_guest_objects.h"

static unsigned calls, fail_at;
static seL4_CPtr pool;
static const seL4_CPtr slots[] = {80u, 91u, 102u, 117u, 133u};
static const seL4_Word expected_types[] = {
    seL4_X86_VCPUObject, seL4_X86_EPTPML4Object,
    seL4_X86_EPTPDPTObject, seL4_X86_EPTPDObject, seL4_X86_EPTPDObject,
};

seL4_Error seL4_Untyped_Retype(seL4_CPtr source, seL4_Word type,
    seL4_Word bits, seL4_CPtr root, seL4_Word node,
    seL4_Word depth, seL4_Word offset, seL4_Word count)
{
    assert(calls < AOS_X86_GUEST_OBJECT_COUNT);
    assert(source == pool && root == 7u);
    assert(type == expected_types[calls] && offset == slots[calls]);
    assert(bits == 0u && node == 0u && depth == 0u && count == 1u);
    calls++;
    return calls == fail_at ? 19 : seL4_NoError;
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
    puts("PASS: x86 VCPU/EPT private allocation sources and every retype failure");
}
