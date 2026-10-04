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
 * it with a Signal-only derivative of a Notification the parent itself
 * created from its pool, starts it, and waits for the child to signal
 * back -- proof the child is a genuinely separate, independently
 * scheduled thread that used exactly the authority it was given.
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
 * frames/TCB/SchedContext from, AND the notification object the parent
 * creates for itself before calling aos_child_spawn(). Nothing the parent
 * retypes at run time comes from anywhere else. */
#define AOS_CHILD_SPAWN_POOL_SLOT             46u

/* Size, in bits, of the untyped pool granted at AOS_CHILD_SPAWN_POOL_SLOT.
 * 1 MiB: generous headroom over the largest single spawn this demo
 * performs (one CNode, one VSpace, up to ~9 page-table objects across
 * three mappings, two leaf frames, one TCB, one SchedContext, plus the
 * parent's own notification and content frame -- each at most a few KB)
 * without pretending the pool is unbounded; Review Focus item 3
 * (exhaustion mid-creation) is about HANDLING running out, not about this
 * demo actually hitting the limit on a single spawn. */
#define AOS_CHILD_SPAWN_POOL_BITS             20u

/* ASID-pool slice for assigning the child's freshly retyped VSpace an
 * ASID -- not Untyped-derived, so not part of the pool above (same
 * reasoning as AOS_GUEST_ASID_POOL_CAP for the guest-RAM VMM). */
#define AOS_CHILD_SPAWN_ASID_POOL_SLOT        47u

/* SchedControl capability (this core) for configuring the child's
 * SchedContext -- MCS kernels only, not Untyped-derived. */
#define AOS_CHILD_SPAWN_SCHEDCONTROL_SLOT     48u

/* Self-reference to the parent's own VSpace -- used ONLY by
 * child_spawn_parent.c itself (not by aos_child_spawn(), which never
 * touches the parent's own address space) to temporarily map a freshly
 * retyped content frame into its OWN VSpace long enough to copy the
 * child's image bytes in, before handing that frame to aos_child_spawn()
 * to map into the CHILD's VSpace. Same self-reference pattern as
 * AOS_CAP_LEND_SELF_VSPACE_SLOT. */
#define AOS_CHILD_SPAWN_SELF_VSPACE_SLOT      50u

/* Slot, in the parent's own CNode, for the content frame the parent
 * retypes from its pool and populates before calling aos_child_spawn().
 * Outside the scratch range: this object's lifetime is the parent's
 * choice, not aos_child_spawn()'s -- the library only maps it, never
 * retypes or deletes it. */
#define AOS_CHILD_SPAWN_CONTENT_FRAME_SLOT    51u

/* Scratch VA, in the PARENT's OWN VSpace, used only transiently to write
 * the child's image bytes into the freshly retyped content frame before
 * it is unmapped from the parent and handed to the child. Chosen clear of
 * every fixed VA a normal service PD already uses (0x400000 own image,
 * 0x10000000 own IPC buffer, plus whatever pd_vspace.c placed its own
 * stack at). */
#define AOS_CHILD_SPAWN_PARENT_SCRATCH_VA  0x20000000UL

/* Radix, in bits, of child_spawn_parent's own CNode (pd_desc_t.
 * cnode_size_bits in system_desc_aarch64.c). */
#define AOS_CHILD_SPAWN_PARENT_CNODE_BITS      8u

/* First of AOS_CHILD_SPAWN_SCRATCH_COUNT (>= AOS_CHILD_SPAWN_SCRATCH_SLOTS
 * from child_spawn.h) contiguous FREE slots in the parent's own CNode
 * that aos_child_spawn() may stage new objects into. */
#define AOS_CHILD_SPAWN_SCRATCH_BASE          52u
#define AOS_CHILD_SPAWN_SCRATCH_COUNT         20u

/* First of 4 slots, just past aos_child_spawn()'s own scratch range, that
 * child_spawn_parent.c uses ONLY for itself: retyping the intermediate
 * page-table objects AOS_CHILD_SPAWN_PARENT_SCRATCH_VA needs in the
 * PARENT's OWN VSpace before it can map the content frame there to write
 * the child's image bytes in. Root's normal per-PD VSpace construction
 * (pd_vspace_create) only covers a PD's ELF image, stack, and IPC buffer
 * regions -- an address the PD picks for itself at run time, like this
 * scratch VA, has no page table until something retypes one, the same
 * reason aos_child_spawn() retries its own mappings on seL4_FailedLookup
 * (see child_spawn.c's map_frame_retrying). */
#define AOS_CHILD_SPAWN_PARENT_SCRATCH_PT_BASE 72u

/* Slot, in the PARENT's own CNode, where the parent retypes the
 * Notification object it endows a Signal-only derivative of into the
 * child. This is the ONLY capability this demo endows -- deliberately
 * small so Review Focus item 1 (endowment exceeding the parent) has
 * exactly one thing to check. */
#define AOS_CHILD_SPAWN_PARENT_NTFN_SLOT       49u

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
_Static_assert(AOS_CHILD_SPAWN_SCRATCH_BASE > AOS_CHILD_SPAWN_CONTENT_FRAME_SLOT,
               "aos_child_spawn()'s own scratch range must start strictly "
               "above every fixed boot-time grant slot, or its internal "
               "retypes will clobber a capability child_spawn_parent.c "
               "still needs");
_Static_assert(AOS_CHILD_SPAWN_PARENT_SCRATCH_PT_BASE >=
               AOS_CHILD_SPAWN_SCRATCH_BASE + AOS_CHILD_SPAWN_SCRATCH_COUNT,
               "child_spawn_parent.c's own page-table scratch range must "
               "not overlap aos_child_spawn()'s internal scratch range");


/* ── Child-side layout (chosen by the parent, not granted by root --
 * the child does not exist until the parent creates it) ───────────────── */

/* Radix, in bits, of the CHILD's own CNode. Small: it holds only the MCS
 * reply slot (9, reserved -- see system_desc.h) and its one endowed
 * capability. */
#define AOS_CHILD_SPAWN_CHILD_CNODE_BITS       6u

/* Slot in the CHILD's own CNode where the endowed Notification derivative
 * lands. */
#define AOS_CHILD_SPAWN_CHILD_NTFN_SLOT       10u

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

/* Badge the parent mints the child's one endowed capability with. */
#define AOS_CHILD_SPAWN_BADGE              0xC417u

/* aos_endow_cap_t.kind value for the one capability this demo endows.
 * endowment_contract.h leaves `kind` a caller-defined enumeration compared
 * only for equality; this demo has exactly one kind, so the specific
 * value is arbitrary. */
#define AOS_CHILD_SPAWN_KIND_NOTIFICATION      1u

/* Greppable boot-log markers (see cap_lend_test.h for why these exist: a
 * mint/map/configure/endow failure and success are otherwise
 * indistinguishable in the boot log, which would make any later proof
 * vacuous). child_spawn_parent emits these over the normal serial channel
 * every PD already has (SVC_ID_SERIAL init EP). */
#define AOS_CHILD_SPAWN_MARKER_OK \
    "[child-spawn-parent] OK: child spawned, endowed, and signalled back\n"
#define AOS_CHILD_SPAWN_MARKER_FAIL_SPAWN \
    "[child-spawn-parent] FAIL: aos_child_spawn\n"
