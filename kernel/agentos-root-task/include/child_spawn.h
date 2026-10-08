/*
 * child_spawn.h — the run-time domain-creation primitive
 *
 * Lets a protection domain ("the parent") create a CHILD domain while the
 * system is running, and endow it from the parent's own authority. This is
 * the mechanism that removes agentOS's build-time-fixed PD set assumption:
 *
 *     No domain ever holds authority its parent did not hold.
 *
 * ── Scope: a domain, not new code ───────────────────────────────────────
 *
 * This creates a seL4 domain (CNode + VSpace + TCB) at run time. It does
 * NOT load new code at run time: the child's program image is bytes that
 * were already part of the PARENT's own ELF, which T3 verified as a whole
 * before any of the parent's code -- including this call -- ever ran. No
 * path here fetches an image from anywhere else (no file path, no network,
 * no shared memory staged by a third party). The caller is responsible for
 * populating req->content_frame from bytes it already holds (see
 * tests/child-spawn/parent_pd.c), including publishing those writes past
 * its own D-cache before handing the frame over (aos_pt_map_retrying does
 * NOT do this for the caller -- see its own doc comment); this module only
 * maps what it is given. If a future caller needs to load an image from
 * somewhere else, that needs a new threat model, not a widened version of
 * this one.
 *
 * ── Mechanism: mirror root's own sequence, scoped to one pool ───────────
 *
 * Every other PD in this tree is built by the ROOT TASK at boot from
 * root's own global untyped pool (see pd_vspace.c, pd_tcb.c). This module
 * does the same three retypes (CNode, VSpace, TCB) -- see
 * platform/guest-ram/vmm_guest_paging.c:22 for the precedent of a non-root
 * PD retyping a VSpace object from its own granted pool at run time -- but
 * the source is a single seL4_Untyped capability the PARENT was granted at
 * boot (aos_child_spawn_req_t.pool_ut), never root's own pool and never
 * anything the parent was not separately, explicitly given. Two more
 * capabilities the parent needs beyond that pool are NOT Untyped-derived
 * and so cannot come from a retype: an ASID-pool slice (to bind the
 * child's new VSpace to a hardware address space) and, on an MCS kernel, a
 * SchedControl capability (to configure the child's scheduling context).
 * Both are boot-time grants alongside the pool, not reached for from
 * anywhere else.
 *
 * ── Endowment: a MINT, not a loan ────────────────────────────────────────
 *
 * Earlier revisions of this module minted endowments with T5's
 * aos_cap_lend() (libs/pd-support/cap_lend.c) and unwound a failed spawn
 * with aos_cap_lend_revoke(). That was wrong and has been removed:
 * aos_cap_lend_revoke() calls seL4_CNode_Revoke on the LENDER'S OWN
 * original capability, which deletes EVERY derivative of it, system-wide
 * -- correct for a T5 loan, which is temporary authority meant to be
 * withdrawn in full at the end of one operation, but catastrophic for
 * spawn teardown: if a parent has endowed the SAME original to two
 * different children (an ordinary hierarchical-delegation case) and the
 * second spawn fails partway through, revoking "this spawn's" mint would
 * silently strip the FIRST, already-running, correctly-endowed child of
 * authority it still legitimately holds. T5's loan lifetime and T6's
 * endowment lifetime are different operations and must not share a
 * teardown path.
 *
 * aos_child_spawn() instead mints each capability in the validated
 * endowment directly with seL4_CNode_Mint, from the capability the
 * caller-supplied endow_cptrs[i] names (in the parent's own CSpace) into
 * the child's CNode. This is the SAME kernel operation T5 uses underneath
 * -- seL4_CNode_Mint is how every rights-reduced derivative in this tree
 * is made -- just without T5's lease bookkeeping or its loan-lifetime
 * revoke semantics layered on top. Task 1's aos_endowment_validate()
 * still gates every mint (see "Validation" below); seL4_CNode_Mint itself
 * additionally masks the requested rights against the source capability's
 * actual rights, so the kernel can never produce a derivative that
 * exceeds the source regardless of what either check asserts.
 *
 * T5's aos_cap_lend() is untouched and remains exactly what it was: the
 * right primitive for a parent (or any PD) that wants to lend TEMPORARY
 * authority to an ALREADY-RUNNING domain and withdraw it later. That is a
 * distinct operation from endowing a child at creation time and does not
 * belong on this path -- a caller that wants both calls aos_cap_lend()
 * separately, after aos_child_spawn() returns.
 *
 * ── The one hard rule: no partially-endowed child ever runs ─────────────
 *
 * aos_child_spawn() fully configures the child's TCB (CSpace, VSpace, IPC
 * buffer, priority, and -- on MCS -- its scheduling context) WITHOUT
 * starting it: seL4_TCB_WriteRegisters is called with resume=0, which
 * writes PC/SP/args but leaves the thread Inactive. Endowment happens
 * strictly after that and strictly before the one and only resume=1 call
 * that makes the thread runnable. If any endowment mint fails, this
 * module does NOT attempt to unmint the ones that already succeeded --
 * there is no safe, targeted "unmint" operation (see above) -- it simply
 * deletes the child's CNode capability along with every other object this
 * call retyped (see staging_teardown() in child_spawn.c). Deleting the
 * ONLY capability to the child's CNode destroys that CNode object and
 * every capability minted into its slots along with it; the PARENT's own
 * original capabilities are never touched. The TCB is never resumed, so
 * the function returns an error and nothing new is left running. A child
 * that starts without its full endowment is a live domain whose authority
 * nobody described; this module would rather fail the whole spawn than
 * let that exist.
 *
 * ── Untyped exhaustion: report the failed step, don't hide it ───────────
 *
 * seL4_Untyped_Retype is a bump allocator: it has no "free one object back
 * to the pool" operation, only "revoke everything ever retyped from this
 * Untyped." If retype N+1 fails after retypes 1..N already succeeded, this
 * module deletes the CAPABILITIES to objects 1..N (so nothing dangling is
 * left reachable) but is honest that the underlying bytes those objects
 * consumed inside pool_ut are NOT returned to the pool -- that is a seL4
 * property, not a bug in this code. aos_child_spawn_result_t.failed_step
 * tells the caller exactly which retype/map/configure/endow step failed,
 * so a caller sees shrinking headroom rather than retrying blindly into
 * total exhaustion. On AOS_CHILD_SPAWN_ERR_START (see below),
 * .scratch_base/.scratch_next report the full range of slots the caller
 * would need to delete to reclaim everything by hand, since that is the
 * one path this module does not unwind automatically.
 *
 * ── Reporting: the endowment-delta ledger ───────────────────────────────
 *
 * Because endowment is a direct seL4_CNode_Mint (see above) rather than a
 * call through aos_cap_lend(), it does not pick up T5's lease-table side
 * effect -- and it should not: the lease table records LOANS, authority
 * whose whole point is that it will be withdrawn again, and an endowment
 * has no such lifetime. Instead, aos_child_spawn() appends one entry to a
 * caller-supplied endowment-delta ledger (platform/endow_ledger.h) per
 * SUCCESSFUL mint, and rolls the whole spawn's worth of entries back if
 * the spawn fails for any reason -- including the one failure path that
 * leaves objects in place (AOS_CHILD_SPAWN_ERR_START), because that child
 * never ran either. The ledger therefore only ever describes children
 * that actually started.
 *
 * That ledger folds into T4's authority snapshot
 * (aos_endow_ledger_merge), which is exactly the channel
 * platform/authority.h already names for this: "it covers the boot-time
 * static set; runtime delegation must be separately reported by the
 * delegating domain." This is that separate report, not a second
 * reporting system.
 *
 * It is a REPORT, NOT A PROOF. seL4 exposes no capability-enumeration
 * syscall, so the ledger records what this module was ASKED to mint and
 * succeeded in minting -- it cannot read kernel state back. The
 * subsetting invariant holds regardless of what the ledger says, because
 * seL4_CNode_Mint masks requested rights against the source capability's
 * actual rights and a domain cannot mint from a capability it does not
 * hold. req->ledger may be NULL, in which case nothing is recorded and
 * the mints are unaffected.
 *
 * ── Not a broker ─────────────────────────────────────────────────────────
 *
 * aos_child_spawn() is a library function a PD links and calls against ITS
 * OWN pool, ITS OWN CNode, ITS OWN holdings. There is no service endpoint
 * here that mints on behalf of an arbitrary caller. A component that did
 * that would be ambient authority wearing a different hat; this module is
 * deliberately not that.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once

#include <sel4/sel4.h>
#include <stdint.h>

#include <platform/endow_ledger.h>

#include "contracts/endowment_contract.h"

/* ── Return codes ─────────────────────────────────────────────────────── */

