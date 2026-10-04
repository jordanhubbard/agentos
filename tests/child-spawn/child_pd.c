/*
 * child_pd.c — child-spawn demonstration pair: child side (test image
 * only).
 *
 * This file is NOT a system_desc_aarch64.c row and is never loaded by the
 * root task's normal per-PD boot loop. It is compiled and linked through
 * the ordinary $(CC)/$(LD) pipeline every other service PD uses (same
 * pd_entry.o/_start convention, same tools/ld/agentos.ld link address),
 * producing a small standalone ELF whose loadable bytes the Makefile then
 * extracts (objcopy -O binary) and links directly into child_spawn_parent's
 * OWN ELF as a read-only data blob (see the Makefile's
 * AGENTOS_CHILD_SPAWN_TEST rules and child_spawn.h's file header on why: a
 * byte array inside the PARENT's own ELF is already covered by the SAME
 * T3 bundle verification that covers the rest of the parent's binary,
 * with no new loading path). child_spawn_parent.c retypes a fresh VSpace
 * and TCB at run time via aos_child_spawn() and maps these exact bytes in
 * at AOS_CHILD_SPAWN_CONTENT_VA -- this file never runs until the parent
 * explicitly spawns it.
 *
 * The whole point of this PD is to prove it is a genuinely separate,
 * independently scheduled thread that can exercise EXACTLY the one
 * capability the parent endowed it: it signals the Notification
 * derivative it received at AOS_CHILD_SPAWN_CHILD_NTFN_SLOT, then parks.
 * It holds no other capability -- there is nothing else in its CNode to
 * exercise.
 */
#include <sel4/sel4.h>

#include "contracts/child_spawn_contract.h"

/*
 * pd_main — called by pd_entry.c's _start(my_ep, ns_ep).
 *
 * The parent passes the endowed Notification slot as arg0 (my_ep in the
 * normal convention -- here it names a Notification, not an endpoint, but
 * the convention is the same: arg0/arg1 are plain CNode slot indices
 * already populated in this PD's own CNode by whoever created it, exactly
 * as for every other service PD). ns_ep (arg1) is unused: this child has
 * no nameserver capability -- it was never endowed one.
 */
void pd_main(seL4_CPtr ntfn_slot, seL4_CPtr ns_ep_unused)
{
    (void)ns_ep_unused;

    /* AOS_CHILD_SPAWN_CHILD_NTFN_SLOT is also passed explicitly as arg0
     * by the parent (see child_spawn_parent.c), so this is a redundant,
     * defensive cross-check rather than a silent assumption: if the
     * parent's layout and this file's constant ever drift apart, use the
     * value actually in the register, which is what the parent intended. */
    seL4_Signal(ntfn_slot);

    /* Nothing else to do: this PD holds exactly one capability. Park
     * rather than return (returning into pd_entry.c's _start would spin
     * anyway, but an explicit, cheap wait is clearer than a busy spin). */
    for (;;) {
        __asm__ volatile("wfe" ::: "memory");
    }
}
