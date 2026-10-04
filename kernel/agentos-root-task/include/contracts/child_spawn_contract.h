/*
 * child_spawn_contract.h — slot/VA layout for the child-spawn demonstration
 * pair (test image only).
 *
 * Built only when AGENTOS_CHILD_SPAWN_TEST is defined (see
 * kernel/agentos-root-task/Makefile and system_desc_aarch64.c); absent
 * from the default PD set and from every other focused test-image variant
 * (AGENTOS_SEL4_TEST_IMAGE / AGENTOS_NATIVE_RUST_TEST /
 * AGENTOS_FRAMEBUFFER_TEST / AGENTOS_CAP_LEND_TEST), same mutual-exclusion
 * discipline the Makefile already enforces for those.
 *
 * ONE PD, child_spawn_parent, exercises libs/pd-support/child_spawn.c
 * end-to-end: at run time it creates a SECOND domain (not a system_desc
 * row -- see child_spawn.h's file header on why the child's code is
 * instead linked as a raw binary blob into the parent's own ELF), endows
 * it from its own authority, starts it, and observes what it does with
 * what it was given. The four probes T6 Task 3 asserts on target are:
 *
 *   Probe 1 -- the child USES its endowment. It reads an exact 16-byte
 *     pattern the parent wrote into an endowed frame, invokes the endowed
 *     FRAME CAPABILITY itself (seL4_ARM_Page_GetAddress, which needs the
 *     capability, not just the mapping), writes a response back through
 *     the mapping, and only then signals the parent through the endowed
 *     Signal-only Notification derivative. The parent re-maps its own
 *     capability to that same frame and checks the response byte for
 *     byte, including that the physical address the CHILD reported
 *     matches the one the PARENT sees -- proof the child held a
 *     capability to that exact frame, not merely a window onto memory.
 *
 *   Probe 2 -- the child cannot EXCEED its endowment. Its last act is a
 *     read at AOS_CHILD_SPAWN_WITHHELD_VA, a page the parent deliberately
 *     never mapped and never endowed. The child faults, and the ROOT TASK
 *     -- not the parent, not the child -- verifies the exact badge,
 *     address and direction through its own fault-probe oracle (main.c's
 *     ROOT_PROBE_* block). A timeout or an unrelated fault cannot satisfy
 *     it.
 *
 *   Probe 3 -- a failed endowment leaves nothing running. The parent
 *     performs a FIRST, deliberately doomed spawn whose endowment names
 *     AOS_CHILD_SPAWN_ABSENT_SLOT, a slot in the parent's own CNode that
 *     is permanently empty. Task 1's validator passes it (the descriptor
 *     is internally consistent -- the parent's holdings declaration is a
 *     claim, not a measurement), the first mint succeeds, and the second
 *     fails in the kernel. aos_child_spawn() then destroys the child's
 *     CNode, taking the already-successful mint with it, and never
 *     resumes the TCB. The parent asserts the error, that the staging
 *     range was fully torn down, that no signal ever arrived, and that
 *     the ledger below is still empty.
 *
 *   Probe 4 -- the ledger reports the child. aos_child_spawn() appends to
 *     an endowment-delta ledger (platform/endow_ledger.h) on each
 *     successful mint; the parent merges it into a T4 authority snapshot
 *     and prints it. This is a REPORT, not a proof -- see
 *     platform/endow_ledger.h.
 *
 * Slot numbers below start at 44, clear of every PD_CNODE_SLOT_* constant
 * system_desc.h defines (system_desc.h's own values go up to 47, but each
 * is only ever populated by root for a PD of the matching role --
 * net_virt, input_virt, entropy_pd, etc. -- never for child_spawn_parent,
 * so reusing the number range is safe). The scratch range
 * (AOS_CHILD_SPAWN_SCRATCH_BASE..+COUNT) extends past
 * PD_IRQHANDLER_SLOT_BASE (64), which is likewise safe: that base only
 * matters for a PD with irq_count > 0, and child_spawn_parent has none.
 */
#pragma once

#include <platform/authority.h> /* aos_authority_kind_t -- see the KIND_*
                                 * constants near the bottom of this file. */

/* ── Boot-time grants to child_spawn_parent (main.c's
 * AGENTOS_CHILD_SPAWN_TEST provisioning block) ─────────────────────────── */