#define AOS_CHILD_SPAWN_OK                  0
/* req->endow failed aos_endowment_validate() against req->parent_holdings
 * -- a declaration problem, checked before any seL4 object is touched.
 * NOTE: req->parent_holdings is a caller-asserted description of what the
 * parent holds, not independently measured -- see aos_child_spawn()'s own
 * doc comment and endowment_contract.h. The kernel is the actual
 * enforcement point (seL4_CNode_Mint masks rights against the real source
 * capability); this check only catches a malformed or dishonest request
 * before any seL4 object is built. */
#define AOS_CHILD_SPAWN_ERR_VALIDATE       (-1)
/* req did not supply enough contiguous free slots in the parent's own
 * CNode (req->scratch_count < AOS_CHILD_SPAWN_SCRATCH_SLOTS) to stage
 * every object this call may need to retype. Nothing was touched. */
#define AOS_CHILD_SPAWN_ERR_SCRATCH        (-2)
/* A seL4_Untyped_Retype call failed. See result->failed_step for which
 * object. Every object retyped before this one in the same call has
 * already been deleted (capability only -- see the file header on
 * Untyped's bump-allocator semantics); the TCB was never resumed. */
#define AOS_CHILD_SPAWN_ERR_RETYPE         (-3)
/* seL4_ARM_ASIDPool_Assign failed on the freshly retyped VSpace. Torn
 * down like AOS_CHILD_SPAWN_ERR_RETYPE. */
