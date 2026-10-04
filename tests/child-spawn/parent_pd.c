/*
 * parent_pd.c — child-spawn demonstration pair: parent side (test image
 * only).
 *
 * Built only under AGENTOS_CHILD_SPAWN_TEST (see system_desc_aarch64.c and
 * the root-task Makefile); absent from the default PD set. Exercises
 * libs/pd-support/child_spawn.c end-to-end against a real seL4 target:
 *
 *   1. Retype a Notification object from the pool root granted this PD at
 *      boot (AOS_CHILD_SPAWN_POOL_SLOT) -- this is the ONE capability this
 *      demo endows, kept deliberately small so the subsetting check has
 *      exactly one thing to verify.
 *   2. Retype one frame from the same pool, temporarily map it into THIS
 *      PD's own VSpace, copy the child's image bytes in (already part of
 *      this PD's own verified ELF -- see __child_spawn_payload_start/end
 *      below), and unmap it. This is the only place in the whole demo
 *      that reads or writes the child's image bytes; aos_child_spawn()
 *      itself never does.
 *   3. Declare parent_holdings (what this PD actually holds: the fresh
 *      Notification, full rights) and endow (a Signal-only derivative of
 *      it), and call aos_child_spawn().
 *   4. Wait on its own copy of the Notification for the child to signal
 *      back -- proof the child is a real, independently scheduled thread
 *      that used exactly the one capability it was given.
 */
#include <sel4/sel4.h>

#include "child_spawn.h"
#include "contracts/child_spawn_contract.h"
#include "sel4_ipc.h"
#include "serial_log.h"

static serial_log_t log_channel = {.ep = PD_CNODE_SLOT_SERIAL_EP};

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