/* Self-reference to the parent's own CNode, same pattern as
 * AOS_CAP_LEND_SELF_CNODE_SLOT / AOS_GUEST_RAM_SELF_CNODE elsewhere. */
#define AOS_CHILD_SPAWN_SELF_CNODE_SLOT      44u

/* Self-reference to the parent's own TCB -- this PD's own mcp is 255 (set
 * by root at boot, same as every PD -- see pd_tcb.c), so the parent can
 * use this as the SetSchedParams/SetPriority authority when creating its
 * child's TCB, exactly as root uses seL4_CapInitThreadTCB for every PD. */
#define AOS_CHILD_SPAWN_SELF_TCB_SLOT         45u

/* Untyped pool the parent retypes the child's CNode/VSpace/page tables/
 * frames/TCB/SchedContext from, AND the notification object and the two
 * frames the parent creates for itself before calling aos_child_spawn().
 * Nothing the parent retypes at run time comes from anywhere else. */
#define AOS_CHILD_SPAWN_POOL_SLOT             46u

/* Size, in bits, of the untyped pool granted at AOS_CHILD_SPAWN_POOL_SLOT.
 * 1 MiB: generous headroom over the TWO spawns this demo performs (the
 * deliberately failed one for Probe 3 and the successful one for Probes
 * 1/2/4), each consuming one CNode, one VSpace, up to ~12 page-table
 * objects across four mappings, two leaf frames, one TCB and one
 * SchedContext -- plus the parent's own notification and two frames.
 * seL4_Untyped_Retype is a bump allocator with no free, so the failed
 * spawn's objects are NOT returned to the pool (see child_spawn.h's
 * "Untyped exhaustion"); the headroom here is what makes a second spawn
 * after a failed one possible at all. Review Focus item 3 (exhaustion
 * mid-creation) is about HANDLING running out, not about this demo
 * actually hitting the limit. */
#define AOS_CHILD_SPAWN_POOL_BITS             20u

/* ASID-pool slice for assigning the child's freshly retyped VSpace an
 * ASID -- not Untyped-derived, so not part of the pool above (same
 * reasoning as AOS_GUEST_ASID_POOL_CAP for the guest-RAM VMM). */
#define AOS_CHILD_SPAWN_ASID_POOL_SLOT        47u

/* SchedControl capability (this core) for configuring the child's
 * SchedContext -- MCS kernels only, not Untyped-derived. */
#define AOS_CHILD_SPAWN_SCHEDCONTROL_SLOT     48u

/* Slot, in the PARENT's own CNode, where the parent retypes the
 * Notification object it endows a Signal-only derivative of into the
 * child. */
#define AOS_CHILD_SPAWN_PARENT_NTFN_SLOT      49u

/* Self-reference to the parent's own VSpace -- used ONLY by
 * child_spawn_parent.c itself (not by aos_child_spawn(), which never
 * touches the parent's own address space) to temporarily map a freshly
 * retyped frame into its OWN VSpace long enough to write the child's
 * image bytes (or the Probe 1 pattern) in, before handing that frame to
 * aos_child_spawn() to map into the CHILD's VSpace. Same self-reference
 * pattern as AOS_CAP_LEND_SELF_VSPACE_SLOT. */
#define AOS_CHILD_SPAWN_SELF_VSPACE_SLOT      50u

/* Slot, in the parent's own CNode, for the content frame the parent
 * retypes from its pool and populates before calling aos_child_spawn().
 * Outside the scratch range: this object's lifetime is the parent's
 * choice, not aos_child_spawn()'s -- the library only maps it, never
 * retypes or deletes it. */
#define AOS_CHILD_SPAWN_CONTENT_FRAME_SLOT    51u

/* Badged copy of the root task's fault endpoint, granted to the parent so
 * it can install it as its CHILD's fault handler (aos_child_spawn_req_t.
 * fault_ep). Without this the child's Probe 2 fault would be unhandled
 * and invisible: seL4 would simply leave the thread faulted, and the
 * root task -- the independent observer the proof relies on -- would
 * never see it. The parent cannot forge this: root mints it with
 * AOS_CHILD_SPAWN_PROBE_BADGE, and the badge is what the oracle matches
 * on, so the parent could not substitute some other endpoint and still
 * produce the marker. */