#define AOS_CHILD_SPAWN_ERR_ASID           (-4)
/* Mapping the content, stack, or IPC buffer frame into the child's
 * VSpace failed even after retyping the bounded number of intermediate
 * page-table objects this call allows. Torn down like ERR_RETYPE. If the
 * content frame had already been successfully mapped before this failure
 * (i.e. the failure is on the stack or IPC buffer mapping), it is
 * unmapped again as part of teardown so a caller can retry the spawn with
 * the SAME content_frame capability: seL4_ARM_Page_Map records the mapping
 * ON THE FRAME CAPABILITY, so a frame left "mapped" into a destroyed
 * VSpace fails its next map for a reason unrelated to whatever went wrong
 * here. tests/child-spawn/parent_pd.c relies on this -- its doomed spawn
 * and its real spawn use the same content_frame -- which makes
 * make test-child-spawn a regression test for it. */
#define AOS_CHILD_SPAWN_ERR_MAP            (-5)
/* seL4_TCB_Configure, seL4_TCB_WriteRegisters(resume=0), or (MCS only)
 * SchedContext retype/configure/SetSchedParams failed. Torn down (content
 * frame unmapped, same as ERR_MAP); the TCB -- if it was created at all --
 * was never resumed. */
#define AOS_CHILD_SPAWN_ERR_CONFIGURE      (-6)
/* At least one seL4_CNode_Mint in the endowment loop failed -- which,
 * given Task 1's validation already ran, means the parent's holdings
 * declaration did not match reality (see ERR_VALIDATE's note) or the
 * source capability was otherwise unusable. The child's CNode (and every
 * other object this call retyped) is deleted: deleting the only
 * capability to the child's CNode destroys it and every mint already
 * placed in its slots along with it. The PARENT's own original
 * capabilities are never touched -- there is no revoke of anything the
 * parent holds. result->failed_endow_index names which endowment entry
 * failed to mint. */
