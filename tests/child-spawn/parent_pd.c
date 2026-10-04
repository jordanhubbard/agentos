/*
 * parent_pd.c — child-spawn demonstration pair: parent side (test image
 * only).
 *
 * Built only under AGENTOS_CHILD_SPAWN_TEST (see system_desc_aarch64.c and
 * the root-task Makefile); absent from the default PD set. Exercises
 * libs/pd-support/child_spawn.c end-to-end against a real seL4 target, and
 * is the PD that emits three of T6 Task 3's four probe markers. The
 * fourth, Probe 2, is emitted by the ROOT TASK instead, because a PD
 * reporting that its own child faulted is not evidence of anything. Note
 * that this is a separation of roles, not an enforced one: root mints this
 * PD's fault endpoint with send rights, so nothing in the kernel stops a
 * PD in this position from fabricating the message -- see the oracle
 * comment in main.c's AGENTOS_CHILD_SPAWN_TEST ROOT_PROBE_* block. This
 * file simply does not do it.
 *
 *   1. Retype, from the pool root granted this PD at boot
 *      (AOS_CHILD_SPAWN_POOL_SLOT), the two objects this demo endows: a
 *      Notification, and a "gift" frame the Probe 1 pattern is written
 *      into. Both are freshly retyped here, so this PD holds them at full
 *      rights and every endowment below is a strict narrowing of
 *      something it really does hold.
 *   2. Retype one more frame from the same pool, temporarily map it into
 *      THIS PD's own VSpace, copy the child's image bytes in (already
 *      part of this PD's own verified ELF -- see
 *      __child_spawn_payload_start/end below), and unmap it. This and the
 *      gift-frame write are the only places in the whole demo that read or
 *      write the child's pages; aos_child_spawn() itself never does.
 *   3. PROBE 3 -- run a deliberately doomed spawn first. Its endowment
 *      names AOS_CHILD_SPAWN_ABSENT_SLOT, a permanently empty slot in this
 *      PD's own CNode, while its parent_holdings declaration claims a
 *      capability is there. Task 1's validator passes it, because
 *      parent_holdings is this PD's own ASSERTION and not an independent
 *      measurement (see child_spawn.h's doc comment on that field) -- the
 *      lie survives exactly as far as seL4_CNode_Mint, which resolves the
 *      slot and refuses. Assert that the first mint's success did not save
 *      the spawn, that nothing was left staged, that no signal ever
 *      arrived, and that the endowment ledger is still empty.
 *   4. PROBES 1/2/4 -- run the real spawn. Declare parent_holdings (this
 *      PD's own assertion of what it holds, not independently measured --
 *      true here because both objects were just freshly retyped) and
 *      endow a Signal-only derivative of the Notification plus a
 *      read/write derivative of the gift frame, which is also mapped into
 *      the child's VSpace before it starts. Wait for the child to signal,
 *      then map this PD's OWN capability to the gift frame back into its
 *      own VSpace and verify the child's response byte for byte --
 *      including that the physical address the CHILD reported for its own
 *      endowed frame capability matches the one this PD sees.
 *   5. PROBE 4 -- merge the endowment-delta ledger aos_child_spawn()
 *      appended to into a T4 authority snapshot and print it. A report,
 *      not a proof: see platform/endow_ledger.h.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <sel4/sel4.h>

#include <platform/authority.h>
#include <platform/endow_ledger.h>

#include "boot_info.h" /* AGENTOS_MEMORY_FENCE() */
#include "child_spawn.h"
#include "contracts/child_spawn_contract.h"
#include "sel4_ipc.h"
#include "serial_log.h"

static serial_log_t log_channel = {.ep = PD_CNODE_SLOT_SERIAL_EP};

/*
 * The parent's own endowment-delta ledger and the authority snapshot it is
 * rendered through. Statics, not stack: this PD's stack is 16 KiB and the
 * snapshot alone is ~2 KiB. Both are this PD's private memory -- nothing
 * else in the system can read or write them, which is also exactly why
 * they are a self-REPORT and not evidence (platform/endow_ledger.h).
 */