#define AOS_CHILD_SPAWN_FAULT_EP_SLOT         52u

/* The parent's ORIGINAL capability to the frame it endows the child with
 * (Probe 1). The parent keeps this one so that, after the child has
 * signalled, it can map the SAME physical frame into its own VSpace again
 * and read the child's response: a frame capability records at most one
 * mapping, so the capability the child's mapping was made from has to be
 * a different one -- hence the copy below. */
#define AOS_CHILD_SPAWN_GIFT_FRAME_SLOT       53u

/* The copy of the gift frame capability that is actually mapped into the
 * child's VSpace and minted (rights-reduced) into the child's CNode.
 * Deriving the child's authority from a COPY rather than from the
 * parent's original is not a security property -- both name the same
 * object and seL4 treats them identically -- it is purely so the parent
 * retains an independently mappable capability to read the response
 * through. */
#define AOS_CHILD_SPAWN_GIFT_COPY_SLOT        54u

/* Permanently EMPTY slot in the parent's own CNode. Probe 3's doomed
 * spawn declares (falsely) that it holds a capability here and asks to
 * endow a derivative of it. Task 1's validator cannot catch that -- it
 * compares a request against a caller-asserted holdings declaration, and
 * both sides of this one agree -- so the lie survives until
 * seL4_CNode_Mint resolves the slot and fails. That is exactly the point:
 * the kernel, not the declaration, is the enforcement boundary. Nothing
 * ever places a capability here. */
#define AOS_CHILD_SPAWN_ABSENT_SLOT           55u

/* First of AOS_CHILD_SPAWN_SCRATCH_COUNT (>= AOS_CHILD_SPAWN_SCRATCH_SLOTS
 * from child_spawn.h) contiguous FREE slots in the parent's own CNode
 * that aos_child_spawn() may stage new objects into. */
#define AOS_CHILD_SPAWN_SCRATCH_BASE          56u
#define AOS_CHILD_SPAWN_SCRATCH_COUNT         24u

/* Scratch VA, in the PARENT's OWN VSpace, used only transiently to write
 * the child's image bytes and the Probe 1 pattern into freshly retyped
 * frames before they are unmapped from the parent and handed to the
 * child, and once more at the end to read the child's response back.
 * Chosen clear of every fixed VA a normal service PD already uses
 * (0x400000 own image, 0x10000000 own IPC buffer, plus whatever
 * pd_vspace.c placed its own stack at). */
#define AOS_CHILD_SPAWN_PARENT_SCRATCH_VA  0x20000000UL

/* Radix, in bits, of child_spawn_parent's own CNode (pd_desc_t.
 * cnode_size_bits in system_desc_aarch64.c). */
#define AOS_CHILD_SPAWN_PARENT_CNODE_BITS      8u

/* First of 4 slots, just past aos_child_spawn()'s own scratch range, that
 * child_spawn_parent.c uses ONLY for itself: retyping the intermediate
 * page-table objects AOS_CHILD_SPAWN_PARENT_SCRATCH_VA needs in the
 * PARENT's OWN VSpace before it can map a frame there. Root's normal
 * per-PD VSpace construction (pd_vspace_create) only covers a PD's ELF
 * image, stack, and IPC buffer regions -- an address the PD picks for
 * itself at run time, like this scratch VA, has no page table until
 * something retypes one, the same reason aos_child_spawn() retries its
 * own mappings on seL4_FailedLookup (see child_spawn.c's
 * aos_pt_map_retrying). Only the FIRST mapping at this VA consumes any of
 * these slots; later map/unmap cycles at the same VA reuse the page
 * tables already installed. */
#define AOS_CHILD_SPAWN_PARENT_SCRATCH_PT_BASE 80u

/*
 * Pin the whole fixed-slot layout at compile time, same style
 * system_desc.h:305 already uses for its own slot ordering. This is
 * exactly the class of bug the implementer hit by booting (self-ref and
 * content-frame slots originally overlapped the scratch range) --
 * without these asserts, nothing notices if a future edit renumbers one
 * constant and creates a silent overlap again.
 */
_Static_assert(AOS_CHILD_SPAWN_SELF_TCB_SLOT > AOS_CHILD_SPAWN_SELF_CNODE_SLOT,
               "child-spawn fixed slots must be strictly ordered");
