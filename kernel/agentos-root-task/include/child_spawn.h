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
 * tests/child-spawn/parent_pd.c); this module only maps what it is given.
 * If a future caller needs to load an image from somewhere else, that
 * needs a new threat model, not a widened version of this one.
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
 * ── Endowment: T5's mint path, a different lifetime ─────────────────────
 *
 * Each capability in the validated endowment is minted into the child's
 * CNode with aos_cap_lend() (libs/pd-support/cap_lend.c) -- the SAME
 * seL4_CNode_Mint call T5 uses to lend a rights-reduced derivative to a
 * borrower, not a second minting implementation. The difference is
 * lifetime: a T5 loan is withdrawn by the lender at the end of one
 * operation (aos_cap_lend_revoke); a child's endowment is meant to last
 * the child's life and this module never revokes it on success. The ONE
 * place this module calls aos_cap_lend_revoke is teardown on a *failed*
 * spawn (see below) -- there it is reused exactly as T5 intends: undo one
 * specific mint and close its lease record, nothing more.
 *
 * ── The one hard rule: no partially-endowed child ever runs ─────────────
 *
 * aos_child_spawn() fully configures the child's TCB (CSpace, VSpace, IPC
 * buffer, priority, and -- on MCS -- its scheduling context) WITHOUT
 * starting it: seL4_TCB_WriteRegisters is called with resume=0, which
 * writes PC/SP/args but leaves the thread Inactive. Endowment happens
 * strictly after that and strictly before the one and only resume=1 call
 * that makes the thread runnable. If any endowment mint fails, every
 * already-minted endowment on this child is revoked (aos_cap_lend_revoke,
 * which also closes its lease record), every object this call retyped is
 * deleted, and the TCB is never resumed -- the function returns an error
 * and nothing new is left running. A child that starts without its full
 * endowment is a live domain whose authority nobody described; this
 * module would rather fail the whole spawn than let that exist.
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
 * total exhaustion.
 *
 * ── Reporting: this is a claim, not a measurement ────────────────────────
 *
 * Every successful endowment mint is recorded in the PARENT's own
 * aos_cap_lend lease table exactly as a T5 loan would be (see
 * aos_cap_lend_lookup) -- that table is this module's ledger entry for the
 * child. A runtime-created child has no boot-time descriptor-table index,
 * so it cannot appear in the root task's own cap_accounting table (T4's
 * authority page, which is populated only at root's own boot time) without
 * a new cross-PD reporting channel that does not exist yet; that gap is
 * real and is called out again in child_spawn.c and in
 * docs/superpowers/plans/2026-10-04-t6-hierarchical-delegation.md. What
 * this module DOES give a ledger reader is exactly what T5 already gives
 * it: a self-reported record of what the parent believes it granted. seL4
 * exposes no capability-enumeration syscall, so no report built from this
 * table -- or from any future table it feeds -- is ever proof of what the
 * child actually holds, only a claim about what this parent, honestly or
 * not, says it minted.
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
 * -- a declaration problem, checked before any seL4 object is touched. */
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
 * page-table objects this call allows. Torn down like ERR_RETYPE. */
#define AOS_CHILD_SPAWN_ERR_MAP            (-5)
/* seL4_TCB_Configure, seL4_TCB_WriteRegisters(resume=0), or (MCS only)
 * SchedContext retype/configure/SetSchedParams failed. Torn down; the
 * TCB -- if it was created at all -- was never resumed. */
#define AOS_CHILD_SPAWN_ERR_CONFIGURE      (-6)
/* At least one aos_cap_lend() mint in the endowment loop failed.
 * Every endowment that DID succeed before this one was revoked
 * (aos_cap_lend_revoke) before returning, every retyped object was
 * deleted, and the TCB was never resumed: nothing about this child is
 * left running or holding authority. result->failed_endow_index names
 * which endowment entry failed. */
#define AOS_CHILD_SPAWN_ERR_ENDOW          (-7)
/* The final seL4_TCB_WriteRegisters(resume=1) call failed. This is the
 * one failure mode where objects were NOT torn down automatically: every
 * endowment had already succeeded and reversing them here would mean
 * revoking authority that was correctly granted because of an unrelated
 * failure in the final kernel call. The child is still Inactive (never
 * ran), so no authority was exercised, but the caller owns the decision
 * of whether to retry the resume or tear the child down itself via the
 * returned handle. */
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

/* Maximum intermediate page-table levels this call will retype for a
 * single mapped VA before giving up -- AArch64 has at most three below
 * the VSpace root, same bound aos_vmm_guest_paging_rebuild()'s retry loop
 * uses (platform/guest-ram/vmm_guest_paging.c). */
#define AOS_CHILD_SPAWN_MAX_PT_LEVELS        4u

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
                                      * can safely populate a fresh frame. */
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
    const aos_endowment_t *parent_holdings;  /* parent's actual holdings,
                                               * checked against `endow`
                                               * before anything is minted --
                                               * Task 1's check is on a
                                               * declaration; this is where
                                               * it binds to reality. */
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
 * file header); every endowment already minted into the child's CNode is
 * revoked via aos_cap_lend_revoke. *out is zeroed except
 * .error / .failed_step / .failed_endow_index.
 */
int aos_child_spawn(const aos_child_spawn_req_t *req,
                     aos_child_spawn_result_t *out);