#define AOS_CHILD_SPAWN_ERR_ENDOW          (-7)
/* The final seL4_TCB_WriteRegisters(resume=1) call failed. This is the
 * one failure mode where objects were NOT torn down automatically: every
 * endowment had already succeeded and reversing them here would mean
 * destroying authority that was correctly granted because of an unrelated
 * failure in the final kernel call. The child is still Inactive (never
 * ran), so no authority was exercised, but the caller owns the decision
 * of whether to retry the resume or tear the child down itself.
 *
 * MANUAL TEARDOWN RECIPE. A caller that chooses to tear the child down
 * rather than retry the resume must do BOTH of the following, in this
 * order:
 *
 *   1. Unmap every CALLER-OWNED frame this call mapped into the child's
 *      VSpace -- req->content_frame, and req->extra_maps[i].frame for
 *      every i < req->extra_map_count. These are the caller's own
 *      capabilities; this module never retyped them, so step 2 does not
 *      and cannot reach them. seL4_ARM_Page_Map records the mapping ON
 *      THE FRAME CAPABILITY, so skipping this leaves the caller's own
 *      frame caps believing they are mapped into a VSpace that step 2
 *      destroyed -- and a later retry with the same frames then fails at
 *      the mapping step for a reason unrelated to whatever went wrong
 *      here. (Every OTHER failure path in this module performs these
 *      unmaps itself; this is the only one that cannot, because it does
 *      not know whether the caller means to retry the resume.)
 *   2. seL4_CNode_Delete every slot in
 *      [result->scratch_base, result->scratch_next) -- the complete set
 *      of objects this call created, not just the three named in
 *      .cnode/.vspace/.tcb.
 *
 * The endowment-delta ledger has already been rolled back to its
 * pre-call state when this is returned: the child never ran, so nothing
 * it was granted is reported. */
#define AOS_CHILD_SPAWN_ERR_START          (-8)

/* ── Sizing ───────────────────────────────────────────────────────────── */

/*
 * Upper bound on how many fresh capability slots in the PARENT's own
 * CNode a single aos_child_spawn() call may use while staging: CNode (1),
 * VSpace (1), up to 3 intermediate page-table levels PER mapped region
 * (content, stack, IPC buffer, and one extra map -- 4 regions x 3 levels
 * = 12), the stack and IPC leaf frames (2 -- the content frame and every
 * extra-map frame are supplied already retyped by the caller, see
 * req->content_frame / req->extra_maps), TCB (1), and (MCS only) one
 * SchedContext (1). 1+1+12+2+1+1 = 18; rounded up for headroom without
 * pretending to be unbounded.
 *
 * A caller passing more than ONE extra map must size req->scratch_count
 * above this bound itself: three further page-table levels per additional
 * region is the worst case, and this constant does not try to express a
 * number that depends on a run-time count. aos_child_spawn() returns
 * AOS_CHILD_SPAWN_ERR_RETYPE (not silent corruption) if the scratch range
 * runs out mid-way, because pt_scratch_claim() refuses past its limit.
 */
#define AOS_CHILD_SPAWN_SCRATCH_SLOTS       24u

/* Maximum intermediate page-table levels aos_pt_map_retrying() (below)
 * will retype for a single mapped VA before giving up -- AArch64 has at
 * most three below the VSpace root, same bound
 * aos_vmm_guest_paging_rebuild()'s retry loop uses
 * (platform/guest-ram/vmm_guest_paging.c). */
#define AOS_CHILD_SPAWN_MAX_PT_LEVELS        4u

/* ── Shared bounded-retry page-table scratch allocator ───────────────────
 *
 * aos_child_spawn() needs to map several frames (content, stack, IPC
 * buffer) into a VSpace that starts out with no page tables at all,
 * retyping intermediate page-table objects from a pool on
 * seL4_FailedLookup exactly as aos_vmm_guest_paging_rebuild() does
 * (platform/guest-ram/vmm_guest_paging.c). A CALLER of aos_child_spawn()
 * needs the identical discipline for a different reason: populating
 * req->content_frame requires mapping it into the CALLER's OWN VSpace
 * first (to write the child's image bytes through it), at a scratch VA
 * that root's normal PD construction never created page tables for
 * either (see tests/child-spawn/parent_pd.c). Rather than let that caller
 * reimplement the same ~20-line retry loop a second time (which is what
 * the first version of this file's test wiring did), both uses share
 * this type and aos_pt_map_retrying().
 */