_Static_assert(AOS_CHILD_SPAWN_POOL_SLOT > AOS_CHILD_SPAWN_SELF_TCB_SLOT,
               "child-spawn fixed slots must be strictly ordered");
_Static_assert(AOS_CHILD_SPAWN_ASID_POOL_SLOT > AOS_CHILD_SPAWN_POOL_SLOT,
               "child-spawn fixed slots must be strictly ordered");
_Static_assert(AOS_CHILD_SPAWN_SCHEDCONTROL_SLOT > AOS_CHILD_SPAWN_ASID_POOL_SLOT,
               "child-spawn fixed slots must be strictly ordered");
_Static_assert(AOS_CHILD_SPAWN_PARENT_NTFN_SLOT > AOS_CHILD_SPAWN_SCHEDCONTROL_SLOT,
               "child-spawn fixed slots must be strictly ordered");
_Static_assert(AOS_CHILD_SPAWN_SELF_VSPACE_SLOT > AOS_CHILD_SPAWN_PARENT_NTFN_SLOT,
               "child-spawn fixed slots must be strictly ordered");
_Static_assert(AOS_CHILD_SPAWN_CONTENT_FRAME_SLOT > AOS_CHILD_SPAWN_SELF_VSPACE_SLOT,
               "child-spawn fixed slots must be strictly ordered");
_Static_assert(AOS_CHILD_SPAWN_FAULT_EP_SLOT > AOS_CHILD_SPAWN_CONTENT_FRAME_SLOT,
               "child-spawn fixed slots must be strictly ordered");
_Static_assert(AOS_CHILD_SPAWN_GIFT_FRAME_SLOT > AOS_CHILD_SPAWN_FAULT_EP_SLOT,
               "child-spawn fixed slots must be strictly ordered");
_Static_assert(AOS_CHILD_SPAWN_GIFT_COPY_SLOT > AOS_CHILD_SPAWN_GIFT_FRAME_SLOT,
               "child-spawn fixed slots must be strictly ordered");
_Static_assert(AOS_CHILD_SPAWN_ABSENT_SLOT > AOS_CHILD_SPAWN_GIFT_COPY_SLOT,
               "child-spawn fixed slots must be strictly ordered");
_Static_assert(AOS_CHILD_SPAWN_SCRATCH_BASE > AOS_CHILD_SPAWN_ABSENT_SLOT,
               "aos_child_spawn()'s own scratch range must start strictly "
               "above every fixed boot-time grant slot, or its internal "
               "retypes will clobber a capability child_spawn_parent.c "
               "still needs");
_Static_assert(AOS_CHILD_SPAWN_PARENT_SCRATCH_PT_BASE >=
               AOS_CHILD_SPAWN_SCRATCH_BASE + AOS_CHILD_SPAWN_SCRATCH_COUNT,
               "child_spawn_parent.c's own page-table scratch range must "
               "not overlap aos_child_spawn()'s internal scratch range");
_Static_assert(AOS_CHILD_SPAWN_PARENT_SCRATCH_PT_BASE + 4u <
               (1u << AOS_CHILD_SPAWN_PARENT_CNODE_BITS),
               "child_spawn_parent's whole slot layout must fit inside its "
               "own CNode");


/* ── Child-side layout (chosen by the parent, not granted by root --
 * the child does not exist until the parent creates it) ───────────────── */

/* Radix, in bits, of the CHILD's own CNode. Small: it holds only the MCS
 * reply slot (9, reserved -- see system_desc.h) and its two endowed
 * capabilities. */
#define AOS_CHILD_SPAWN_CHILD_CNODE_BITS       6u

/* Slots in the CHILD's own CNode where the endowed derivatives land.
 * aos_child_spawn() places endowment entry i at
 * req->endow_child_base_slot + i, so these must be consecutive and must
 * match the order of the aos_endowment_t entries in parent_pd.c. */
#define AOS_CHILD_SPAWN_CHILD_NTFN_SLOT       10u
#define AOS_CHILD_SPAWN_CHILD_FRAME_SLOT      11u

_Static_assert(AOS_CHILD_SPAWN_CHILD_FRAME_SLOT ==
               AOS_CHILD_SPAWN_CHILD_NTFN_SLOT + 1u,
               "endowed child slots are assigned consecutively from "
               "endow_child_base_slot, in aos_endowment_t order");
