/*
 * child_spawn.c — the run-time domain-creation primitive: implementation.
 *
 * See child_spawn.h for the full design rationale. This file is the AArch64
 * mechanics: real seL4_Untyped_Retype / seL4_ARM_* / seL4_TCB_* calls,
 * mirroring platform/guest-ram/vmm_guest_paging.c (VSpace + page-table
 * retyping from a granted pool) and kernel/agentos-root-task/src/pd_tcb.c
 * (the seL4_TCB_Configure argument shape and the "WriteRegisters(resume=1)
 * only after the SC is bound" ordering) rather than inventing a new
 * sequence. Endowment reuses libs/pd-support/cap_lend.c's mint path
 * unchanged, including its revoke path for teardown.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "child_spawn.h"

#include <stddef.h>

#include "cap_lend.h"
#include "sel4_boot.h" /* AGENTOS_CTX_PC/SP/ARG0/ARG1 -- same portable
                        * register-name macros pd_tcb.c uses. */

/* ── Step identifiers (aos_child_spawn_result_t.failed_step) ────────────── */
enum {
    STEP_NONE = 0,
    STEP_CNODE,
    STEP_VSPACE,
    STEP_ASID,
    STEP_CONTENT_MAP,
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
};

/*
 * staging_t — the parent-local bookkeeping this one call needs to tear
 * down whatever it has retyped so far. Deliberately NOT persistent module
 * state: aos_child_spawn() is a single call that either finishes with a
 * running, fully endowed child or leaves nothing new behind.
 */
typedef struct {
    seL4_CPtr self_cnode;
    seL4_Word self_cnode_bits; /* radix of self_cnode -- the depth argument
                                * every CNode_Delete on it must use; req's
                                * own self-reference cap resolves to the
                                * full CSpace root, not a single CNode
                                * level, so this is NOT seL4_WordBits */
    seL4_Word base_slot;    /* req->scratch_slot */
    seL4_Word next_slot;    /* next free slot == base_slot + used so far */
    seL4_Word limit_slot;   /* base_slot + req->scratch_count */
} staging_t;

static void staging_init(staging_t *st, const aos_child_spawn_req_t *req)
{
    st->self_cnode      = req->self_cnode;
    st->self_cnode_bits = req->self_cnode_bits;
    st->base_slot  = req->scratch_slot;
    st->next_slot  = req->scratch_slot;
    st->limit_slot = req->scratch_slot + req->scratch_count;
}

/* Claim the next free scratch slot, or seL4_CapNull if none remain. */
static seL4_CPtr staging_claim(staging_t *st)
{
    if (st->next_slot >= st->limit_slot) {
        return seL4_CapNull;
    }
    return (seL4_CPtr)(st->next_slot++);
}

/*
 * staging_teardown — delete every capability this call has staged, in
 * reverse order of creation.
 *
 * This deletes the parent's own CAPABILITY REFERENCE to each retyped
 * object. It does NOT -- cannot -- return the underlying bytes inside
 * req->pool_ut to the pool: seL4_Untyped_Retype is a bump allocator with
 * no inverse operation. See child_spawn.h's file header ("Untyped
 * exhaustion"). Reverse order is used so a partial TCB (which references
 * the CNode/VSpace/frames created before it) is always deleted before the
 * objects it referenced, even though these are independent peer objects
 * (not a derivation chain) and the kernel does not require any particular
 * order here.
 */
static void staging_teardown(staging_t *st)
{
    while (st->next_slot > st->base_slot) {
        st->next_slot--;
        (void)seL4_CNode_Delete(st->self_cnode, (seL4_Word)st->next_slot,
                                 (seL4_Uint8)st->self_cnode_bits);
    }
}

/* ── Retype helpers ───────────────────────────────────────────────────── */

static seL4_Error retype_one(const aos_child_spawn_req_t *req, staging_t *st,
                              seL4_Word type, seL4_Word size_bits, seL4_CPtr *out)
{
    seL4_CPtr slot = staging_claim(st);
    if (slot == seL4_CapNull) {
        return seL4_NotEnoughMemory;
    }
    seL4_Error err = seL4_Untyped_Retype(req->pool_ut, type, size_bits,
                                          req->self_cnode, 0u, 0u, slot, 1u);
    if (err != seL4_NoError) {
        /* Nothing landed in the claimed slot; give it back so accounting
         * stays exact rather than leaving a hole staging_teardown would
         * harmlessly but pointlessly try to delete. */
        st->next_slot--;
        return err;
    }
    *out = slot;
    return seL4_NoError;
}

/*
 * map_frame_retrying — map `frame` into `vspace` at `va`, retyping
 * intermediate page-table objects from the pool on seL4_FailedLookup,
 * exactly as aos_vmm_guest_page_map() does (platform/guest-ram/
 * vmm_guest_paging.c) -- AArch64 has at most three paging levels below the
 * VSpace root, so a failed map after AOS_CHILD_SPAWN_MAX_PT_LEVELS retypes
 * means something other than "missing table" is wrong and this gives up
 * rather than looping forever.
 */
