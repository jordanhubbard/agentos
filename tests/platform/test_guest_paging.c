#include <platform/guest_paging.h>
#include "contracts/guest_paging_caps.h"
#include <sel4/sel4.h>
#include <assert.h>
#include <stdio.h>

static unsigned calls, fail_at, tables, missing, maps;
static bool vspace, asid;
static bool fail(void) { return ++calls == fail_at; }
seL4_Error seL4_CNode_Revoke(seL4_CPtr root, seL4_Word slot, uint8_t depth)
{
    assert(root == AOS_GUEST_RAM_SELF_CNODE && slot == AOS_GUEST_PAGING_POOL_CAP);
    assert(depth == AOS_GUEST_RAM_CNODE_BITS);
    if (fail()) return 1;
    tables = 0;
    vspace = asid = false;
    return seL4_NoError;
}
seL4_Error seL4_Untyped_Retype(seL4_CPtr pool, seL4_Word type, seL4_Word bits,
    seL4_CPtr root, seL4_Word node, seL4_Word depth, seL4_Word slot, seL4_Word count)
{
    assert(pool == AOS_GUEST_PAGING_POOL_CAP && root == AOS_GUEST_RAM_SELF_CNODE);
    assert(!bits && !node && !depth && count == 1);
    if (fail()) return 1;
    if (type == seL4_ARM_VSpaceObject) {
        assert(!vspace && slot == AOS_GUEST_RAM_GUEST_VSPACE);
        vspace = true;
    } else {
        assert(type == seL4_ARM_PageTableObject && asid);
        assert(tables < AOS_GUEST_PAGING_TABLE_COUNT && slot == AOS_GUEST_PAGING_TABLE_BASE + tables);
        ++tables;
    }
    return seL4_NoError;
}
seL4_Error seL4_ARM_ASIDPool_Assign(seL4_CPtr pool, seL4_CPtr space)
{
    assert(pool == AOS_GUEST_ASID_POOL_CAP && space == AOS_GUEST_RAM_GUEST_VSPACE && vspace);
    if (fail()) return 1;
    asid = true;
    return seL4_NoError;
}
seL4_Error seL4_ARM_Page_Map(seL4_CPtr frame, seL4_CPtr space, seL4_Word va,
    seL4_Word rights, seL4_Word attr)
{
    assert(frame == AOS_GUEST_RAM_FRAME_BASE && space == AOS_GUEST_RAM_GUEST_VSPACE);
    assert(va == 0x40000000 && rights == seL4_AllRights && attr == seL4_ARM_Default_VMAttributes && asid);
    ++maps;
    if (fail()) return 1;
    return missing ? seL4_FailedLookup : seL4_NoError;
}
seL4_Error seL4_ARM_PageTable_Map(seL4_CPtr table, seL4_CPtr space,
    seL4_Word va, seL4_Word attr)
{
    assert(table == AOS_GUEST_PAGING_TABLE_BASE + tables - 1 && missing);
    assert(space == AOS_GUEST_RAM_GUEST_VSPACE && va == 0x40000000 && attr == seL4_ARM_Default_VMAttributes);
    if (fail()) return 1;
    --missing;
    return seL4_NoError;
}
static bool rebuild_and_map(void)
{
    return aos_vmm_guest_paging_release() && aos_vmm_guest_paging_rebuild() &&
        aos_vmm_guest_page_map(AOS_GUEST_RAM_FRAME_BASE, 0x40000000);
}
int main(void)
{
    for (unsigned levels = 0; levels <= 3; ++levels) {
        fail_at = calls = maps = 0;
        missing = levels;
        assert(rebuild_and_map() && tables == levels && maps == levels + 1);
        unsigned successful_calls = calls;
        for (unsigned failure = 1; failure <= successful_calls; ++failure) {
            fail_at = 0;
            assert(aos_vmm_guest_paging_release());
            calls = maps = 0;
            missing = levels;
            fail_at = failure;
            assert(!rebuild_and_map());
            fail_at = 0;
            missing = levels;
            assert(rebuild_and_map()); /* release resets partial allocation */
        }
    }
    fail_at = 0;
    assert(aos_vmm_guest_paging_release() && aos_vmm_guest_paging_rebuild());
    maps = 0;
    missing = 4;
    assert(!aos_vmm_guest_page_map(AOS_GUEST_RAM_FRAME_BASE, 0x40000000));
    assert(maps == 4 && tables == 3); /* bounded even on repeated lookup failure */
    assert(aos_vmm_guest_paging_release() && aos_vmm_guest_paging_rebuild());
    for (unsigned i = 0; i < AOS_GUEST_PAGING_TABLE_COUNT; ++i) {
        missing = 1;
        assert(aos_vmm_guest_page_map(AOS_GUEST_RAM_FRAME_BASE, 0x40000000));
    }
    missing = 1;
    assert(!aos_vmm_guest_page_map(AOS_GUEST_RAM_FRAME_BASE, 0x40000000));
    assert(tables == AOS_GUEST_PAGING_TABLE_COUNT);
    missing = 0;
    assert(aos_vmm_guest_page_map(AOS_GUEST_RAM_FRAME_BASE, 0x40000000));
    assert(aos_vmm_guest_paging_release());
    puts("PASS: private paging rebuild, ASID assignment, bounded table allocation and failure recovery");
}
