/* Host test: seL4 object type -> agentOS authority kind mapping. */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include "platform/authority.h"

uint32_t aos_authority_kind_from_sel4(uint32_t obj_type);

/* Mirrors of the seL4 constants the root task maps. The implementation must
 * use the real seL4 enum; these values are supplied by the test harness via
 * -D so the two cannot drift silently. */
int main(void)
{
    assert(aos_authority_kind_from_sel4(AOSTEST_SEL4_UNTYPED)   == AOS_AUTHORITY_KIND_UNTYPED);
    assert(aos_authority_kind_from_sel4(AOSTEST_SEL4_TCB)       == AOS_AUTHORITY_KIND_TCB);
    assert(aos_authority_kind_from_sel4(AOSTEST_SEL4_ENDPOINT)  == AOS_AUTHORITY_KIND_ENDPOINT);
    assert(aos_authority_kind_from_sel4(AOSTEST_SEL4_NOTIFICATION) == AOS_AUTHORITY_KIND_NOTIFICATION);
    assert(aos_authority_kind_from_sel4(AOSTEST_SEL4_CNODE)     == AOS_AUTHORITY_KIND_CNODE);

    /* Anything unmapped must bucket as OTHER, never vanish. */
    assert(aos_authority_kind_from_sel4(0xFFFFu) == AOS_AUTHORITY_KIND_OTHER);

    printf("test_authority_kindmap: PASS\n");
    return 0;
}