static seL4_Error map_frame_retrying(const aos_child_spawn_req_t *req, staging_t *st,
                                      seL4_CPtr frame, seL4_CPtr vspace, seL4_Word va)
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
        seL4_Error rt = retype_one(req, st, seL4_ARM_PageTableObject, 0u, &pt);
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

/* Unpack an aos_endow_cap_t.rights bitmask into seL4_CapRights_t. Bit
 * layout matches cap_lend.c's cap_lend_pack_rights() exactly (bit0=write,
 * bit1=read, bit2=grant, bit3=grantreply) so a parent_holdings declaration
 * built by packing a real seL4_CapRights_t round-trips through this and
 * back. aos_cap_lend() itself still rejects seL4_AllRights (no reduction
 * at all), independent of what this unpack produces. */
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
     * ACTUAL HOLDINGS before touching a single seL4 object. Task 1's
     * aos_endowment_validate() already enforces every structural and
     * subsetting rule (version, count bounds, parent_slot/kind match,
     * rights-subset); passing req->parent_holdings here -- a declaration
     * of what the parent actually has, not of what it merely intends -- is
     * what turns the Task 1 check from "is this request well-formed" into
     * "is this request something the parent can actually back."
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

    staging_t st;
    staging_init(&st, req);

    /* ── Step 1: retype the child's CNode, VSpace and TCB from the
     * parent's pool. Every retype below names req->pool_ut and nothing
     * else -- mirrors vmm_guest_paging.c:22 for the VSpace object,
     * blk_virt.c:518 for the general retype shape. ── */

    seL4_CPtr child_cnode;
    if (retype_one(req, &st, seL4_CapTableObject, (seL4_Word)req->cnode_size_bits,
                    &child_cnode) != seL4_NoError) {
        out->error = AOS_CHILD_SPAWN_ERR_RETYPE;
        out->failed_step = STEP_CNODE;
        staging_teardown(&st);
        return out->error;
    }

    seL4_CPtr child_vspace;
    if (retype_one(req, &st, seL4_ARM_VSpaceObject, 0u, &child_vspace) != seL4_NoError) {
        out->error = AOS_CHILD_SPAWN_ERR_RETYPE;
        out->failed_step = STEP_VSPACE;
        staging_teardown(&st);
        return out->error;
    }

    if (seL4_ARM_ASIDPool_Assign(req->asid_pool, child_vspace) != seL4_NoError) {
        out->error = AOS_CHILD_SPAWN_ERR_ASID;
        out->failed_step = STEP_ASID;
        staging_teardown(&st);
        return out->error;
    }

    /* Map the caller-populated content frame (see child_spawn.h: this
     * call never reads or copies image bytes itself). */
    if (map_frame_retrying(req, &st, req->content_frame, child_vspace,
                            req->content_va) != seL4_NoError) {
        out->error = AOS_CHILD_SPAWN_ERR_MAP;
        out->failed_step = STEP_CONTENT_MAP;
        staging_teardown(&st);
        return out->error;
    }

    /* Stack: one fresh, zeroed page (seL4_Untyped_Retype zero-fills new
     * objects), mapped below stack_va_top. */
    seL4_CPtr stack_frame;
    if (retype_one(req, &st, seL4_ARM_SmallPageObject, 0u, &stack_frame) != seL4_NoError) {
        out->error = AOS_CHILD_SPAWN_ERR_RETYPE;
        out->failed_step = STEP_STACK_FRAME;
        staging_teardown(&st);
        return out->error;
    }
    seL4_Word stack_va = req->stack_va_top - 0x1000u;
    if (map_frame_retrying(req, &st, stack_frame, child_vspace, stack_va) != seL4_NoError) {
        out->error = AOS_CHILD_SPAWN_ERR_MAP;
        out->failed_step = STEP_STACK_MAP;
        staging_teardown(&st);
        return out->error;
    }

    /* IPC buffer: one fresh page at req->ipc_buf_va. */
    seL4_CPtr ipc_frame;
    if (retype_one(req, &st, seL4_ARM_SmallPageObject, 0u, &ipc_frame) != seL4_NoError) {
        out->error = AOS_CHILD_SPAWN_ERR_RETYPE;
        out->failed_step = STEP_IPC_FRAME;
        staging_teardown(&st);
        return out->error;
    }
    if (map_frame_retrying(req, &st, ipc_frame, child_vspace, req->ipc_buf_va) != seL4_NoError) {
        out->error = AOS_CHILD_SPAWN_ERR_MAP;
        out->failed_step = STEP_IPC_MAP;
        staging_teardown(&st);
        return out->error;
    }

    seL4_CPtr child_tcb;
    if (retype_one(req, &st, seL4_TCBObject, 0u, &child_tcb) != seL4_NoError) {
        out->error = AOS_CHILD_SPAWN_ERR_RETYPE;
        out->failed_step = STEP_TCB;
        staging_teardown(&st);
        return out->error;
    }

    /* ── Step 3 (part 1): configure, but do not start. Mirrors
     * pd_tcb.c:30's seL4_TCB_Configure argument shape exactly. ── */
    seL4_Word cspace_root_data = (seL4_Word)(seL4_WordBits - (uint32_t)req->cnode_size_bits);
    if (seL4_TCB_Configure(child_tcb, child_cnode, cspace_root_data, child_vspace, 0u,
                            req->ipc_buf_va, ipc_frame) != seL4_NoError) {
        out->error = AOS_CHILD_SPAWN_ERR_CONFIGURE;
        out->failed_step = STEP_CONFIGURE;
        staging_teardown(&st);
        return out->error;
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
        staging_teardown(&st);
        return out->error;
    }

