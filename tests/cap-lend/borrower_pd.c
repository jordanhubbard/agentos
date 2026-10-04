/*
 * borrower_pd.c — cap-lend demonstration pair: borrower side (test image
 * only). See lender_pd.c for the full picture; this PD receives the
 * rights-reduced derivative cap_lend_lender mints and transfers, maps it,
 * and reads back the byte pattern the lender wrote -- proof the loan was
 * actually usable, not merely transferred (a loan that was never used
 * would make any later revocation proof vacuous; see Task 3).
 *
 * Task 3's target proof continues past that point, in this file: this PD
 * mints its own sub-delegated copy of the received derivative (Probe 3
 * setup) and proves THAT copy is alive too, then signals the lender over
 * AOS_CAP_LEND_DONE_NTFN_SLOT that it is done using the loan, then waits
 * on AOS_CAP_LEND_REVOKED_NTFN_SLOT for the lender's revoke to actually
 * happen before checking that both the sub-delegated copy (Probe 3) and
 * the direct loan (Probe 2) are now dead. See lender_pd.c and
 * cap_lend_test.h for the full handshake design.
 *
 * Built only under AGENTOS_CAP_LEND_TEST; absent from the default PD set.
 */
#include <sel4/sel4.h>

#include "boot_info.h" /* seL4_ARCH_Page_Map */
#include "cap_lend.h"
#include "contracts/cap_lend_test.h"
#include "sel4_ipc.h"
#include "serial_log.h"

/* Same channel native_rust_client uses: an EP to SVC_ID_SERIAL plus the
 * serial-contract shared page, both granted by main.c's
 * AGENTOS_CAP_LEND_TEST provisioning (system_desc_aarch64.c init_eps +
 * the serial-transfer-page name_eq list). */
static serial_log_t log_channel = {.ep = PD_CNODE_SLOT_SERIAL_EP};

/*
 * Park on `ep` forever. Clears the receive path to seL4_CapNull first --
 * NOT optional once a receive path has ever been set in this PD's
 * lifetime (see pd_main below): leaving it pointed at
 * AOS_CAP_LEND_BORROWER_RECV_SLOT, now occupied by whatever this PD last
 * received, means every later seL4_Recv on that same path would silently
 * fail to deliver its incoming capability -- the exact occupied-receive-
 * slot trap the net_virt.c:567-570 Delete-before-SetCapReceivePath
 * discipline exists to avoid, except here applied to the park loop
 * itself. Harmless today because nothing else sends a capability to this
 * endpoint, but Task 3 re-loans over the same endpoint, so this stops
 * being hypothetical in the very next task.
 */
static void park(seL4_CPtr ep)
{
    seL4_SetCapReceivePath(seL4_CapNull, 0u, 0u);
    for (;;) {
        seL4_Word badge = 0u;
#ifdef CONFIG_KERNEL_MCS
        (void)seL4_Recv(ep, &badge, AGENTOS_IPC_REPLY_CAP);
#else
        (void)seL4_Recv(ep, &badge);
#endif
    }
}