_Static_assert(AOS_CHILD_SPAWN_CHILD_FRAME_SLOT <
               (1u << AOS_CHILD_SPAWN_CHILD_CNODE_BITS),
               "endowed child slots must fit inside the child's CNode");

/* VA, in the child's freshly built VSpace, of its one content page --
 * also its entry point. Matches the standard PD link address
 * (tools/ld/agentos.ld's `. = 0x400000`) purely so the payload blob is
 * linked at the address it will actually run at; this has nothing to do
 * with the real PD-loading path, which this child never goes through. */
#define AOS_CHILD_SPAWN_CONTENT_VA     0x400000UL
#define AOS_CHILD_SPAWN_ENTRY_VA       0x400000UL

/* VA of the child's stack: top of a single fresh page, far from the
 * content page so the two mappings' page-table retypes are independent. */
#define AOS_CHILD_SPAWN_STACK_VA_TOP   0x500000UL

/* VA, in the CHILD's VSpace, of the endowed "gift" frame (Probe 1). The
 * parent maps it there through aos_child_spawn_req_t.extra_maps, BEFORE
 * the child is started, so the child's whole endowment -- mapped memory
 * and minted capabilities alike -- is complete before it can execute a
 * single instruction. */
#define AOS_CHILD_SPAWN_GIFT_VA        0x600000UL

/* VA, in the CHILD's VSpace, that the parent deliberately NEVER maps and
 * never endows (Probe 2). The child's last act is a read here. It must be
 * page-aligned and must be the exact address the root task's fault oracle
 * compares against (main.c's ROOT_PROBE_ADDRESS): on AArch64 the VMFault
 * message reports the faulting address exactly, so an off-by-anything
 * read would not satisfy the probe. */
#define AOS_CHILD_SPAWN_WITHHELD_VA    0x700000UL

_Static_assert(AOS_CHILD_SPAWN_GIFT_VA != AOS_CHILD_SPAWN_WITHHELD_VA &&
               AOS_CHILD_SPAWN_GIFT_VA != AOS_CHILD_SPAWN_CONTENT_VA &&
               AOS_CHILD_SPAWN_WITHHELD_VA != AOS_CHILD_SPAWN_STACK_VA_TOP,
               "the withheld page must not collide with anything the child "
               "was actually given, or Probe 2 proves nothing");

/* VA of the child's IPC buffer. MUST be 0x10000000 -- this is not a free
 * choice: the child is linked against the SAME pd_entry.c every other
 * service PD uses (see tests/child-spawn/child_pd.c), and pd_entry.c's
 * _start() unconditionally redirects __sel4_ipc_buffer to
 * PD_IPC_BUF_VA == 0x10000000 before calling pd_main(). Mapping the IPC
 * frame anywhere else would leave the child's IPC buffer pointer aimed at
 * a page that was never mapped. */
#define AOS_CHILD_SPAWN_IPC_BUF_VA     0x10000000UL

/* Priority the parent grants its child. Below the parent's own so the
 * parent (which must still run to observe the child's signal) is never
 * starved by it. */
#define AOS_CHILD_SPAWN_CHILD_PRIORITY         50u

/* Badge the parent mints the child's endowed capabilities with. */
#define AOS_CHILD_SPAWN_BADGE              0xC417u

/* Badge root mints AOS_CHILD_SPAWN_FAULT_EP_SLOT with, and the only badge
 * main.c's fault oracle accepts for this probe. Distinct from
 * AOS_CHILD_SPAWN_BADGE so a fault arriving on the wrong endpoint cannot
 * be confused with the child's own endowment badge. */
#define AOS_CHILD_SPAWN_PROBE_BADGE        0xC418u

_Static_assert(AOS_CHILD_SPAWN_PROBE_BADGE != AOS_CHILD_SPAWN_BADGE,
               "the fault-probe badge must be distinguishable from the "
               "endowment badge");

/* ── Probe 1 payload ────────────────────────────────────────────────────
 *
 * The parent writes PATTERN0/PATTERN1 at offsets 0 and 8 of the gift
 * frame before the child exists. The child checks both for exact
 * equality, and only then writes its response at
 * AOS_CHILD_SPAWN_RESP_OFF: a magic word, the physical address
 * seL4_ARM_Page_GetAddress returned for its OWN endowed frame CAPABILITY,
 * and the bitwise complement of each pattern word. The parent verifies
 * all four, comparing the reported physical address against the one its
 * own capability reports -- which is what makes this "the child used the
 * capability we gave it" rather than merely "something wrote to a page".
 */