void pd_main(seL4_CPtr endpoint, seL4_CPtr nameserver)
{
    (void)endpoint;
    (void)nameserver;

    /* ── Step 2 (prep): retype this PD's one notification -- the sole
     * capability it endows -- and the content frame, from its pool. ── */
    seL4_CPtr ntfn = AOS_CHILD_SPAWN_PARENT_NTFN_SLOT;
    if (seL4_Untyped_Retype(AOS_CHILD_SPAWN_POOL_SLOT, seL4_NotificationObject, 0u,
            AOS_CHILD_SPAWN_SELF_CNODE_SLOT, 0u, 0u, ntfn, 1u) != seL4_NoError) {
        serial_log_puts(&log_channel, "[child-spawn-parent] FAIL prep1\n");
        park();
    }

    seL4_CPtr content_frame = AOS_CHILD_SPAWN_CONTENT_FRAME_SLOT;
    if (seL4_Untyped_Retype(AOS_CHILD_SPAWN_POOL_SLOT, seL4_ARM_SmallPageObject, 0u,
            AOS_CHILD_SPAWN_SELF_CNODE_SLOT, 0u, 0u, content_frame, 1u) != seL4_NoError) {
        serial_log_puts(&log_channel, "[child-spawn-parent] FAIL prep2\n");
        park();
    }

    /* Copy the child's image into the fresh (zero-filled by retype) frame
     * via a transient mapping into THIS PD's own VSpace. The frame is
     * unmapped again immediately: aos_child_spawn() maps it a second time,
     * into the CHILD's VSpace, and this PD never touches it again.
     *
     * AOS_CHILD_SPAWN_PARENT_SCRATCH_VA is not part of any region root's
     * normal PD construction mapped (ELF image, stack, IPC buffer), so it
     * has no page table yet -- retype one on seL4_FailedLookup, same
     * bounded retry pattern child_spawn.c's map_frame_retrying uses, using
     * dedicated scratch slots outside aos_child_spawn()'s own range. */
    {
        int mapped = 0;
        for (unsigned attempt = 0; attempt < AOS_CHILD_SPAWN_MAX_PT_LEVELS; attempt++) {
            seL4_Error err = seL4_ARM_Page_Map(content_frame, AOS_CHILD_SPAWN_SELF_VSPACE_SLOT,
                AOS_CHILD_SPAWN_PARENT_SCRATCH_VA, seL4_AllRights,
                seL4_ARM_Default_VMAttributes);
            if (err == seL4_NoError) {
                mapped = 1;
                break;
            }
            if (err != seL4_FailedLookup || attempt >= 4u) {
                break;
            }
            seL4_CPtr pt = AOS_CHILD_SPAWN_PARENT_SCRATCH_PT_BASE + attempt;
            if (seL4_Untyped_Retype(AOS_CHILD_SPAWN_POOL_SLOT, seL4_ARM_PageTableObject, 0u,
                    AOS_CHILD_SPAWN_SELF_CNODE_SLOT, 0u, 0u, pt, 1u) != seL4_NoError) {
                break;
            }
            if (seL4_ARM_PageTable_Map(pt, AOS_CHILD_SPAWN_SELF_VSPACE_SLOT,
                    AOS_CHILD_SPAWN_PARENT_SCRATCH_VA, seL4_ARM_Default_VMAttributes) != seL4_NoError) {
                break;
            }
        }
        if (!mapped) {
            serial_log_puts(&log_channel, "[child-spawn-parent] FAIL prep3\n");
            park();
        }
    }
    seL4_Word blob_len = (seL4_Word)(__child_spawn_payload_end - __child_spawn_payload_start);
    if (blob_len > 4096u) {
        /* The child's image must fit in the one page aos_child_spawn()
         * maps for it. Growing tests/child-spawn/child_pd.c materially
         * risks exceeding this -- fail loudly rather than copy a
         * truncated image. */
        serial_log_puts(&log_channel, "[child-spawn-parent] FAIL prep4\n");
        park();
    }
    volatile uint8_t *dst = (volatile uint8_t *)AOS_CHILD_SPAWN_PARENT_SCRATCH_VA;
    for (seL4_Word i = 0; i < blob_len; i++) {
        dst[i] = __child_spawn_payload_start[i];
    }
    /* Bytes at [blob_len, 4096) stay zero from the fresh retype -- exactly
     * what the child's .bss, if any, needs, as long as the child's whole
     * image (code + rodata + data + bss) fits in one page. */
    seL4_ARM_Page_Unmap(content_frame);

    /* ── Step 3: declare what this PD actually holds, and what it will
     * endow. Exactly one capability, so Task 1's subsetting check has
     * exactly one entry to verify on each side. ── */
    aos_endowment_t holdings = {
        .version = AOS_ENDOWMENT_VERSION,
        .count = 1u,
        .caps = {
            { .kind = AOS_CHILD_SPAWN_KIND_NOTIFICATION,
              .rights = 0xFu, /* full rights: a freshly retyped object */
              .parent_slot = AOS_CHILD_SPAWN_PARENT_NTFN_SLOT },
        },
    };
    aos_endowment_t endow = {
        .version = AOS_ENDOWMENT_VERSION,
        .count = 1u,
        .caps = {
            { .kind = AOS_CHILD_SPAWN_KIND_NOTIFICATION,
              .rights = 0x1u, /* write/Signal only -- NOT read/Wait, so the
                                * child can tell the parent something but
                                * can never itself wait for a signal back
                                * on this same object. A strict subset of
                                * the 0xF the parent declared it holds. */
              .parent_slot = AOS_CHILD_SPAWN_PARENT_NTFN_SLOT },
        },
    };
    seL4_CPtr endow_cptrs[1] = { AOS_CHILD_SPAWN_PARENT_NTFN_SLOT };

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
        .entry_point        = AOS_CHILD_SPAWN_ENTRY_VA,
        .stack_va_top       = AOS_CHILD_SPAWN_STACK_VA_TOP,
        .ipc_buf_va         = AOS_CHILD_SPAWN_IPC_BUF_VA,
        .arg0               = AOS_CHILD_SPAWN_CHILD_NTFN_SLOT,
        .arg1               = 0u,
        .cnode_size_bits    = AOS_CHILD_SPAWN_CHILD_CNODE_BITS,
        .priority           = AOS_CHILD_SPAWN_CHILD_PRIORITY,
        .endow              = &endow,
        .parent_holdings    = &holdings,
        .endow_cptrs        = endow_cptrs,
        .endow_child_base_slot = AOS_CHILD_SPAWN_CHILD_NTFN_SLOT,
        .endow_badge        = AOS_CHILD_SPAWN_BADGE,
    };

    aos_child_spawn_result_t result;
    if (aos_child_spawn(&req, &result) != AOS_CHILD_SPAWN_OK) {
        /*
         * Report which step failed (Step 4 of the brief: "report which
         * step failed" on exhaustion/failure, not just a bare failure).
         * result.error/.failed_step are debugging breadcrumbs, not a
         * stable ABI (see child_spawn.h) -- encoded as decimal digits in
         * ONE serial_log_puts call so there is exactly one seL4_Call to
         * serial_pd on this path: "D<err>-<step>-<endow index>".
         */
        char dbuf[16];
        seL4_Word e = (seL4_Word)(-result.error);
        seL4_Word s = result.failed_step;
        seL4_Word fi = result.failed_endow_index;
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
        park();
    }

    /* ── Step 4: the child is now running (its TCB was resumed inside
     * aos_child_spawn only after its one endowment succeeded). Wait for
     * it to use the Signal-only derivative it was given. ── */
    serial_log_puts(&log_channel, "[child-spawn-parent] spawned ok, waiting\n");
    seL4_Word badge = 0u;
    seL4_Wait(ntfn, &badge);

    serial_log_puts(&log_channel, AOS_CHILD_SPAWN_MARKER_OK);
    park();
}
