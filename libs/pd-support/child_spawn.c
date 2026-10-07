/*
 * child_spawn.c — the run-time domain-creation primitive: implementation.
 *
 * See child_spawn.h for the full design rationale. This file is the AArch64
 * mechanics: real seL4_Untyped_Retype / seL4_ARM_* / seL4_TCB_* calls,
 * mirroring platform/guest-ram/vmm_guest_paging.c (VSpace + page-table
 * retyping from a granted pool) and kernel/agentos-root-task/src/pd_tcb.c
 * (the seL4_TCB_Configure argument shape and the "WriteRegisters(resume=1)
 * only after the SC is bound" ordering) rather than inventing a new
 * sequence.
 *
 * Endowment is a direct seL4_CNode_Mint, not a call into T5's
 * libs/pd-support/cap_lend.c -- see child_spawn.h's "Endowment: a MINT,
 * not a loan" section for why reusing aos_cap_lend's revoke-on-teardown
 * would have been a cross-domain authority bug. This file does not
 * include cap_lend.h at all.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "child_spawn.h"

#include <stddef.h>

#include "sel4_boot.h" /* AGENTOS_CTX_PC/SP/ARG0/ARG1 -- same portable
                        * register-name macros pd_tcb.c uses. */

/*
 * This file is MCS-only, stated here rather than papered over with a
 * non-working fallback branch (pd_tcb.c makes the same assumption the
 * same way). The seL4_TCB_Configure call below is the MCS seven-argument
 * form (no fault_ep parameter -- that is set later via
 * seL4_TCB_SetSchedParams, together with the SchedContext bind), and the
 * SchedContext retype/configure/SetSchedParams sequence has no non-MCS
 * equivalent in this file at all.
 */
#ifndef CONFIG_KERNEL_MCS
#error "child_spawn.c requires an MCS kernel"
#endif

/* ── Step identifiers (aos_child_spawn_result_t.failed_step) ────────────── */
enum {
    STEP_NONE = 0,
    STEP_CNODE,
    STEP_VSPACE,
    STEP_ASID,
    STEP_CONTENT_MAP,
    STEP_EXTRA_MAP,
    STEP_STACK_FRAME,
    STEP_STACK_MAP,
    STEP_IPC_FRAME,
    STEP_IPC_MAP,
    STEP_TCB,
    STEP_CONFIGURE,
    STEP_REGS,
    STEP_SC,
    STEP_SC_CONFIGURE,
    STEP_SCHED_PARAMS,
    STEP_ENDOW,
    STEP_START,
    /* Appended, not inserted: existing values must not renumber. */
    STEP_FAULT_INSTALL,
};

/* ── aos_pt_scratch_t: shared bounded-retry page-table allocator ───────── */

void aos_pt_scratch_init(aos_pt_scratch_t *sc, seL4_CPtr pool_ut,
                          seL4_CPtr self_cnode, seL4_Word self_cnode_bits,
                          seL4_Word base_slot, seL4_Word count)
{
    sc->pool_ut         = pool_ut;
    sc->self_cnode      = self_cnode;
    sc->self_cnode_bits = self_cnode_bits;
    sc->base_slot       = base_slot;
    sc->next_slot       = base_slot;
    sc->limit_slot      = base_slot + count;
}

/*
 * Claim the next free scratch slot, or seL4_CapNull if none remain.
 *
 * This overloads seL4_CapNull (slot 0) as the exhaustion sentinel, so a
 * scratch range BASED AT SLOT 0 would make its own first slot
 * indistinguishable from "none left" and retype_one() would report
 * seL4_NotEnoughMemory for a perfectly good allocation. Slot 0 is the null
 * capability in every CSpace in this tree and no caller can usefully stage
 * objects there, so the constraint costs nothing -- but it is a real
 * precondition on req->scratch_slot and is stated here rather than left
 * implicit. contracts/child_spawn_contract.h's _Static_assert block keeps
 * AOS_CHILD_SPAWN_SCRATCH_BASE above every fixed slot, which enforces it
 * for the only caller in this tree.
 */
