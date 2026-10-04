/*
 * lender_pd.c — cap-lend demonstration pair: lender side (test image only).
 *
 * Built only under AGENTOS_CAP_LEND_TEST (see system_desc_aarch64.c and the
 * root-task Makefile); absent from the default PD set. Exercises
 * libs/pd-support/cap_lend.c end-to-end against a real seL4 target:
 *
 *   1. Write a known byte pattern into the 4K frame the root task retyped,
 *      mapped (at AOS_CAP_LEND_FRAME_VA -- see main.c's AGENTOS_CAP_LEND_TEST
 *      provisioning block) and moved into this PD's own CNode at boot
 *      (AOS_CAP_LEND_FRAME_SLOT, full rights -- this PD's own object, not
 *      shared with anyone else, and already mapped: a PD thread holds no
 *      Untyped capability and so cannot install the page-table objects a
 *      fresh seL4_ARCH_Page_Map of its own would need; only the root task
 *      can, which is why it maps before moving the cap in).
 *   2. aos_cap_lend() the frame: mint a badged, READ-ONLY derivative (no
 *      write, no grant -- a strict subset of the full rights this PD holds)
 *      into this PD's own CNode, ready for transfer.
 *   3. Transfer that derivative to cap_lend_borrower by IPC capability
 *      transfer over the shared AOS_CAP_LEND_XFER_EP endpoint.
 *
 * This file provisions and exercises the mint + transfer steps (Task 2).
 * The synchronized revoke-then-fault proof, and the sub-delegation check,
 * are Task 3's target proof and are driven externally (not from this PD's
 * own unsupervised boot-time control flow, which has no way to learn when
 * the borrower has finished using its loan).
 */
#include <sel4/sel4.h>

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
 * Park on `ep` forever. This PD never sets a receive path (it only sends),
 * but clears it to seL4_CapNull defensively anyway -- see the matching
 * comment in borrower_pd.c's park() for why an occupied receive path left
 * set across a park loop is exactly the silent-failure shape this track
 * keeps warning about, and Task 3 re-loans over this same endpoint.
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

    /* The frame at AOS_CAP_LEND_FRAME_SLOT arrives already mapped at
     * AOS_CAP_LEND_FRAME_VA (root task mapped it before moving the cap in
     * -- see the file header). Write a byte pattern a borrower can verify
     * it reads back exactly, proving the loan was actually used. */
    volatile uint8_t *frame = (volatile uint8_t *)(uintptr_t)AOS_CAP_LEND_FRAME_VA;
    for (unsigned i = 0; i < 4096u; i++) {
        frame[i] = (uint8_t)(AOS_CAP_LEND_PATTERN_BYTE + i);
    }

    /*
     * Mint a READ-ONLY derivative: capAllowRead=1, capAllowWrite=0,
     * capAllowGrant=0, capAllowGrantReply=0 -- a strict subset of the
     * seL4_AllRights this PD holds on its own frame. aos_cap_lend() itself
     * asserts the subsetting invariant; this call additionally documents,
     * at the call site, exactly which right is being dropped: write and
     * grant/grantreply are not lent, only read.
     *
     * src_root == dest_cnode == AOS_CAP_LEND_SELF_CNODE_SLOT here because
     * this lender mints from, and into, its own CNode -- but aos_cap_lend()
     * treats them as independent roots; passing the same self-reference
     * capability for both is a choice this call makes, not something the
     * library assumes.
     */
    seL4_CapRights_t lend_rights = seL4_CapRights_new(
        0 /* grantreply */, 0 /* grant */, 1 /* read */, 0 /* write */);
    int lend_err = aos_cap_lend(AOS_CAP_LEND_SELF_CNODE_SLOT /* src_root */,
                                 AOS_CAP_LEND_FRAME_SLOT /* original */,
                                 AOS_CAP_LEND_CNODE_BITS /* src_depth */,
                                 AOS_CAP_LEND_SELF_CNODE_SLOT /* dest_cnode */,
                                 AOS_CAP_LEND_DERIVED_SLOT /* dest_slot */,
                                 AOS_CAP_LEND_CNODE_BITS /* dest_depth */,
                                 lend_rights, AOS_CAP_LEND_BADGE);
    if (lend_err != AOS_CAP_LEND_OK) {
        serial_log_puts(&log_channel, AOS_CAP_LEND_MARKER_LENDER_FAIL_LEND);
        park(xfer_ep);
    }

    /*
     * Transfer the derivative to cap_lend_borrower over the shared
     * endpoint. seL4_Send on an endpoint with no matching seL4_Recv yet
     * blocks until the borrower calls Recv -- the rendezvous, and the cap
     * transfer into the borrower's receive slot, complete atomically
     * before this Send returns, so the borrower is guaranteed to already
     * hold the derivative once control returns here.
     */
    seL4_SetCap(0, AOS_CAP_LEND_DERIVED_SLOT);
    seL4_MessageInfo_t xfer_msg = seL4_MessageInfo_new(0u, 0u, 1u, 0u);
    seL4_Send(xfer_ep, xfer_msg);
    seL4_SetCap(0, seL4_CapNull);

    serial_log_puts(&log_channel, AOS_CAP_LEND_MARKER_LENDER_OK);

    /* Loan transferred; this PD's part of the Task 2 demonstration is
     * done. Revocation is exercised by Task 3's externally-driven proof,
     * which can call aos_cap_lend_revoke(AOS_CAP_LEND_FRAME_SLOT) through
     * whatever control channel that proof wires up. Park rather than spin. */
    park(xfer_ep);
}