static aos_endow_ledger_t       g_ledger;
static aos_authority_snapshot_t g_snapshot;
static char                     g_report[512];

/*
 * The child's loadable image bytes, linked directly into THIS PD's own
 * ELF by the Makefile's AGENTOS_CHILD_SPAWN_TEST objcopy rule (see
 * child_spawn.h's file header and tests/child-spawn/child_pd.c). These
 * symbols are the exact bytes T3 verifies as part of child_spawn_parent's
 * own ELF -- nothing in this file fetches an image from anywhere else.
 */
extern const uint8_t __child_spawn_payload_start[];
extern const uint8_t __child_spawn_payload_end[];

static void park(void)
{
    for (;;) {
        __asm__ volatile("wfe" ::: "memory");
    }
}

/* Report and stop. Every failure in this file is fatal: a partially
 * completed proof that keeps going would let a later marker appear
 * without the earlier one being true. */
static void fail(const char *message)
{
    serial_log_puts(&log_channel, message);
    park();
}

/*
 * Map `frame` at this PD's scratch VA, using the PD's own bounded-retry
 * page-table allocator. Only the FIRST call retypes page tables; later
 * calls at the same VA reuse them, which is why one aos_pt_scratch_t with
 * four slots covers every mapping this file makes.
 */
static aos_pt_scratch_t g_pt_scratch;

static seL4_Error scratch_map(seL4_CPtr frame)
{
    return aos_pt_map_retrying(&g_pt_scratch, frame,
                                AOS_CHILD_SPAWN_SELF_VSPACE_SLOT,
                                AOS_CHILD_SPAWN_PARENT_SCRATCH_VA);
}

/* Emit "<prefix>D<err>-<step>-<endow index>\n" in ONE serial_log_puts call,
 * so there is exactly one seL4_Call to serial_pd on a failure path.
 * result.error/.failed_step are debugging breadcrumbs, not a stable ABI
 * (see child_spawn.h). */
static void report_spawn_failure(const aos_child_spawn_result_t *result)
{
    char dbuf[16];
    seL4_Word e = (seL4_Word)(-result->error);
    seL4_Word s = result->failed_step;
    seL4_Word fi = result->failed_endow_index;
    seL4_Word i = 0;
    dbuf[i++] = 'D';
    dbuf[i++] = (char)('0' + (e / 10u) % 10u);
    dbuf[i++] = (char)('0' + (e % 10u));
    dbuf[i++] = '-';
    dbuf[i++] = (char)('0' + (s / 10u) % 10u);
    dbuf[i++] = (char)('0' + (s % 10u));
    dbuf[i++] = '-';
    dbuf[i++] = (char)('0' + (fi / 10u) % 10u);
    dbuf[i++] = (char)('0' + (fi % 10u));
    dbuf[i++] = '\n';
    dbuf[i] = '\0';
    serial_log_puts(&log_channel, dbuf);
}