#ifdef CONFIG_KERNEL_MCS
    seL4_CPtr sc;
    if (retype_one(req, &st, seL4_SchedContextObject, seL4_MinSchedContextBits, &sc) != seL4_NoError) {
        out->error = AOS_CHILD_SPAWN_ERR_RETYPE;
        out->failed_step = STEP_SC;
        staging_teardown(&st);
        return out->error;
    }
    if (seL4_SchedControl_ConfigureFlags(req->sched_control, sc,
            10000u /* budget us, same default as PD_DEFAULT_SC_BUDGET_US */,
            1000000u /* period us, same default as PD_DEFAULT_SC_PERIOD_US */,
            0u, 0u, 0u) != seL4_NoError) {
        out->error = AOS_CHILD_SPAWN_ERR_CONFIGURE;
        out->failed_step = STEP_SC_CONFIGURE;
        staging_teardown(&st);
        return out->error;
    }
    /* Binds the SC and sets mcp/priority/fault_ep; does NOT change the
     * thread's Inactive state -- only WriteRegisters(resume=1) below does
     * that, and that call is the LAST thing this function does on
     * success. */
    if (seL4_TCB_SetSchedParams(child_tcb, req->self_tcb, 255u, (seL4_Word)req->priority,
                                 sc, seL4_CapNull) != seL4_NoError) {
        out->error = AOS_CHILD_SPAWN_ERR_CONFIGURE;
        out->failed_step = STEP_SCHED_PARAMS;
        staging_teardown(&st);
        return out->error;
    }
#else
    if (seL4_TCB_SetPriority(child_tcb, req->self_tcb, (seL4_Word)req->priority) != seL4_NoError) {
        out->error = AOS_CHILD_SPAWN_ERR_CONFIGURE;
        out->failed_step = STEP_SCHED_PARAMS;
        staging_teardown(&st);
        return out->error;
    }
#endif

    /*
     * ── Step 2 (mechanics): endow using T5's mint path. ──
     *
     * The child's TCB is fully configured and provably Inactive (never
     * resumed) at this point, so minting into its CNode cannot race
     * anything the child's own thread could do -- there is no child
     * thread yet, only a CNode object. Each successful mint is also
     * automatically recorded into the PARENT's cap_lend lease table (see
     * cap_lend.c) -- that record is this spawn's report, not proof (see
     * child_spawn.h).
     */
    seL4_Word minted = 0u;
    for (seL4_Word i = 0u; i < req->endow->count; i++) {
        const aos_endow_cap_t *c = &req->endow->caps[i];
        seL4_CapRights_t rights = unpack_rights(c->rights);
        int lend_err = aos_cap_lend(req->self_cnode, req->endow_cptrs[i],
                                     req->self_cnode_bits,
                                     child_cnode, req->endow_child_base_slot + i,
                                     (seL4_Word)req->cnode_size_bits, rights, req->endow_badge);
        if (lend_err != AOS_CAP_LEND_OK) {
            /* Undo every endowment that DID succeed -- aos_cap_lend_revoke
             * removes the mint from the child's CNode and closes its
             * lease record, exactly as T5 intends for a withdrawn loan. */
            for (seL4_Word j = 0u; j < minted; j++) {
                (void)aos_cap_lend_revoke(req->endow_cptrs[j]);
            }
            out->error = AOS_CHILD_SPAWN_ERR_ENDOW;
            out->failed_step = STEP_ENDOW;
            out->failed_endow_index = i;
            staging_teardown(&st);
            return out->error;
        }
        minted++;
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
         * caller to retry the resume or tear it down explicitly via the
         * handle below. */
        out->cnode  = child_cnode;
        out->vspace = child_vspace;
        out->tcb    = child_tcb;
        out->error  = AOS_CHILD_SPAWN_ERR_START;
        out->failed_step = STEP_START;
        return out->error;
    }

    out->cnode  = child_cnode;
    out->vspace = child_vspace;
    out->tcb    = child_tcb;
    out->error  = AOS_CHILD_SPAWN_OK;
    return AOS_CHILD_SPAWN_OK;
}