typedef struct {
    seL4_CPtr   pool_ut;         /* source Untyped for any page table this
                                   * allocator retypes. */
    seL4_CPtr   self_cnode;      /* owner's own CNode -- destination root
                                   * for every retype and the CNode
                                   * aos_pt_map_retrying()'s page tables
                                   * land in. */
    seL4_Word   self_cnode_bits; /* radix of self_cnode. */
    seL4_Word   base_slot;       /* first slot this allocator was given --
                                   * recorded, not consumed by this type
                                   * itself, purely so an owner that wants
                                   * to delete everything it ever claimed
                                   * (e.g. aos_child_spawn()'s own
                                   * teardown) has it without separate
                                   * bookkeeping. */
    seL4_Word   next_slot;       /* next free slot. */
    seL4_Word   limit_slot;      /* one past the last usable slot
                                   * (base_slot + the count the owner
                                   * reserved). */
} aos_pt_scratch_t;

/* Initialise sc to claim slots from [base_slot, base_slot+count). */
void aos_pt_scratch_init(aos_pt_scratch_t *sc, seL4_CPtr pool_ut,
                          seL4_CPtr self_cnode, seL4_Word self_cnode_bits,
                          seL4_Word base_slot, seL4_Word count);

/*
 * aos_pt_map_retrying — map `frame` into `vspace` at `va`, retyping
 * intermediate page-table objects from sc->pool_ut into sc's own scratch
 * range on seL4_FailedLookup, up to AOS_CHILD_SPAWN_MAX_PT_LEVELS
 * attempts. Returns seL4_NoError on success.
 *
 * Does NOT perform any cache maintenance on `frame` -- if the caller
 * wrote to the frame through a mapping established by (or before) this
 * call and intends to hand it to another VSpace afterward (as
 * tests/child-spawn/parent_pd.c does with its content frame), AArch64
 * requires the caller to clean/invalidate the frame's D-cache lines
 * (seL4_ARM_Page_CleanInvalidate_Data) and issue a memory fence BEFORE
 * unmapping it and handing it elsewhere -- see
 * kernel/agentos-root-task/src/pd_vspace.c's sync_scratch_frame() for the
 * existing in-tree precedent this module's callers should follow; this
 * function does not do it on a caller's behalf because it has no way to
 * know whether the caller has finished writing yet.
 */
seL4_Error aos_pt_map_retrying(aos_pt_scratch_t *sc, seL4_CPtr frame,
                                seL4_CPtr vspace, seL4_Word va);

/* ── Extra mappings ───────────────────────────────────────────────────── */

/*
 * One additional caller-owned frame to map into the CHILD's VSpace before
 * the child is started.
 *
 * req->content_frame covers the child's program image; this covers memory
 * the parent means to ENDOW. It has to happen inside this call, not after
 * it: the hard rule is that no partially endowed child ever runs, and a
 * mapping installed after aos_child_spawn() returns would be authority
 * arriving at a domain that is already executing. The frames are
 * caller-owned on exactly the same terms as content_frame -- this module
 * maps them, never retypes, writes, flushes or deletes them, and unmaps
 * them again on every failure path it unwinds (see
 * AOS_CHILD_SPAWN_ERR_START for the one it does not).
 *
 * A caller that also wants the child to hold a CAPABILITY to such a frame,
 * rather than only a window onto it, lists it in req->endow as well: the
 * mapping and the mint are independent grants and this struct is only the
 * former.
 */
typedef struct {
    seL4_CPtr   frame;  /* caller-owned frame capability, already populated
                          * and already cache-cleaned by the caller if it
                          * wrote through its own mapping first (see
                          * aos_pt_map_retrying()'s doc comment). */
    seL4_Word   va;     /* VA in the child's VSpace to map it at. */
} aos_child_spawn_map_t;

/* ── Request ──────────────────────────────────────────────────────────── */

/*
 * aos_child_spawn_req_t — everything one aos_child_spawn() call needs.
 *
 * Capabilities marked "self-ref" are copies of the PARENT's own CNode /
 * VSpace / TCB that root granted the parent at boot (same pattern as
 * AOS_GUEST_RAM_SELF_CNODE / AOS_CAP_LEND_SELF_CNODE_SLOT elsewhere in
 * this tree) so the parent can name itself as a root argument to
 * seL4_Untyped_Retype / seL4_CNode_Mint / seL4_TCB_Configure.
 */