static seL4_CPtr pt_scratch_claim(aos_pt_scratch_t *sc)
{
    if (sc->next_slot >= sc->limit_slot) {
        return seL4_CapNull;
    }
    return (seL4_CPtr)(sc->next_slot++);
}

/*
 * retype_one — retype one object of `type`/`size_bits` from sc->pool_ut
 * into a freshly claimed slot in sc->self_cnode. Used both for page
 * tables (via aos_pt_map_retrying below) and for every other object
 * aos_child_spawn() retypes (CNode, VSpace, frames, TCB, SchedContext).
 */
static seL4_Error retype_one(aos_pt_scratch_t *sc, seL4_Word type,
                              seL4_Word size_bits, seL4_CPtr *out)
{
    seL4_CPtr slot = pt_scratch_claim(sc);
    if (slot == seL4_CapNull) {
        return seL4_NotEnoughMemory;
    }
    seL4_Error err = seL4_Untyped_Retype(sc->pool_ut, type, size_bits,
                                          sc->self_cnode, 0u, 0u, slot, 1u);
    if (err != seL4_NoError) {
        /* Nothing landed in the claimed slot; give it back so accounting
         * stays exact rather than leaving a hole teardown would harmlessly
         * but pointlessly try to delete. */
        sc->next_slot--;
        return err;
    }
    *out = slot;
    return seL4_NoError;
}

seL4_Error aos_pt_map_retrying(aos_pt_scratch_t *sc, seL4_CPtr frame,
                                seL4_CPtr vspace, seL4_Word va)
{
    for (unsigned attempt = 0; attempt < AOS_CHILD_SPAWN_MAX_PT_LEVELS; attempt++) {
        seL4_Error err = seL4_ARM_Page_Map(frame, vspace, va, seL4_AllRights,
                                            seL4_ARM_Default_VMAttributes);
        if (err == seL4_NoError) {
            return seL4_NoError;
        }
        if (err != seL4_FailedLookup || attempt + 1 == AOS_CHILD_SPAWN_MAX_PT_LEVELS) {
            return err;
        }
        seL4_CPtr pt;
        seL4_Error rt = retype_one(sc, seL4_ARM_PageTableObject, 0u, &pt);
        if (rt != seL4_NoError) {
            return rt;
        }
        seL4_Error mt = seL4_ARM_PageTable_Map(pt, vspace, va, seL4_ARM_Default_VMAttributes);
        if (mt != seL4_NoError) {
            return mt;
        }
    }
    return seL4_FailedLookup;
}

/*
 * staging_teardown — delete every capability aos_child_spawn() has staged
 * in `sc`, in reverse order of creation.
 *
 * This deletes the parent's own CAPABILITY REFERENCE to each retyped
 * object -- including, when it was reached, the child's CNode itself.
 * Deleting the ONLY capability to that CNode destroys the CNode object
 * and every capability minted into its slots along with it (see
 * child_spawn.h's "The one hard rule" section): this is how a partially
 * endowed child's mints are undone, NOT by touching the parent's own
 * original capabilities, which this function never names.
 *
 * It does NOT -- cannot -- return the underlying bytes inside
 * sc->pool_ut to the pool: seL4_Untyped_Retype is a bump allocator with
 * no inverse operation. See child_spawn.h's file header ("Untyped
 * exhaustion"). Reverse order is used so a partial TCB (which references
 * the CNode/VSpace/frames created before it) is always deleted before the
 * objects it referenced, even though these are independent peer objects
 * (not a derivation chain) and the kernel does not require any particular
 * order here.
 */
static void staging_teardown(aos_pt_scratch_t *sc)
{
    while (sc->next_slot > sc->base_slot) {
        sc->next_slot--;
        (void)seL4_CNode_Delete(sc->self_cnode, (seL4_Word)sc->next_slot,
                                 (seL4_Uint8)sc->self_cnode_bits);
    }
}

