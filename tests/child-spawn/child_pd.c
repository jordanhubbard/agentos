/*
 * child_pd.c — child-spawn demonstration pair: child side (test image
 * only).
 *
 * This file is NOT a system_desc_aarch64.c row and is never loaded by the
 * root task's normal per-PD boot loop. It is compiled and linked through
 * the ordinary $(CC)/$(LD) pipeline, at the same tools/ld/agentos.ld link
 * address every other service PD uses, producing a small standalone ELF
 * whose loadable bytes the Makefile then extracts (objcopy -O binary) and
 * links directly into child_spawn_parent's OWN ELF as a read-only data
 * blob (see the Makefile's AGENTOS_CHILD_SPAWN_TEST rules and
 * child_spawn.h's file header on why: a byte array inside the PARENT's own
 * ELF is already covered by the SAME T3 bundle verification that covers
 * the rest of the parent's binary, with no new loading path). parent_pd.c
 * retypes a fresh VSpace and TCB at run time via aos_child_spawn() and
 * maps these exact bytes in at AOS_CHILD_SPAWN_CONTENT_VA -- this file
 * never runs until the parent explicitly spawns it.
 *
 * ── DO NOT link this against the shared pd_entry.o ──────────────────────
 *
 * This PD's link line is deliberately NOT the one every other service PD
 * uses, and "tidying" it back to the common one reintroduces a bug that
 * survived two reviews. See the child_spawn_child.elf rule in
 * kernel/agentos-root-task/Makefile. Three differences, all load-bearing:
 *
 *   1. It links $(BUILD_DIR)/child_spawn_pd_entry.o, built from the same
 *      pd_entry.c but with -DAGENTOS_LOG_RINGS=1 FILTERED OUT. Under that
 *      define, _start() UNCONDITIONALLY dereferences AOS_LOG_CONFIG_VA
 *      (0x1000a000) before it can even decide whether log rings exist, and
 *      conditionally writes log_drain_rings_vaddr, which the linker places
 *      in .bss at 0x401000. Root maps the log-config page into every PD it
 *      creates at boot. NOBODY maps it into this one: this domain holds
 *      exactly what its parent endowed it, and log rings are not on that
 *      list. Linking the shared object makes this PD fault inside _start()
 *      before pd_main() ever runs -- which is the endowment boundary doing
 *      its job, and is why the fix is to stop linking authority-assuming
 *      startup code into it rather than to widen the endowment.
 *   2. It does not link rust_pd_memory.o (~3 KiB of memcpy/memmove/memset/
 *      memcmp/strlen this PD never calls).
 *   3. It links --nmagic, so sel4_crt.c's unused 1 KiB IPC-buffer
 *      placeholder in .bss is not pushed past the mapped page by default
 *      4 KiB segment alignment.
 *
 * (2) and (3) exist because the parent maps exactly ONE page for this
 * image; the Makefile's `_end <= 0x401000` check enforces that and will
 * fail the build if this file outgrows it. That check does NOT backstop
 * (1): a log-rings regression is a touch of an unmapped VA, not a size
 * overrun, so it builds cleanly and shows up only as a 300-second timeout
 * with Probe 1's marker missing, on an image only CI's os-claim-gate job
 * ever builds.
 *
 * This PD holds exactly what its parent endowed it with and nothing else:
 * its own image, stack and IPC buffer pages; a Signal-only derivative of
 * a Notification; and a read/write derivative of ONE frame capability,
 * which is also mapped at AOS_CHILD_SPAWN_GIFT_VA. There is no serial
 * capability -- it cannot print, so everything it proves reaches the boot
 * log through the parent (Probe 1) or through the root task's fault
 * oracle (Probe 2).
 *
 * What it does, in order:
 *
 *   1. Reads the exact 16-byte pattern the parent wrote into the endowed
 *      frame before this domain existed. A mismatch means this is not the
 *      memory the parent meant to grant, and the child stops: it never
 *      signals, the parent never prints its OK marker, and the test fails
 *      rather than passing on a coincidence.
 *   2. Invokes the endowed FRAME CAPABILITY itself -- not the mapping --
 *      with seL4_ARM_Page_GetAddress, and writes the physical address it
 *      reports into the frame along with a magic word and the complement
 *      of each pattern word. The parent checks that physical address
 *      against the one its OWN capability to the same object reports:
 *      that is what makes Probe 1 "the child used the capability it was
 *      given" rather than merely "something wrote to a page".
 *   3. Signals the endowed Notification -- the one capability that lets
 *      this domain say anything at all to anyone.
 *   4. Reads AOS_CHILD_SPAWN_WITHHELD_VA, which its parent deliberately
 *      never mapped and never endowed. This faults, by design: it is
 *      Probe 2, and the root task verifies the exact badge, address and
 *      direction. The read is through a volatile pointer so the compiler
 *      cannot elide it, and nothing after it is ever reached.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <sel4/sel4.h>

#include "contracts/child_spawn_contract.h"

static void park(void)
{
    for (;;) {
        __asm__ volatile("wfe" ::: "memory");
    }
}

/*
 * pd_main — called by pd_entry.c's _start(my_ep, ns_ep).
 *
 * The parent passes the two endowed CNode slots as arg0/arg1 (my_ep/ns_ep
 * in the normal convention -- here they name a Notification and a Frame,
 * not endpoints, but the convention is the same: arg0/arg1 are plain CNode
 * slot indices already populated in this PD's own CNode by whoever created
 * it, exactly as for every other service PD). Using the register values
 * rather than the contract constants directly means that if the parent's
 * layout and this file's constants ever drift apart, this PD uses what the
 * parent actually intended.
 */