typedef struct {
    seL4_CPtr   pool_ut;            /* parent's granted untyped pool; the
                                      * ONLY source of every retyped object
                                      * below -- see Review Focus item 1. */
    seL4_CPtr   self_cnode;         /* self-ref to the parent's own CNode */
    uint8_t     self_cnode_bits;    /* radix of the PARENT's own CNode --
                                      * the depth argument for every
                                      * CNode_Delete/Mint this call issues
                                      * against self_cnode. */
    seL4_CPtr   self_tcb;           /* self-ref to the parent's own TCB;
                                      * this PD's own mcp was set to 255 by
                                      * root at boot (same as every PD --
                                      * see pd_tcb.c), so it can grant the
                                      * child any priority 0..255, exactly
                                      * as root grants every PD using its
                                      * own TCB. */
    seL4_CPtr   asid_pool;          /* ASID-pool slice for the child's new
                                      * VSpace; not Untyped-derived. */
#ifdef CONFIG_KERNEL_MCS
    seL4_CPtr   sched_control;      /* SchedControl cap (this core); not
                                      * Untyped-derived, MCS kernels only. */
#endif
    seL4_Word   scratch_slot;       /* first of >= AOS_CHILD_SPAWN_SCRATCH_SLOTS
                                      * contiguous FREE slots in self_cnode
                                      * this call may stage new objects into. */
    seL4_Word   scratch_count;      /* how many slots are free starting at
                                      * scratch_slot; checked up front. */

    seL4_CPtr   content_frame;      /* a frame cap, already in the parent's
                                      * own CNode, already populated by the
                                      * caller with bytes from the parent's
                                      * OWN verified ELF (see child_spawn.h
                                      * file header) -- NOT retyped or
                                      * written by this call, only mapped.
                                      * This call never reads or copies
                                      * bytes itself: only the caller, which
                                      * knows what scratch VA range is safe
                                      * in its own already-running VSpace,
                                      * can safely populate a fresh frame.
                                      * The caller is responsible for its
                                      * own cache maintenance before handing
                                      * this frame over -- see
                                      * aos_pt_map_retrying()'s doc comment. */
    seL4_Word   content_va;         /* VA to map content_frame at in the
                                      * child's VSpace (its entry point
                                      * lives inside this page). */

    /* Additional caller-owned frames to map into the child's VSpace
     * before it starts -- see aos_child_spawn_map_t. May be NULL with
     * extra_map_count == 0. */
    const aos_child_spawn_map_t *extra_maps;
    seL4_Word   extra_map_count;

    /* Fault handler endpoint installed on the child's TCB
     * (seL4_TCB_SetSchedParams' fault_ep argument). seL4_CapNull leaves
     * the child with no fault handler, in which case a faulting child is
     * simply suspended by the kernel and nobody is told. A caller that
     * wants a child's faults to be OBSERVABLE -- by the root task, or by
     * itself -- passes a (typically badged) endpoint capability here. The
     * child cannot see or name this capability: it is written into the
     * TCB, not into the child's CSpace.
     *
     * NOTE what this does NOT buy, and see fault_install_ep below. seL4's
     * MCS validFaultHandler() requires a fault-handler capability to
     * carry Send plus Grant or GrantReply -- a rights-stripped copy is
     * refused with seL4_InvalidCapability at the SetSchedParams above
     * (measured, not inferred). So any caller that passes a capability
     * here necessarily HOLDS send rights on the endpoint the handler
     * reports to, and can therefore compose messages to whoever is
     * listening on it. That is fine when the listener is the caller
     * itself; it is not fine when the listener is an independent
     * observer whose report is meant to be evidence about the caller. */
    seL4_CPtr   fault_ep;

    /* Alternative to fault_ep for the "independent observer" case: a
     * Send+Grant endpoint capability on a service that will install ITS
     * OWN fault-handler capability on the child's TCB on the caller's
     * behalf. When this is not seL4_CapNull, fault_ep is ignored, and
     * this call instead:
     *
     *   1. configures the child's TCB and writes its initial registers
     *      (resume = 0) exactly as before;
     *   2. seL4_Call()s fault_install_ep with the child's TCB capability
     *      as the single transferred capability and
     *      fault_install_label as the message label, BEFORE the child's
     *      SchedContext exists. The installer answers with label 0 on
     *      success. It must perform the TCB_SetSchedParams itself (with
     *      a null SchedContext capability -- the child has none yet, so
     *      nothing is unbound) which is what sets the child's priority
     *      and mcp as well;
     *   3. binds the SchedContext with seL4_SchedContext_Bind rather
     *      than a second SetSchedParams, because SetSchedParams ALWAYS
     *      rewrites the fault handler (thread_control_sched_update_fault
     *      is unconditional in the kernel) and would immediately undo
     *      step 2.
     *
     * The point is that the caller never holds a capability to the
     * endpoint the child's faults are reported on. It still chooses
     * which TCB it presents to the installer -- see the installer's own
     * documentation for what that leaves open. */
    seL4_CPtr   fault_install_ep;
    seL4_Word   fault_install_label; /* message label for the request
                                       * above; ignored when
                                       * fault_install_ep is
                                       * seL4_CapNull. */
    seL4_Word   entry_point;        /* child's initial PC */
    seL4_Word   stack_va_top;       /* child's initial SP (top of a single
                                      * freshly retyped, zeroed stack page) */
    seL4_Word   ipc_buf_va;         /* VA for the child's IPC buffer page */
    seL4_Word   arg0;               /* child's initial ARG0 */
    seL4_Word   arg1;               /* child's initial ARG1 */

    uint8_t     cnode_size_bits;    /* radix of the child's own CNode */
    uint8_t     priority;           /* child's scheduling priority */

    const aos_endowment_t *endow;            /* validated declaration */
    const aos_endowment_t *parent_holdings;  /* the parent's claimed
                                               * holdings, checked against
                                               * `endow` before anything is
                                               * minted. This is a
                                               * CALLER-ASSERTED structure,
                                               * not independently
                                               * measured -- Task 1's check
                                               * (aos_endowment_validate)
                                               * only verifies internal
                                               * consistency between `endow`
                                               * and whatever this struct
                                               * claims, so a caller that
                                               * lies about its holdings
                                               * passes this check exactly
                                               * as a caller that tells the
                                               * truth would. The REAL
                                               * enforcement of "a mint can
                                               * never exceed its source" is
                                               * seL4_CNode_Mint's own
                                               * rights-masking against the
                                               * actual source capability,
                                               * at mint time -- this check
                                               * exists to refuse a
                                               * malformed or dishonest
                                               * request before any seL4
                                               * object is built, not to
                                               * replace the kernel's
                                               * guarantee. */
    /*
     * endow_cptrs[i] is the REAL seL4_CPtr, in the parent's own CSpace, of
     * the capability endow->caps[i] describes (parent_slot is deliberately
     * opaque at the contract layer -- see endowment_contract.h -- and
     * resolving it to a real capability is explicitly this layer's job).
     * Must have endow->count valid entries.
     */
    const seL4_CPtr *endow_cptrs;
    seL4_Word   endow_child_base_slot; /* first slot in the child's CNode
                                         * where endowed derivatives land;
                                         * entry i lands at base_slot + i. */
    seL4_Word   endow_badge;           /* badge used for every mint in this
                                         * endowment (single badge: a
                                         * caller endowing several kinds
                                         * that need distinguishable badges
                                         * would need a per-entry badge
                                         * array, not added here because
                                         * nothing in this tree needs it
                                         * yet). NOTE: seL4 refuses to
                                         * re-badge a derivative, so every
                                         * capability named by endow_cptrs
                                         * must be an original (a freshly
                                         * retyped object, or a frame) --
                                         * minting an already-badged
                                         * endpoint with a different badge
                                         * fails, and is reported as
                                         * AOS_CHILD_SPAWN_ERR_ENDOW. */

    /*
     * Endowment-delta ledger this call appends to, once per SUCCESSFUL
     * mint, and rolls back in full if the spawn fails -- see
     * platform/endow_ledger.h and the file header's "Reporting" section.
     * May be NULL: nothing is recorded and the mints are unaffected.
     *
     * aos_endow_cap_t.kind is recorded verbatim, so a caller that wants
     * its deltas to merge meaningfully into a T4 authority snapshot
     * numbers its kinds as aos_authority_kind_t values.
     */
    aos_endow_ledger_t *ledger;
    uint32_t    child_index;        /* the caller's own identifier for this
                                      * child, used as the ledger/authority
                                      * row key. */
    const char *child_name;         /* the caller's name for this child, as
                                      * it should appear in the report. */
} aos_child_spawn_req_t;