/*
 * fail_teardown — the common failure path once CALLER-OWNED frames may
 * already be mapped into the child's (about-to-be-destroyed) VSpace.
 *
 * req->content_frame and every req->extra_maps[i].frame are CALLER-owned
 * and never staged (so staging_teardown never touches them), but
 * seL4_ARM_Page_Map records the mapping ON THE FRAME CAPABILITY itself.
 * If this call leaves those mappings in place and then destroys the VSpace
 * they pointed into, the caller's own frame caps are left believing they
 * are mapped somewhere that no longer exists -- a retry with the SAME
 * frames would then fail at a mapping step for a completely different
 * reason than the original failure. Unmapping them here, before tearing
 * down the objects they were mapped into, keeps a retry possible with the
 * same frames. tests/child-spawn/parent_pd.c exercises exactly this on
 * target: its doomed spawn (Probe 3) and its real spawn pass the SAME
 * content_frame capability, so dropping this unmap makes the real spawn
 * fail at STEP_CONTENT_MAP and Probe 1's marker never appear.
 *
 * `extra_mapped` is the number of req->extra_maps entries that were
 * successfully mapped, so a failure PART WAY through the extra-map loop
 * unmaps exactly the ones that landed and leaves the rest alone.
 */
static void fail_teardown(aos_pt_scratch_t *sc, int content_mapped,
                           seL4_CPtr content_frame,
                           const aos_child_spawn_map_t *extra_maps,
                           seL4_Word extra_mapped)
{
    for (seL4_Word i = extra_mapped; i > 0u; i--) {
        (void)seL4_ARM_Page_Unmap(extra_maps[i - 1u].frame);
    }
    if (content_mapped) {
        (void)seL4_ARM_Page_Unmap(content_frame);
    }
    staging_teardown(sc);
}

/* Unpack an aos_endow_cap_t.rights bitmask into seL4_CapRights_t. Bit
 * layout matches cap_lend.c's cap_lend_pack_rights() exactly (bit0=write,
 * bit1=read, bit2=grant, bit3=grantreply), kept consistent so a
 * parent_holdings declaration built by packing a real seL4_CapRights_t
 * round-trips through this and back -- even though this module no longer
 * calls into cap_lend.c itself. seL4_CNode_Mint masks the requested
 * rights against the source capability's actual rights, so this unpack
 * can never WIDEN authority no matter what it produces; equality with the
 * source's rights is permitted (Task 1's validator allows it -- see
 * endowment_contract.h), unlike T5's aos_cap_lend(), which deliberately
 * refuses to lend at full rights because a LOAN must always narrow. An
 * endowment is not a loan: the child may receive exactly what the parent
 * holds. */
static seL4_CapRights_t unpack_rights(uint32_t bits)
{
    return seL4_CapRights_new(
        (bits & (1u << 3)) ? 1 : 0,  /* grantreply */
        (bits & (1u << 2)) ? 1 : 0,  /* grant */
        (bits & (1u << 1)) ? 1 : 0,  /* read */
        (bits & (1u << 0)) ? 1 : 0); /* write */
}

/* ── aos_child_spawn ──────────────────────────────────────────────────── */