void pd_main(seL4_CPtr endpoint, seL4_CPtr nameserver)
{
    (void)endpoint;
    (void)nameserver;

    aos_endow_ledger_init(&g_ledger);

    /* ── Step 1: retype the two objects this PD endows. Both are fresh,
     * so this PD holds them at full rights. ── */
    seL4_CPtr ntfn = AOS_CHILD_SPAWN_PARENT_NTFN_SLOT;
    if (seL4_Untyped_Retype(AOS_CHILD_SPAWN_POOL_SLOT, seL4_NotificationObject, 0u,
            AOS_CHILD_SPAWN_SELF_CNODE_SLOT, 0u, 0u, ntfn, 1u) != seL4_NoError) {
        fail("[child-spawn-parent] FAIL prep1\n");
    }

    seL4_CPtr content_frame = AOS_CHILD_SPAWN_CONTENT_FRAME_SLOT;
    if (seL4_Untyped_Retype(AOS_CHILD_SPAWN_POOL_SLOT, seL4_ARM_SmallPageObject, 0u,
            AOS_CHILD_SPAWN_SELF_CNODE_SLOT, 0u, 0u, content_frame, 1u) != seL4_NoError) {
        fail("[child-spawn-parent] FAIL prep2\n");
    }

    seL4_CPtr gift_frame = AOS_CHILD_SPAWN_GIFT_FRAME_SLOT;
    if (seL4_Untyped_Retype(AOS_CHILD_SPAWN_POOL_SLOT, seL4_ARM_SmallPageObject, 0u,
            AOS_CHILD_SPAWN_SELF_CNODE_SLOT, 0u, 0u, gift_frame, 1u) != seL4_NoError) {
        fail("[child-spawn-parent] FAIL prep3\n");
    }

    /*
     * AOS_CHILD_SPAWN_PARENT_SCRATCH_VA is not part of any region root's
     * normal PD construction mapped (ELF image, stack, IPC buffer), so it
     * has no page table yet -- retype on seL4_FailedLookup via the SAME
     * aos_pt_scratch_t/aos_pt_map_retrying() child_spawn.c itself uses
     * internally (see child_spawn.h), rather than a second hand-rolled
     * copy of that retry loop, using dedicated scratch slots outside
     * aos_child_spawn()'s own range.
     */
    aos_pt_scratch_init(&g_pt_scratch, AOS_CHILD_SPAWN_POOL_SLOT,
                         AOS_CHILD_SPAWN_SELF_CNODE_SLOT, AOS_CHILD_SPAWN_PARENT_CNODE_BITS,
                         AOS_CHILD_SPAWN_PARENT_SCRATCH_PT_BASE, 4u);

    /* ── Step 2: copy the child's image into the fresh (zero-filled by
     * retype) content frame via a transient mapping into THIS PD's own
     * VSpace. The frame is unmapped again immediately: aos_child_spawn()
     * maps it a second time, into the CHILD's VSpace, and this PD never
     * touches it again. ── */
    if (scratch_map(content_frame) != seL4_NoError) {
        fail("[child-spawn-parent] FAIL prep4\n");
    }
    seL4_Word blob_len = (seL4_Word)(__child_spawn_payload_end - __child_spawn_payload_start);
    if (blob_len > 4096u) {
        /* The child's image must fit in the one page aos_child_spawn()
         * maps for it. Growing tests/child-spawn/child_pd.c materially
         * risks exceeding this -- fail loudly rather than copy a
         * truncated image. */
        fail("[child-spawn-parent] FAIL prep5\n");
    }
    volatile uint8_t *dst = (volatile uint8_t *)AOS_CHILD_SPAWN_PARENT_SCRATCH_VA;
    for (seL4_Word i = 0; i < blob_len; i++) {
        dst[i] = __child_spawn_payload_start[i];
    }
    /* Bytes at [blob_len, 4096) stay zero from the fresh retype -- exactly
     * what the child's .bss, if any, needs, as long as the child's whole
     * image (code + rodata + data + bss) fits in one page.
     *
     * Publish these writes past THIS PD's own D-cache before unmapping
     * and handing the frame to aos_child_spawn(), which maps it into the
     * CHILD's VSpace: AArch64 requires explicit data-cache maintenance
     * here, exactly as kernel/agentos-root-task/src/pd_vspace.c's
     * sync_scratch_frame() documents and does for root's own identical
     * scratch-alias-then-remap pattern ("AArch64 requires explicit
     * data-cache maintenance ... x86-64 is cache coherent"). Without
     * this, the child's instruction fetch from this frame can observe
     * stale or uninitialised memory on real hardware even though the
     * writes above are complete from this PD's own point of view --
     * QEMU TCG has no cache model and will not catch a missing clean. */
    if (seL4_ARM_Page_CleanInvalidate_Data(content_frame, 0u, 4096u) != seL4_NoError) {
        fail("[child-spawn-parent] FAIL prep6\n");
    }
    AGENTOS_MEMORY_FENCE();
    seL4_ARM_Page_Unmap(content_frame);

    /* Same treatment for the gift frame: write the Probe 1 pattern the
     * child will check for, publish it past this PD's D-cache, and unmap.
     * The child reads these exact 16 bytes and answers at
     * AOS_CHILD_SPAWN_RESP_OFF. */
    if (scratch_map(gift_frame) != seL4_NoError) {
        fail("[child-spawn-parent] FAIL prep7\n");
    }
    volatile uint64_t *pattern = (volatile uint64_t *)AOS_CHILD_SPAWN_PARENT_SCRATCH_VA;
    pattern[0] = AOS_CHILD_SPAWN_PATTERN0;
    pattern[1] = AOS_CHILD_SPAWN_PATTERN1;
    if (seL4_ARM_Page_CleanInvalidate_Data(gift_frame, 0u, 4096u) != seL4_NoError) {
        fail("[child-spawn-parent] FAIL prep8\n");
    }
    AGENTOS_MEMORY_FENCE();
    seL4_ARM_Page_Unmap(gift_frame);

    /*
     * A frame capability records at most ONE mapping, and the child's
     * mapping has to persist while this PD maps the same object again to
     * read the response. So the capability that gets mapped into the
     * child's VSpace (and minted into its CNode) is a COPY; the original
     * at AOS_CHILD_SPAWN_GIFT_FRAME_SLOT stays unmapped and is what this
     * PD maps at the end. Both name the same object and seL4 treats them
     * identically -- this is a mechanical requirement, not a security
     * boundary.
     */
    if (seL4_CNode_Copy(AOS_CHILD_SPAWN_SELF_CNODE_SLOT, AOS_CHILD_SPAWN_GIFT_COPY_SLOT,
            AOS_CHILD_SPAWN_PARENT_CNODE_BITS, AOS_CHILD_SPAWN_SELF_CNODE_SLOT,
            gift_frame, AOS_CHILD_SPAWN_PARENT_CNODE_BITS, seL4_AllRights) != seL4_NoError) {
        fail("[child-spawn-parent] FAIL prep9\n");
    }

    /* ── Step 3 (PROBE 3): the doomed spawn. ────────────────────────────
     *
     * Two endowment entries. The first names the Notification this PD
     * really holds and WILL mint successfully. The second names
     * AOS_CHILD_SPAWN_ABSENT_SLOT, which is empty and always will be, but
     * the holdings declaration claims a capability is there -- so the pair
     * is internally consistent and aos_endowment_validate() accepts it.
     * The mint of entry 1 then fails inside the kernel, after entry 0 has
     * already landed in the child's CNode. That is precisely the state the
     * "no partially-endowed child ever runs" rule exists for.
     *
     * This also, deliberately, regression-tests aos_child_spawn()'s
     * caller-owned-frame unmap on failure. The doomed spawn below and the
     * real spawn further down pass the SAME content_frame capability.
     * seL4_ARM_Page_Map records the mapping ON THE FRAME CAPABILITY, so if
     * fail_teardown() ever stopped unmapping it, this frame would still
     * believe it is mapped into the VSpace the doomed spawn destroyed, and
     * the real spawn would fail at STEP_CONTENT_MAP -- Probe 1's marker
     * would vanish and make test-child-spawn would fail. Keep the two
     * spawns sharing one content_frame; it is load-bearing, not
     * incidental economy.
     */
    {
        aos_endowment_t doomed_holdings = {
            .version = AOS_ENDOWMENT_VERSION,
            .count = 2u,
            .caps = {
                { .kind = AOS_CHILD_SPAWN_KIND_NOTIFICATION, .rights = 0xFu,
                  .parent_slot = AOS_CHILD_SPAWN_PARENT_NTFN_SLOT },
                /* The lie. Nothing is in this slot. */
                { .kind = AOS_CHILD_SPAWN_KIND_NOTIFICATION, .rights = 0xFu,
                  .parent_slot = AOS_CHILD_SPAWN_ABSENT_SLOT },
            },
        };
        aos_endowment_t doomed_endow = {
            .version = AOS_ENDOWMENT_VERSION,
            .count = 2u,
            .caps = {
                { .kind = AOS_CHILD_SPAWN_KIND_NOTIFICATION, .rights = 0x1u,
                  .parent_slot = AOS_CHILD_SPAWN_PARENT_NTFN_SLOT },
                { .kind = AOS_CHILD_SPAWN_KIND_NOTIFICATION, .rights = 0x1u,
                  .parent_slot = AOS_CHILD_SPAWN_ABSENT_SLOT },
            },
        };
        seL4_CPtr doomed_cptrs[2] = {
            AOS_CHILD_SPAWN_PARENT_NTFN_SLOT,
            AOS_CHILD_SPAWN_ABSENT_SLOT,
        };
        aos_child_spawn_req_t doomed = {
            .pool_ut            = AOS_CHILD_SPAWN_POOL_SLOT,
            .self_cnode         = AOS_CHILD_SPAWN_SELF_CNODE_SLOT,
            .self_cnode_bits    = AOS_CHILD_SPAWN_PARENT_CNODE_BITS,
            .self_tcb           = AOS_CHILD_SPAWN_SELF_TCB_SLOT,
            .asid_pool          = AOS_CHILD_SPAWN_ASID_POOL_SLOT,
#ifdef CONFIG_KERNEL_MCS
            .sched_control      = AOS_CHILD_SPAWN_SCHEDCONTROL_SLOT,
#endif
            .scratch_slot       = AOS_CHILD_SPAWN_SCRATCH_BASE,
            .scratch_count      = AOS_CHILD_SPAWN_SCRATCH_COUNT,
            .content_frame      = content_frame,
            .content_va         = AOS_CHILD_SPAWN_CONTENT_VA,
            .extra_maps         = NULL,
            .extra_map_count    = 0u,
            .fault_ep           = AOS_CHILD_SPAWN_FAULT_EP_SLOT,
            .entry_point        = AOS_CHILD_SPAWN_ENTRY_VA,
            .stack_va_top       = AOS_CHILD_SPAWN_STACK_VA_TOP,
            .ipc_buf_va         = AOS_CHILD_SPAWN_IPC_BUF_VA,
            .arg0               = AOS_CHILD_SPAWN_CHILD_NTFN_SLOT,
            .arg1               = AOS_CHILD_SPAWN_CHILD_FRAME_SLOT,
            .cnode_size_bits    = AOS_CHILD_SPAWN_CHILD_CNODE_BITS,
            .priority           = AOS_CHILD_SPAWN_CHILD_PRIORITY,
            .endow              = &doomed_endow,
            .parent_holdings    = &doomed_holdings,
            .endow_cptrs        = doomed_cptrs,
            .endow_child_base_slot = AOS_CHILD_SPAWN_CHILD_NTFN_SLOT,
            .endow_badge        = AOS_CHILD_SPAWN_BADGE,
            .ledger             = &g_ledger,
            .child_index        = 99u,
            .child_name         = "doomed_child",
        };

        aos_child_spawn_result_t doomed_result;
        int rc = aos_child_spawn(&doomed, &doomed_result);
        if (rc != AOS_CHILD_SPAWN_ERR_ENDOW) {
            /* Either it succeeded (catastrophic: a child ran with an
             * endowment the kernel should have refused) or it failed
             * somewhere else (the probe is testing the wrong thing). */
            serial_log_puts(&log_channel, "[child-spawn-parent] FAIL probe3-rc\n");
            report_spawn_failure(&doomed_result);
            park();
        }
        if (doomed_result.failed_endow_index != 1u) {
            fail("[child-spawn-parent] FAIL probe3-index\n");
        }
        /* Nothing left staged: every object the call retyped was deleted,
         * including the child's CNode and therefore the entry-0 mint that
         * HAD succeeded. */
        if (doomed_result.scratch_next != doomed_result.scratch_base ||
            doomed_result.tcb != seL4_CapNull) {
            fail("[child-spawn-parent] FAIL probe3-teardown\n");
        }
        /* Nothing ran: no signal can have arrived, because the only thread
         * that could have sent one was never resumed. seL4_Poll returns
         * immediately; a non-zero badge here would mean a child the spawn
         * reported as failed had nonetheless executed. */
        seL4_Word stray = 0u;
        (void)seL4_Poll(ntfn, &stray);
        if (stray != 0u) {
            fail("[child-spawn-parent] FAIL probe3-signalled\n");
        }
        /* Nothing reported: the ledger must not describe a child that does
         * not exist, even though one of its two mints succeeded. */
        if (g_ledger.count != 0u || g_ledger.dropped != 0u) {
            fail("[child-spawn-parent] FAIL probe3-ledger\n");
        }
        serial_log_puts(&log_channel, AOS_CHILD_SPAWN_MARKER_ENDOW_FAIL);
    }

    /* ── Step 4 (PROBES 1/2/4): the real spawn. ─────────────────────────
     *
     * `holdings` is this PD's own assertion, not independently measured
     * (see child_spawn.h's doc comment on
     * aos_child_spawn_req_t.parent_holdings) -- it happens to be true here
     * because both objects really were just freshly retyped with full
     * rights, but the real guarantee that `endow` cannot exceed it comes
     * from seL4_CNode_Mint's own rights-masking against the real source
     * capability at mint time, not from this struct.
     */
    aos_endowment_t holdings = {
        .version = AOS_ENDOWMENT_VERSION,
        .count = 2u,
        .caps = {
            { .kind = AOS_CHILD_SPAWN_KIND_NOTIFICATION,
              .rights = 0xFu, /* full rights: a freshly retyped object */
              .parent_slot = AOS_CHILD_SPAWN_PARENT_NTFN_SLOT },
            { .kind = AOS_CHILD_SPAWN_KIND_FRAME,
              .rights = 0xFu, /* full rights: a freshly retyped frame */
              .parent_slot = AOS_CHILD_SPAWN_GIFT_COPY_SLOT },
        },
    };
    aos_endowment_t endow = {
        .version = AOS_ENDOWMENT_VERSION,
        .count = 2u,
        .caps = {
            { .kind = AOS_CHILD_SPAWN_KIND_NOTIFICATION,
              .rights = 0x1u, /* write/Signal only -- NOT read/Wait, so the
                                * child can tell the parent something but
                                * can never itself wait for a signal back
                                * on this same object. A strict subset of
                                * the 0xF the parent declared it holds. */
              .parent_slot = AOS_CHILD_SPAWN_PARENT_NTFN_SLOT },
            { .kind = AOS_CHILD_SPAWN_KIND_FRAME,
              .rights = 0x3u, /* read+write, NOT grant: the child may use
                                * the frame and may not pass the capability
                                * on. Again a strict subset of 0xF. */
              .parent_slot = AOS_CHILD_SPAWN_GIFT_COPY_SLOT },
        },
    };
    seL4_CPtr endow_cptrs[2] = {
        AOS_CHILD_SPAWN_PARENT_NTFN_SLOT,
        AOS_CHILD_SPAWN_GIFT_COPY_SLOT,
    };
    /* The gift frame is mapped into the child's VSpace INSIDE
     * aos_child_spawn(), before the child is startable -- endowed memory
     * is part of the endowment, so it falls inside the same "no
     * partially-endowed child ever runs" window as the mints. */
    const aos_child_spawn_map_t extra_maps[1] = {
        { .frame = AOS_CHILD_SPAWN_GIFT_COPY_SLOT, .va = AOS_CHILD_SPAWN_GIFT_VA },
    };

    aos_child_spawn_req_t req = {
        .pool_ut            = AOS_CHILD_SPAWN_POOL_SLOT,
        .self_cnode         = AOS_CHILD_SPAWN_SELF_CNODE_SLOT,
        .self_cnode_bits    = AOS_CHILD_SPAWN_PARENT_CNODE_BITS,
        .self_tcb           = AOS_CHILD_SPAWN_SELF_TCB_SLOT,
        .asid_pool          = AOS_CHILD_SPAWN_ASID_POOL_SLOT,
#ifdef CONFIG_KERNEL_MCS
        .sched_control      = AOS_CHILD_SPAWN_SCHEDCONTROL_SLOT,
#endif
        .scratch_slot       = AOS_CHILD_SPAWN_SCRATCH_BASE,
        .scratch_count      = AOS_CHILD_SPAWN_SCRATCH_COUNT,
        .content_frame      = content_frame,
        .content_va         = AOS_CHILD_SPAWN_CONTENT_VA,
        .extra_maps         = extra_maps,
        .extra_map_count    = 1u,
        /* Root minted this badged copy of its own fault endpoint for this
         * PD; it goes into the CHILD's TCB, never into the child's CSpace.
         * Without it the child's Probe 2 fault would be invisible. */
        .fault_ep           = AOS_CHILD_SPAWN_FAULT_EP_SLOT,
        .entry_point        = AOS_CHILD_SPAWN_ENTRY_VA,
        .stack_va_top       = AOS_CHILD_SPAWN_STACK_VA_TOP,
        .ipc_buf_va         = AOS_CHILD_SPAWN_IPC_BUF_VA,
        .arg0               = AOS_CHILD_SPAWN_CHILD_NTFN_SLOT,
        .arg1               = AOS_CHILD_SPAWN_CHILD_FRAME_SLOT,
        .cnode_size_bits    = AOS_CHILD_SPAWN_CHILD_CNODE_BITS,
        .priority           = AOS_CHILD_SPAWN_CHILD_PRIORITY,
        .endow              = &endow,
        .parent_holdings    = &holdings,
        .endow_cptrs        = endow_cptrs,
        .endow_child_base_slot = AOS_CHILD_SPAWN_CHILD_NTFN_SLOT,
        .endow_badge        = AOS_CHILD_SPAWN_BADGE,
        .ledger             = &g_ledger,
        .child_index        = AOS_CHILD_SPAWN_CHILD_INDEX,
        .child_name         = AOS_CHILD_SPAWN_CHILD_NAME,
    };

    aos_child_spawn_result_t result;
    if (aos_child_spawn(&req, &result) != AOS_CHILD_SPAWN_OK) {
        serial_log_puts(&log_channel, AOS_CHILD_SPAWN_MARKER_FAIL_SPAWN);
        report_spawn_failure(&result);
        park();
    }

    /* The child is now running (its TCB was resumed inside
     * aos_child_spawn only after every endowment succeeded). Wait for it
     * to use the Signal-only derivative it was given. */
    serial_log_puts(&log_channel, "[child-spawn-parent] spawned ok, waiting\n");
    seL4_Word badge = 0u;
    seL4_Wait(ntfn, &badge);
    if (badge != AOS_CHILD_SPAWN_BADGE) {
        fail("[child-spawn-parent] FAIL probe1-badge\n");
    }

    /* ── PROBE 1: verify what the child actually did. ───────────────────
     *
     * Map this PD's OWN (still unmapped) capability to the gift frame and
     * read the child's response. seL4_ARM_Page_Invalidate_Data discards
     * any stale lines this PD might hold for the page before the read --
     * this PD cleaned its own writes out before unmapping, so nothing is
     * lost; QEMU TCG models no cache, so this matters on real hardware
     * only, which is exactly when it would matter most.
     */
    if (scratch_map(gift_frame) != seL4_NoError) {
        fail("[child-spawn-parent] FAIL probe1-map\n");
    }
    if (seL4_ARM_Page_Invalidate_Data(gift_frame, 0u, 4096u) != seL4_NoError) {
        fail("[child-spawn-parent] FAIL probe1-inval\n");
    }
    AGENTOS_MEMORY_FENCE();

    seL4_ARM_Page_GetAddress_t parent_pa = seL4_ARM_Page_GetAddress(gift_frame);
    if (parent_pa.error != seL4_NoError) {
        fail("[child-spawn-parent] FAIL probe1-paddr\n");
    }
    volatile const uint64_t *resp = (volatile const uint64_t *)
        (AOS_CHILD_SPAWN_PARENT_SCRATCH_VA + AOS_CHILD_SPAWN_RESP_OFF);
    if (resp[0] != AOS_CHILD_SPAWN_RESP_MAGIC) {
        fail("[child-spawn-parent] FAIL probe1-magic\n");
    }
    /* The decisive check: the physical address the CHILD's own endowed
     * frame CAPABILITY reported is the physical address THIS PD's
     * capability to the same object reports. The child did not merely
     * write to some page it could see -- it invoked the capability it was
     * endowed with, and that capability names this exact frame. */
    if (resp[1] != (uint64_t)parent_pa.paddr) {
        fail("[child-spawn-parent] FAIL probe1-paddr-mismatch\n");
    }
    if (resp[2] != ~(uint64_t)AOS_CHILD_SPAWN_PATTERN0 ||
        resp[3] != ~(uint64_t)AOS_CHILD_SPAWN_PATTERN1) {
        fail("[child-spawn-parent] FAIL probe1-pattern\n");
    }
    serial_log_puts(&log_channel, AOS_CHILD_SPAWN_MARKER_OK);

    /* ── PROBE 4: the endowment-delta ledger. ───────────────────────────
     *
     * aos_child_spawn() appended one entry per successful mint and rolled
     * back the doomed spawn's entries in full. Fold them into a T4
     * authority snapshot and print it with T4's own formatter: the
     * delegating domain reporting its runtime delegation is exactly the
     * gap platform/authority.h names ("runtime delegation must be
     * separately reported by the delegating domain"), so this goes
     * through that path rather than inventing a second one.
     *
     * This is a REPORT, NOT A PROOF. seL4 exposes no capability-
     * enumeration syscall, so nothing here verifies that the child holds
     * what this says it holds, or that it holds nothing else. The
     * subsetting invariant is enforced by the kernel, unconditionally and
     * independently of this record: seL4_CNode_Mint masks requested rights
     * against the real source capability, and a domain cannot mint from a
     * capability it does not possess.
     */
    aos_authority_init(&g_snapshot);
    if (aos_endow_ledger_merge(&g_ledger, &g_snapshot) != AOS_ENDOW_LEDGER_OK) {
        fail("[child-spawn-parent] FAIL probe4-merge\n");
    }
    if (g_snapshot.pd_count != 1u ||
        g_snapshot.pds[0].pd_index != AOS_CHILD_SPAWN_CHILD_INDEX ||
        g_snapshot.pds[0].counts[AOS_AUTHORITY_KIND_NOTIFICATION] != 1u ||
        g_snapshot.pds[0].counts[AOS_AUTHORITY_KIND_FRAME] != 1u ||
        g_snapshot.total_recorded != 2u ||
        g_snapshot.truncated_adds != 0u ||
        g_snapshot.saturated != 0u) {
        fail("[child-spawn-parent] FAIL probe4-counts\n");
    }
    if (aos_authority_format(&g_snapshot, g_report, sizeof(g_report)) <= 0) {
        fail("[child-spawn-parent] FAIL probe4-format\n");
    }
    serial_log_puts(&log_channel, AOS_CHILD_SPAWN_MARKER_LEDGER_BEGIN);
    serial_log_puts(&log_channel, g_report);
    serial_log_puts(&log_channel, AOS_CHILD_SPAWN_MARKER_LEDGER_OK);

    park();
}