/* ── Result ───────────────────────────────────────────────────────────── */

typedef struct {
    seL4_CPtr   cnode;      /* child's CNode capability (in the parent's
                              * own CSpace, at the scratch slot it landed
                              * on) -- seL4_CapNull if spawn failed. */
    seL4_CPtr   vspace;     /* child's VSpace capability, same scope. */
    seL4_CPtr   tcb;        /* child's TCB capability, same scope. */
    int         error;      /* AOS_CHILD_SPAWN_* */
    int         failed_step;/* opaque step id (see child_spawn.c) naming
                              * exactly which retype/map/configure/endow
                              * call failed, for diagnosis -- not a stable
                              * external ABI, just a debugging breadcrumb. */
    seL4_Word   failed_endow_index; /* valid when error ==
                              * AOS_CHILD_SPAWN_ERR_ENDOW: which entry in
                              * req->endow->caps[] failed to mint. */
    seL4_Word   scratch_base;  /* req->scratch_slot: first slot this call
                              * may have staged objects into.
                              *
                              * Populated on every return that reached the
                              * staging stage. The three up-front rejections
                              * -- AOS_CHILD_SPAWN_ERR_VALIDATE (including
                              * req == NULL) and AOS_CHILD_SPAWN_ERR_SCRATCH
                              * -- return with this and .scratch_next both
                              * ZERO, because no slot range was claimed and
                              * reporting req->scratch_slot would imply one
                              * had been. On those paths there is nothing to
                              * tear down: no seL4 object was touched. The
                              * two fields are equal on every failure path
                              * except AOS_CHILD_SPAWN_ERR_START, so
                              * "scratch_next == scratch_base" is a valid
                              * "nothing is left staged" test on all of
                              * them. */
    seL4_Word   scratch_next;  /* one past the LAST slot this call actually
                              * used. On AOS_CHILD_SPAWN_ERR_START (the one
                              * path that does not auto-teardown), a caller
                              * that wants to tear the child down itself
                              * should delete every slot in
                              * [scratch_base, scratch_next) -- that is the
                              * complete set of objects this call created,
                              * not just the three named in .cnode/.vspace/
                              * .tcb -- and it must be preceded by unmapping
                              * the caller-owned frames listed in that error
                              * code's manual-teardown recipe above
                              * (content_frame and every extra_maps[i]),
                              * which this range does NOT cover because this
                              * call never retyped them. On every other
                              * failure path this equals scratch_base
                              * (everything was already torn down, and the
                              * caller-owned frames were already unmapped
                              * for the caller). On success it reports
                              * exactly
                              * what the child's live object set occupies,
                              * for a caller that wants to know. */
} aos_child_spawn_result_t;

/*
 * aos_child_spawn — create and start a child domain from the parent's own
 * authority.
 *
 * On success, the child is fully configured, fully endowed, and running
 * (its TCB has been resumed) -- *out is populated and
 * AOS_CHILD_SPAWN_OK is returned.
 *
 * On any failure other than AOS_CHILD_SPAWN_ERR_START, the child's TCB --
 * if one was ever created -- was never resumed: no code the child's own
 * instruction pointer could reach has executed. Every capability this
 * call retyped from req->pool_ut is deleted (not reclaimed -- see the
 * file header), which includes the child's CNode and therefore every
 * endowment mint already placed in it -- the PARENT's own original
 * capabilities are never touched, revoked, or otherwise affected by this
 * teardown. *out is zeroed except .error / .failed_step /
 * .failed_endow_index / .scratch_base / .scratch_next.
 */
int aos_child_spawn(const aos_child_spawn_req_t *req,
                     aos_child_spawn_result_t *out);
