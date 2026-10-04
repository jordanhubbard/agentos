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
 * ── Reporting: there is currently none beyond the kernel itself ─────────
 *
 * aos_child_spawn() does not write any ledger. Because endowment is now a
 * direct seL4_CNode_Mint (see above) rather than a call through
 * aos_cap_lend(), it does not pick up T5's lease-table side effect
 * either; nothing records a runtime-spawned child's endowment anywhere a
 * human or T4's authority page could read it. seL4 itself enforces the
 * subsetting invariant regardless (a mint can never exceed its source's
 * actual rights), but there is currently no "report, not proof" layer at
 * all for this path -- that is a real, open gap, not a deferred one; see
 * docs/superpowers/plans/2026-10-04-t6-hierarchical-delegation.md. A
 * caller that wants its endowments self-reported can still call
 * aos_cap_lend() itself, separately, understanding that doing so creates
 * a T5-lifetime loan (revocable in full by the parent later) layered on
 * top of the permanent mint this module already made -- two different
 * capabilities with two different lifetimes, not a substitute for one
 * another.
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
 * the SAME content_frame capability -- see I2 in the Task 2 review this
 * module was revised against. */
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
 * of whether to retry the resume or tear the child down itself --
 * result->scratch_base/.scratch_next report the full slot range to delete
 * for that purpose (every object this call created, not just the three
 * named in the handle). */
#define AOS_CHILD_SPAWN_ERR_START          (-8)

/* ── Sizing ───────────────────────────────────────────────────────────── */

/*
 * Upper bound on how many fresh capability slots in the PARENT's own
 * CNode a single aos_child_spawn() call may use while staging: CNode (1),
 * VSpace (1), up to 3 intermediate page-table levels PER mapped region
 * (content, stack, IPC buffer -- 3 regions x 3 levels = 9), the stack and
 * IPC leaf frames (2 -- the content frame itself is supplied already
 * retyped by the caller, see req->content_frame), TCB (1), and (MCS only)
 * one SchedContext (1). 1+1+9+2+1+1 = 15; rounded up for headroom without
 * pretending to be unbounded.
 */
#define AOS_CHILD_SPAWN_SCRATCH_SLOTS       20u

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
                                         * endowment (single badge: this
                                         * demo endows one capability; a
                                         * caller endowing several distinct
                                         * kinds that need distinguishable
                                         * badges would need a per-entry
                                         * badge array, not added here
                                         * because nothing in this tree
                                         * needs it yet). */
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
                              * may have staged objects into. Populated on
                              * every return, success or failure. */
    seL4_Word   scratch_next;  /* one past the LAST slot this call actually
                              * used. On AOS_CHILD_SPAWN_ERR_START (the one
                              * path that does not auto-teardown), a caller
                              * that wants to tear the child down itself
                              * should delete every slot in
                              * [scratch_base, scratch_next) -- that is the
                              * complete set of objects this call created,
                              * not just the three named in .cnode/.vspace/
                              * .tcb. On every other failure path this
                              * equals scratch_base (everything was already
                              * torn down). On success it reports exactly
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