void pd_main(seL4_CPtr ntfn_slot, seL4_CPtr frame_slot)
{
    /* Step 1: the endowed frame really is the memory the parent granted. */
    volatile const uint64_t *gift = (volatile const uint64_t *)AOS_CHILD_SPAWN_GIFT_VA;
    if (gift[0] != AOS_CHILD_SPAWN_PATTERN0 || gift[1] != AOS_CHILD_SPAWN_PATTERN1) {
        park();
    }

    /* Step 2: invoke the endowed frame CAPABILITY, and answer through the
     * frame. A mint-derived frame capability carries the mapping its
     * source had at mint time (the parent maps this frame into this
     * domain's VSpace before minting -- see aos_child_spawn()'s ordering),
     * but seL4_ARM_Page_GetAddress needs no mapping at all: it needs the
     * capability, which is exactly the point. */
    seL4_ARM_Page_GetAddress_t pa = seL4_ARM_Page_GetAddress(frame_slot);
    if (pa.error != seL4_NoError) {
        park();
    }
    volatile uint64_t *resp =
        (volatile uint64_t *)(AOS_CHILD_SPAWN_GIFT_VA + AOS_CHILD_SPAWN_RESP_OFF);
    resp[0] = AOS_CHILD_SPAWN_RESP_MAGIC;
    resp[1] = (uint64_t)pa.paddr;
    resp[2] = ~(uint64_t)AOS_CHILD_SPAWN_PATTERN0;
    resp[3] = ~(uint64_t)AOS_CHILD_SPAWN_PATTERN1;

    /* Step 3: the one way this domain can reach the outside world. */
    seL4_Signal(ntfn_slot);

    /* Step 4 (Probe 2): the parent withheld this page. Reading it must
     * fault. volatile so the read is really issued; the value is never
     * used because control never comes back. */
    seL4_Word withheld = *(volatile seL4_Word *)AOS_CHILD_SPAWN_WITHHELD_VA;
    (void)withheld;

    /* Unreachable: the read above faults and this domain has no fault
     * handler of its own to resume it. Parking rather than returning keeps
     * the "unreachable" honest if a future change ever makes it reachable. */
    park();
}