void pd_main(seL4_CPtr endpoint, seL4_CPtr nameserver)
{
    (void)endpoint;
    (void)nameserver;

    const seL4_CPtr xfer_ep = AOS_CAP_LEND_XFER_EP_SLOT;

    /*
     * Prepare the receive slot before calling Recv: delete whatever
     * (nothing, on first use) occupies AOS_CAP_LEND_BORROWER_RECV_SLOT,
     * then set it as the receive path. This is not optional --
     * seL4_SetCapReceivePath into an OCCUPIED slot silently fails to
     * deliver the incoming capability, leaving this PD to proceed with
     * whatever was already there. Follows net_virt.c:567-570 exactly.
     */
    (void)seL4_CNode_Delete(AOS_CAP_LEND_SELF_CNODE_SLOT,
                             AOS_CAP_LEND_BORROWER_RECV_SLOT,
                             AOS_CAP_LEND_CNODE_BITS);
    seL4_SetCapReceivePath(AOS_CAP_LEND_SELF_CNODE_SLOT,
                            AOS_CAP_LEND_BORROWER_RECV_SLOT,
                            AOS_CAP_LEND_CNODE_BITS);

    seL4_Word badge = 0u;
#ifdef CONFIG_KERNEL_MCS
    seL4_MessageInfo_t info = seL4_Recv(xfer_ep, &badge, AGENTOS_IPC_REPLY_CAP);
#else
    seL4_MessageInfo_t info = seL4_Recv(xfer_ep, &badge);
#endif
    if (seL4_MessageInfo_get_extraCaps(info) != 1u) {
        serial_log_puts(&log_channel, AOS_CAP_LEND_MARKER_BORROWER_FAIL_RECV);
        park(xfer_ep);
    }

    /* Map the received derivative READ-ONLY: it was minted with only
     * capAllowRead set, so a writable mapping request would simply fail
     * at the kernel -- but ask for exactly what was granted regardless,
     * since this borrower has no business assuming it has more. */
    if (seL4_ARCH_Page_Map(AOS_CAP_LEND_BORROWER_RECV_SLOT,
            AOS_CAP_LEND_SELF_VSPACE_SLOT, AOS_CAP_LEND_FRAME_VA,
            seL4_CanRead, seL4_ARM_Default_VMAttributes) != seL4_NoError) {
        serial_log_puts(&log_channel, AOS_CAP_LEND_MARKER_BORROWER_FAIL_MAP);
        park(xfer_ep);
    }

    volatile const uint8_t *frame =
        (volatile const uint8_t *)(uintptr_t)AOS_CAP_LEND_FRAME_VA;
    uint8_t sum_ok = 1u;
    for (unsigned i = 0; i < 4096u; i++) {
        if (frame[i] != (uint8_t)(AOS_CAP_LEND_PATTERN_BYTE + i)) {
            sum_ok = 0u;
            break;
        }
    }

    if (!sum_ok) {
        /* Probe 1 failed: the loan never actually worked. Stop here --
         * everything past this point (sub-delegation, revoke, fault)
         * would be meaningless without a working loan behind it. */
        serial_log_puts(&log_channel, AOS_CAP_LEND_MARKER_BORROWER_FAIL_VERIFY);
        park(xfer_ep);
    }
    serial_log_puts(&log_channel, AOS_CAP_LEND_MARKER_BORROWER_OK);

    /*
     * Probe 3 setup: mint our OWN copy of the received derivative into a
     * second slot via the same aos_cap_lend() the lender used -- any
     * holder of a capability, including a borrowed one, can lend it
     * further. `rights` is read-only, already a strict subset of
     * seL4_AllRights (aos_cap_lend's subsetting check is against
     * seL4_AllRights, not against what we currently hold, but we could
     * not request more than read even if we tried -- seL4_CNode_Mint masks
     * requested rights against the source capability's rights).
     */
    seL4_CapRights_t subdelegate_rights = seL4_CapRights_new(0, 0, 1, 0);
    int subdelegate_err = aos_cap_lend(AOS_CAP_LEND_SELF_CNODE_SLOT /* src_root */,
                                        AOS_CAP_LEND_BORROWER_RECV_SLOT /* original */,
                                        AOS_CAP_LEND_CNODE_BITS /* src_depth */,
                                        AOS_CAP_LEND_SELF_CNODE_SLOT /* dest_cnode */,
                                        AOS_CAP_LEND_SUBDELEGATE_SLOT /* dest_slot */,
                                        AOS_CAP_LEND_CNODE_BITS /* dest_depth */,
                                        subdelegate_rights, AOS_CAP_LEND_SUBDELEGATE_BADGE);
    if (subdelegate_err != AOS_CAP_LEND_OK) {
        serial_log_puts(&log_channel, AOS_CAP_LEND_MARKER_BORROWER_FAIL_SUBDELEGATE);
        park(xfer_ep);
    }

    /*
     * Prove the sub-delegated copy is alive BEFORE revocation -- mirrors
     * Probe 1's "prove it worked first" discipline, so "the sub-delegate
     * is dead" means something once we check again after revoke. A cheap,
     * kernel-enforced liveness check: copy it to a scratch slot, then
     * immediately delete the scratch copy again (leaving only the one
     * real sub-delegated copy at AOS_CAP_LEND_SUBDELEGATE_SLOT, which is
     * what the post-revoke check below re-targets).
     */
    if (seL4_CNode_Copy(AOS_CAP_LEND_SELF_CNODE_SLOT, AOS_CAP_LEND_SUBDELEGATE_PROBE_SLOT,
            AOS_CAP_LEND_CNODE_BITS, AOS_CAP_LEND_SELF_CNODE_SLOT, AOS_CAP_LEND_SUBDELEGATE_SLOT,
            AOS_CAP_LEND_CNODE_BITS, subdelegate_rights) != seL4_NoError) {
        serial_log_puts(&log_channel, AOS_CAP_LEND_MARKER_BORROWER_FAIL_SUBDELEGATE_PRECHECK);
        park(xfer_ep);
    }
    (void)seL4_CNode_Delete(AOS_CAP_LEND_SELF_CNODE_SLOT, AOS_CAP_LEND_SUBDELEGATE_PROBE_SLOT,
                             AOS_CAP_LEND_CNODE_BITS);
    serial_log_puts(&log_channel, AOS_CAP_LEND_MARKER_BORROWER_SUBDELEGATE_OK);

    /*
     * Tell the lender we're done using the loan (Probe 1) and have
     * confirmed our sub-delegated copy is alive (Probe 3's precondition).
     * The lender must not revoke before this signal, or the evidence above
     * would be racing the revoke instead of strictly preceding it.
     */
    seL4_Signal(AOS_CAP_LEND_DONE_NTFN_SLOT);

    /*
     * Wait for the lender's revoke to actually have happened before
     * touching either capability again -- without this, "the borrower
     * faults" (or doesn't) would depend on scheduling luck instead of on
     * revocation.
     */
    seL4_Word revoked_badge = 0u;
    seL4_Wait(AOS_CAP_LEND_REVOKED_NTFN_SLOT, &revoked_badge);

    /*
     * Probe 3: the sub-delegated copy must be dead now too -- re-run the
     * EXACT same kernel-enforced liveness check as the precondition above.
     * This time it must FAIL (seL4_CNode_Copy of an already-revoked
     * capability returns an error), proving revocation reached the
     * sub-delegation and not just the direct loan. If it unexpectedly
     * succeeds, clean up the stray copy it produced and report the miss --
     * but deliberately do NOT park() here: Step 4's non-vacuity run
     * (aos_cap_lend_revoke neutered) needs Probe 2's STILL_READABLE
     * evidence too, in the SAME boot, and that check is below. Parking
     * here would hide Probe 2's non-vacuity evidence behind Probe 3's.
     */
    if (seL4_CNode_Copy(AOS_CAP_LEND_SELF_CNODE_SLOT, AOS_CAP_LEND_SUBDELEGATE_PROBE_SLOT,
            AOS_CAP_LEND_CNODE_BITS, AOS_CAP_LEND_SELF_CNODE_SLOT, AOS_CAP_LEND_SUBDELEGATE_SLOT,
            AOS_CAP_LEND_CNODE_BITS, subdelegate_rights) == seL4_NoError) {
        (void)seL4_CNode_Delete(AOS_CAP_LEND_SELF_CNODE_SLOT, AOS_CAP_LEND_SUBDELEGATE_PROBE_SLOT,
                                 AOS_CAP_LEND_CNODE_BITS);
        serial_log_puts(&log_channel, AOS_CAP_LEND_MARKER_BORROWER_FAIL_SUBDELEGATE_ALIVE);
    } else {
        serial_log_puts(&log_channel, AOS_CAP_LEND_MARKER_BORROWER_SUBDELEGATE_DEAD_OK);
    }

    /*
     * Probe 2: our directly-received capability's mapping must be dead
     * too. seL4_CNode_Revoke on the lender's original unmaps any mapped
     * descendant as part of deleting it, so this read -- the exact same
     * verification loop as Probe 1, deliberately re-run -- is expected to
     * raise a VMFault on its very first byte, which the root task's fault
     * oracle (ROOT_PROBE_* in main.c, AGENTOS_CAP_LEND_TEST branch) matches
     * on exact badge/address/direction and reports as
     * AOS_CAP_LEND_MARKER_ROOT_FAULT_VERIFIED. This PD does not return from
     * that fault. The loop below only runs to completion, and the WARN
     * marker only fires, if revocation did NOT actually withdraw the
     * mapping -- see Step 4 of the Task 3 plan, which neuters
     * aos_cap_lend_revoke() on purpose to confirm this path is reachable.
     */
    uint8_t still_ok = 1u;
    for (unsigned i = 0; i < 4096u; i++) {
        if (frame[i] != (uint8_t)(AOS_CAP_LEND_PATTERN_BYTE + i)) {
            still_ok = 0u;
            break;
        }
    }
    if (still_ok) {
        serial_log_puts(&log_channel, AOS_CAP_LEND_MARKER_BORROWER_STILL_READABLE);
    }

    park(xfer_ep);
}