#define AOS_CHILD_SPAWN_PATTERN0   0xA6C417D300F0E1D2ULL
#define AOS_CHILD_SPAWN_PATTERN1   0x5A3BE82CFF0F1E2DULL
#define AOS_CHILD_SPAWN_RESP_OFF   2048u
#define AOS_CHILD_SPAWN_RESP_MAGIC 0xC417C417C417C417ULL

_Static_assert(AOS_CHILD_SPAWN_RESP_OFF >= 16u &&
               AOS_CHILD_SPAWN_RESP_OFF + 32u <= 4096u,
               "the child's response must not overlap the pattern and must "
               "stay inside the one endowed page");

/* ── Endowment kinds ────────────────────────────────────────────────────
 *
 * contracts/endowment_contract.h leaves aos_endow_cap_t.kind a
 * caller-defined enumeration compared only for equality. This demo
 * numbers its kinds as aos_authority_kind_t values so the
 * endowment-delta ledger (platform/endow_ledger.h) can fold them straight
 * into a T4 authority snapshot without a second translation table that
 * could silently disagree with authority_kindmap.c.
 */
#define AOS_CHILD_SPAWN_KIND_NOTIFICATION  ((uint32_t)AOS_AUTHORITY_KIND_NOTIFICATION)
#define AOS_CHILD_SPAWN_KIND_FRAME         ((uint32_t)AOS_AUTHORITY_KIND_FRAME)

/* How the child appears in the parent's endowment-delta ledger, and
 * therefore in the merged authority report Probe 4 asserts on. The index
 * is the PARENT's own numbering of its children -- nothing outside the
 * parent assigns or validates it (see platform/endow_ledger.h). */
#define AOS_CHILD_SPAWN_CHILD_INDEX        1u
#define AOS_CHILD_SPAWN_CHILD_NAME         "child_spawn_child"

/* Greppable boot-log markers (see cap_lend_test.h for why these exist: a
 * mint/map/configure/endow failure and success are otherwise
 * indistinguishable in the boot log, which would make any later proof
 * vacuous). child_spawn_parent emits these over the normal serial channel
 * every PD already has (SVC_ID_SERIAL init EP); the child has no serial
 * capability of its own -- it was never endowed one -- so everything the
 * child proves reaches the log through the parent or, for Probe 2,
 * through the root task. */

/* Probe 3: the doomed spawn was refused at the endowment step and left
 * nothing running. */
#define AOS_CHILD_SPAWN_MARKER_ENDOW_FAIL \
    "[child-spawn-parent] OK: endowment failed, child never started, ledger empty\n"

/* Probe 1: the child read the exact pattern, invoked its endowed frame
 * capability, and signalled back through its endowed notification; the
 * parent verified the response byte for byte. */
#define AOS_CHILD_SPAWN_MARKER_OK \
    "[child-spawn-parent] OK: child used endowment, response and paddr verified\n"

/* Probe 4: the endowment-delta ledger, rendered through T4's authority
 * snapshot formatter. The line between these two markers is the actual
 * aos_authority_format() output and is what the test asserts on. */
#define AOS_CHILD_SPAWN_MARKER_LEDGER_BEGIN \
    "[child-spawn-parent] endowment delta (report, not proof; seL4 has no cap enumeration):\n"
#define AOS_CHILD_SPAWN_MARKER_LEDGER_OK \
    "[child-spawn-parent] OK: ledger reports 1 child, 2 endowed capabilities\n"

#define AOS_CHILD_SPAWN_MARKER_FAIL_SPAWN \
    "[child-spawn-parent] FAIL: aos_child_spawn\n"

/* Probe 2's marker is emitted by the ROOT TASK's fault oracle, never by
 * the parent or the child (main.c's AGENTOS_CHILD_SPAWN_TEST ROOT_PROBE_*
 * block): the child cannot report its own fault, and the parent must not
 * be able to claim one happened. */
#define AOS_CHILD_SPAWN_MARKER_ROOT_FAULT_VERIFIED \
    "[rt] child-spawn: expected withheld-page fault verified\n"