int aos_child_spawn(const aos_child_spawn_req_t *req, aos_child_spawn_result_t *out)
{
    if (out == NULL) {
        return AOS_CHILD_SPAWN_ERR_VALIDATE;
    }
    *out = (aos_child_spawn_result_t){0};

    if (req == NULL) {
        out->error = AOS_CHILD_SPAWN_ERR_VALIDATE;
        return out->error;
    }

    /*
     * Step 2 of the brief: validate the endowment against the PARENT'S
     * DECLARED HOLDINGS before touching a single seL4 object. Task 1's
     * aos_endowment_validate() enforces every structural and subsetting
     * rule (version, count bounds, parent_slot/kind match, rights-subset-
     * or-equal). req->parent_holdings is caller-asserted, not
     * independently measured (see child_spawn.h's doc comment on that
     * field) -- this check catches a malformed or internally-inconsistent
     * request before any seL4 object is built; the kernel's own
     * seL4_CNode_Mint rights-masking, at mint time below, is what actually
     * enforces "a derivative can never exceed its source."
     */
    if (aos_endowment_validate(req->endow, req->parent_holdings) != 0) {
        out->error = AOS_CHILD_SPAWN_ERR_VALIDATE;
        out->failed_step = STEP_NONE;
        return out->error;
    }
    if (req->endow_cptrs == NULL && req->endow->count != 0u) {
        out->error = AOS_CHILD_SPAWN_ERR_VALIDATE;
        return out->error;
    }
    if (req->scratch_count < AOS_CHILD_SPAWN_SCRATCH_SLOTS) {
        out->error = AOS_CHILD_SPAWN_ERR_SCRATCH;
        return out->error;
    }

    if (req->extra_map_count != 0u && req->extra_maps == NULL) {
        out->error = AOS_CHILD_SPAWN_ERR_VALIDATE;
        return out->error;
    }

    aos_pt_scratch_t st;
    aos_pt_scratch_init(&st, req->pool_ut, req->self_cnode, req->self_cnode_bits,
                         req->scratch_slot, req->scratch_count);
    int content_mapped = 0;
    seL4_Word extra_mapped = 0u;

    /*
     * Everything this call appends to the endowment-delta ledger between
     * here and the final resume=1 is provisional: a child that does not
     * end up running was never endowed, whatever mints the kernel accepted
     * on the way (its CNode, and every mint in it, is destroyed by
     * teardown). Take the mark now and roll back to it on EVERY failure
     * exit, including AOS_CHILD_SPAWN_ERR_START -- see child_spawn.h's
     * "Reporting" section.
     */
    aos_endow_ledger_mark_t ledger_mark = aos_endow_ledger_mark(req->ledger);

    /* ── Step 1: retype the child's CNode, VSpace and TCB from the
     * parent's pool. Every retype below names req->pool_ut (via `st`) and
     * nothing else -- mirrors vmm_guest_paging.c:22 for the VSpace
     * object, blk_virt.c:518 for the general retype shape. ── */

    seL4_CPtr child_cnode;
    if (retype_one(&st, seL4_CapTableObject, (seL4_Word)req->cnode_size_bits,
                    &child_cnode) != seL4_NoError) {
        out->error = AOS_CHILD_SPAWN_ERR_RETYPE;
        out->failed_step = STEP_CNODE;
        fail_teardown(&st, content_mapped, req->content_frame, req->extra_maps, extra_mapped);
        goto done_fail;
    }

    seL4_CPtr child_vspace;
    if (retype_one(&st, seL4_ARM_VSpaceObject, 0u, &child_vspace) != seL4_NoError) {
        out->error = AOS_CHILD_SPAWN_ERR_RETYPE;
        out->failed_step = STEP_VSPACE;
        fail_teardown(&st, content_mapped, req->content_frame, req->extra_maps, extra_mapped);
        goto done_fail;
    }

    if (seL4_ARM_ASIDPool_Assign(req->asid_pool, child_vspace) != seL4_NoError) {
        out->error = AOS_CHILD_SPAWN_ERR_ASID;
        out->failed_step = STEP_ASID;
        fail_teardown(&st, content_mapped, req->content_frame, req->extra_maps, extra_mapped);
        goto done_fail;
    }

    /* Map the caller-populated content frame (see child_spawn.h: this
     * call never reads or copies image bytes itself, and performs no
     * cache maintenance -- that is the caller's responsibility before
     * this point). aos_pt_map_retrying() maps every frame -- content,
     * stack, and IPC buffer alike -- with seL4_AllRights; the content
     * page therefore ends up writable as well as executable in the
     * child, which pd_vspace.c's per-segment ELF loader is more careful
     * about for normal PDs. Not required by the brief and not a
     * subsetting-invariant issue (the child's rights here are a property
     * of ITS OWN image mapping, not of what was endowed to it), but a
     * caller wanting W^X for the content page specifically would need a
     * second mapping entry point; nothing in this demo needs it yet. */
    if (aos_pt_map_retrying(&st, req->content_frame, child_vspace,
                             req->content_va) != seL4_NoError) {
        out->error = AOS_CHILD_SPAWN_ERR_MAP;
        out->failed_step = STEP_CONTENT_MAP;
        fail_teardown(&st, content_mapped, req->content_frame, req->extra_maps, extra_mapped);
        goto done_fail;
    }
    content_mapped = 1;

    /*
     * Endowed memory: map every caller-supplied extra frame into the
     * child's VSpace, before the child exists as a runnable thread. These
     * are grants of the caller's own memory authority, so they belong
     * inside the "no partially endowed child ever runs" window exactly as
     * the CNode mints below do -- a mapping installed after this function
     * returned would be authority arriving at an already-executing domain.
     * Like content_frame, these frames are caller-owned: this module maps
     * them and nothing else (see aos_child_spawn_map_t).
     */
    for (seL4_Word i = 0u; i < req->extra_map_count; i++) {
        if (aos_pt_map_retrying(&st, req->extra_maps[i].frame, child_vspace,
                                 req->extra_maps[i].va) != seL4_NoError) {
            out->error = AOS_CHILD_SPAWN_ERR_MAP;
            out->failed_step = STEP_EXTRA_MAP;
            fail_teardown(&st, content_mapped, req->content_frame, req->extra_maps, extra_mapped);
            goto done_fail;
        }
        extra_mapped++;
    }

    /* Stack: one fresh, zeroed page (seL4_Untyped_Retype zero-fills new
     * objects), mapped below stack_va_top. */
    seL4_CPtr stack_frame;
    if (retype_one(&st, seL4_ARM_SmallPageObject, 0u, &stack_frame) != seL4_NoError) {
        out->error = AOS_CHILD_SPAWN_ERR_RETYPE;
        out->failed_step = STEP_STACK_FRAME;
        fail_teardown(&st, content_mapped, req->content_frame, req->extra_maps, extra_mapped);
        goto done_fail;
    }
    seL4_Word stack_va = req->stack_va_top - 0x1000u;
    if (aos_pt_map_retrying(&st, stack_frame, child_vspace, stack_va) != seL4_NoError) {
        out->error = AOS_CHILD_SPAWN_ERR_MAP;
        out->failed_step = STEP_STACK_MAP;
        fail_teardown(&st, content_mapped, req->content_frame, req->extra_maps, extra_mapped);
        goto done_fail;
    }

    /* IPC buffer: one fresh page at req->ipc_buf_va. */
    seL4_CPtr ipc_frame;
    if (retype_one(&st, seL4_ARM_SmallPageObject, 0u, &ipc_frame) != seL4_NoError) {
        out->error = AOS_CHILD_SPAWN_ERR_RETYPE;
        out->failed_step = STEP_IPC_FRAME;
        fail_teardown(&st, content_mapped, req->content_frame, req->extra_maps, extra_mapped);
        goto done_fail;
    }
    if (aos_pt_map_retrying(&st, ipc_frame, child_vspace, req->ipc_buf_va) != seL4_NoError) {
        out->error = AOS_CHILD_SPAWN_ERR_MAP;
        out->failed_step = STEP_IPC_MAP;
        fail_teardown(&st, content_mapped, req->content_frame, req->extra_maps, extra_mapped);
        goto done_fail;
    }

    seL4_CPtr child_tcb;
    if (retype_one(&st, seL4_TCBObject, 0u, &child_tcb) != seL4_NoError) {
        out->error = AOS_CHILD_SPAWN_ERR_RETYPE;
        out->failed_step = STEP_TCB;
        fail_teardown(&st, content_mapped, req->content_frame, req->extra_maps, extra_mapped);
        goto done_fail;
    }

    /* ── Step 3 (part 1): configure, but do not start. Mirrors
     * pd_tcb.c:30's seL4_TCB_Configure argument shape exactly. This file
     * is MCS-only -- see the #ifndef CONFIG_KERNEL_MCS guard near the top
     * of this file. ── */
    seL4_Word cspace_root_data = (seL4_Word)(seL4_WordBits - (uint32_t)req->cnode_size_bits);
    if (seL4_TCB_Configure(child_tcb, child_cnode, cspace_root_data, child_vspace, 0u,
                            req->ipc_buf_va, ipc_frame) != seL4_NoError) {
        out->error = AOS_CHILD_SPAWN_ERR_CONFIGURE;
        out->failed_step = STEP_CONFIGURE;
        fail_teardown(&st, content_mapped, req->content_frame, req->extra_maps, extra_mapped);
        goto done_fail;
    }

    /*
     * Write initial register state with resume=0: this latches PC/SP/args
     * but does NOT enqueue the thread (seL4_TCB_WriteRegisters only
     * enqueues when resume=1). The thread is Inactive and cannot run no
     * matter what else this function does next -- endowment below is
     * therefore race-free by construction, not by careful ordering alone.
     */
    seL4_UserContext regs;
    for (uint32_t i = 0; i < sizeof(regs) / sizeof(seL4_Word); i++) {
        ((seL4_Word *)&regs)[i] = 0;
    }
    regs.AGENTOS_CTX_PC   = req->entry_point;
    regs.AGENTOS_CTX_SP   = req->stack_va_top;
    regs.AGENTOS_CTX_ARG0 = req->arg0;
    regs.AGENTOS_CTX_ARG1 = req->arg1;
    if (seL4_TCB_WriteRegisters(child_tcb, 0 /* resume */, 0 /* arch_flags */,
            (seL4_Word)(sizeof(seL4_UserContext) / sizeof(seL4_Word)), &regs) != seL4_NoError) {
        out->error = AOS_CHILD_SPAWN_ERR_CONFIGURE;
        out->failed_step = STEP_REGS;
        fail_teardown(&st, content_mapped, req->content_frame, req->extra_maps, extra_mapped);
        goto done_fail;
    }

    /*
     * Fault-handler provenance. Two shapes, selected by
     * req->fault_install_ep (see its doc comment in child_spawn.h):
     *
     *   Delegated install (fault_install_ep != seL4_CapNull). Hand the
     *     child's TCB capability to an installer service and let IT write
     *     its OWN fault-handler capability into the TCB, so this caller
     *     never holds a capability to the endpoint its child's faults are
     *     reported on. It has to happen HERE, before the SchedContext
     *     exists: the installer's seL4_TCB_SetSchedParams necessarily
     *     passes a null SchedContext capability, and the kernel treats
     *     that as "no change" only while the TCB has none bound -- after
     *     a bind, the same null argument would UNBIND it
     *     (decodeSetSchedParams: `if (tcb->tcbSchedContext != sc)`).
     *
     *   Direct install (the default). req->fault_ep is written into the
     *     TCB by this call's own SetSchedParams below.
     *
     * The child's TCB is still Inactive either way: nothing in this block
     * resumes it.
     */
    if (req->fault_install_ep != seL4_CapNull) {
        seL4_SetCap(0, child_tcb);
        seL4_MessageInfo_t install_reply = seL4_Call(req->fault_install_ep,
            /* The mask is not cosmetic: seL4_MessageInfo_new asserts the
             * label fits in 52 bits, and with a non-constant label that
             * assert survives into the link as __assert_fail. */
            seL4_MessageInfo_new(req->fault_install_label & 0xfffffffffffffull,
                                 0u, 1u, 0u));
        if (seL4_MessageInfo_get_label(install_reply) != 0u) {
            out->error = AOS_CHILD_SPAWN_ERR_CONFIGURE;
            out->failed_step = STEP_FAULT_INSTALL;
            fail_teardown(&st, content_mapped, req->content_frame, req->extra_maps, extra_mapped);
            goto done_fail;
        }
    }

    seL4_CPtr sc;
    if (retype_one(&st, seL4_SchedContextObject, seL4_MinSchedContextBits, &sc) != seL4_NoError) {
        out->error = AOS_CHILD_SPAWN_ERR_RETYPE;
        out->failed_step = STEP_SC;
        fail_teardown(&st, content_mapped, req->content_frame, req->extra_maps, extra_mapped);
        goto done_fail;
    }
    if (seL4_SchedControl_ConfigureFlags(req->sched_control, sc,
            10000u /* budget us, same default as PD_DEFAULT_SC_BUDGET_US */,
            1000000u /* period us, same default as PD_DEFAULT_SC_PERIOD_US */,
            0u, 0u, 0u) != seL4_NoError) {
        out->error = AOS_CHILD_SPAWN_ERR_CONFIGURE;
        out->failed_step = STEP_SC_CONFIGURE;
        fail_teardown(&st, content_mapped, req->content_frame, req->extra_maps, extra_mapped);
        goto done_fail;
    }
    if (req->fault_install_ep != seL4_CapNull) {
        /* Bind ONLY the SchedContext. seL4_TCB_SetSchedParams would also
         * rewrite the fault handler -- thread_control_sched_update_fault
         * is unconditional in decodeSetSchedParams -- and would therefore
         * clear what the installer just wrote. mcp and priority were set
         * by the installer. Binding does not make the thread runnable:
         * it is Inactive until WriteRegisters(resume=1) below. */
        if (seL4_SchedContext_Bind(sc, child_tcb) != seL4_NoError) {
            out->error = AOS_CHILD_SPAWN_ERR_CONFIGURE;
            out->failed_step = STEP_SCHED_PARAMS;
            fail_teardown(&st, content_mapped, req->content_frame, req->extra_maps, extra_mapped);
            goto done_fail;
        }
    } else {
        /* Binds the SC and sets mcp/priority/fault_ep; does NOT change the
         * thread's Inactive state -- only WriteRegisters(resume=1) below
         * does that, and that call is the LAST thing this function does on
         * success.
         *
         * req->fault_ep is the child's fault handler. It is written into
         * the TCB, never into the child's CSpace, so the child can neither
         * name nor forge it -- but the CALLER necessarily holds send rights
         * on it (see the field's doc comment). A seL4_CapNull here simply
         * means nobody is told when the child faults. */
        if (seL4_TCB_SetSchedParams(child_tcb, req->self_tcb, 255u,
                                     (seL4_Word)req->priority,
                                     sc, req->fault_ep) != seL4_NoError) {
            out->error = AOS_CHILD_SPAWN_ERR_CONFIGURE;
            out->failed_step = STEP_SCHED_PARAMS;
            fail_teardown(&st, content_mapped, req->content_frame, req->extra_maps, extra_mapped);
            goto done_fail;
        }
    }

    /*
     * ── Step 2 (mechanics): endow with a direct seL4_CNode_Mint. ──
     *
     * See child_spawn.h's "Endowment: a MINT, not a loan" for why this is
     * NOT aos_cap_lend(): a loan's revoke-on-teardown would destroy every
     * derivative of the source capability system-wide, which would strip
     * any OTHER child the parent has already, correctly, endowed with the
     * same original. The child's TCB is fully configured and provably
     * Inactive (never resumed) at this point, so minting into its CNode
     * cannot race anything the child's own thread could do -- there is no
     * child thread yet, only a CNode object.
     */
    for (seL4_Word i = 0u; i < req->endow->count; i++) {
        const aos_endow_cap_t *c = &req->endow->caps[i];
        seL4_CapRights_t rights = unpack_rights(c->rights);
        seL4_Error mint_err = seL4_CNode_Mint(
            child_cnode, req->endow_child_base_slot + i, (seL4_Uint8)req->cnode_size_bits,
            req->self_cnode, req->endow_cptrs[i], (seL4_Uint8)req->self_cnode_bits,
            rights, req->endow_badge);
        if (mint_err != seL4_NoError) {
            /*
             * Do NOT touch req->endow_cptrs[j] for any j -- those are the
             * PARENT'S OWN originals and this function never revokes or
             * deletes anything the parent holds. Tearing down the child's
             * CNode (inside fail_teardown -> staging_teardown) destroys
             * every mint already placed in it, including the i entries
             * that succeeded before this one, without touching the
             * parent's side of any of them.
             */
            out->error = AOS_CHILD_SPAWN_ERR_ENDOW;
            out->failed_step = STEP_ENDOW;
            out->failed_endow_index = i;
            fail_teardown(&st, content_mapped, req->content_frame, req->extra_maps, extra_mapped);
            goto done_fail;
        }
        /*
         * Self-report this mint -- and only now that the kernel has
         * actually accepted it. The ledger records what was asked for and
         * granted; it cannot read back what the child holds, because seL4
         * has no capability-enumeration syscall. See
         * platform/endow_ledger.h. A NULL ledger records nothing and
         * changes nothing about the mint.
         *
         * The return value is deliberately discarded: running out of
         * ledger space is a REPORTING limit, not a reason to refuse a
         * child authority its parent legitimately holds, so a spawn that
         * overflows the ledger still succeeds. What must then survive is
         * the ledger's own .dropped counter, which is the only record that
         * the resulting report is incomplete -- which is why
         * aos_endow_ledger_rollback() restores the mark's .dropped rather
         * than clearing it.
         */
        (void)aos_endow_ledger_record(req->ledger, req->child_index,
                                       req->child_name, c->kind, c->rights,
                                       (uint64_t)req->endow_badge);
    }

    /* ── Step 3 (part 2): start, only now that every endowment has
     * succeeded. This is the ONE call in this function that makes the
     * child's thread runnable. ── */
    if (seL4_TCB_WriteRegisters(child_tcb, 1 /* resume */, 0 /* arch_flags */,
            (seL4_Word)(sizeof(seL4_UserContext) / sizeof(seL4_Word)), &regs) != seL4_NoError) {
        /* See child_spawn.h on AOS_CHILD_SPAWN_ERR_START: every endowment
         * already succeeded for a reason unrelated to this failure, so
         * this does NOT unwind them automatically. The child is still
         * Inactive -- it never ran -- but is left in place for the
         * caller to retry the resume or tear it down explicitly using
         * the full [scratch_base, scratch_next) range reported below.
         *
         * The ledger IS rolled back here, unlike the objects: the child
         * never ran, so reporting its endowment would describe authority
         * nothing is exercising. If the caller retries the resume
         * successfully it owns re-recording the delta; this module cannot
         * know whether it will. */
        aos_endow_ledger_rollback(req->ledger, ledger_mark);
        out->cnode  = child_cnode;
        out->vspace = child_vspace;
        out->tcb    = child_tcb;
        out->error  = AOS_CHILD_SPAWN_ERR_START;
        out->failed_step = STEP_START;
        out->scratch_base = st.base_slot;
        out->scratch_next = st.next_slot;
        return out->error;
    }

    out->cnode  = child_cnode;
    out->vspace = child_vspace;
    out->tcb    = child_tcb;
    out->error  = AOS_CHILD_SPAWN_OK;
    out->scratch_base = st.base_slot;
    out->scratch_next = st.next_slot;
    return AOS_CHILD_SPAWN_OK;

done_fail:
    /* No child started, so nothing this call minted is held by anybody:
     * the child's CNode is gone and every mint in it with it. */
    aos_endow_ledger_rollback(req->ledger, ledger_mark);
    out->scratch_base = st.base_slot;
    out->scratch_next = st.base_slot; /* fully torn down by fail_teardown */
    return out->error;
}
