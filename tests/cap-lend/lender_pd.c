/*
 * lender_pd.c — cap-lend demonstration pair: lender side (test image only).
 *
 * Built only under AGENTOS_CAP_LEND_TEST (see system_desc_aarch64.c and the
 * root-task Makefile); absent from the default PD set. Exercises
 * libs/pd-support/cap_lend.c end-to-end against a real seL4 target:
 *
 *   1. Map the 4K frame the root task retyped directly into this PD's own
 *      CNode at boot (AOS_CAP_LEND_FRAME_SLOT, full rights -- this PD's own
 *      object, not shared with anyone else) and write a known byte pattern.
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

#include "boot_info.h" /* seL4_ARCH_Page_Map */
#include "cap_lend.h"
#include "contracts/cap_lend_test.h"
#include "sel4_ipc.h"

static void park(seL4_CPtr ep)
{
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

    /* Map this PD's own frame (full rights: this PD owns it outright) and
     * write a byte pattern a borrower can verify it reads back exactly. */
    if (seL4_ARCH_Page_Map(AOS_CAP_LEND_FRAME_SLOT, AOS_CAP_LEND_SELF_VSPACE_SLOT,
            AOS_CAP_LEND_FRAME_VA, seL4_ReadWrite,
            seL4_ARM_Default_VMAttributes) != seL4_NoError) {
        park(xfer_ep);
    }
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
     */
    seL4_CapRights_t lend_rights = seL4_CapRights_new(
        0 /* grantreply */, 0 /* grant */, 1 /* read */, 0 /* write */);
    int lend_err = aos_cap_lend(AOS_CAP_LEND_FRAME_SLOT,
                                 AOS_CAP_LEND_SELF_CNODE_SLOT,
                                 AOS_CAP_LEND_DERIVED_SLOT,
                                 AOS_CAP_LEND_CNODE_BITS,
                                 lend_rights, AOS_CAP_LEND_BADGE);
    if (lend_err != AOS_CAP_LEND_OK) {
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

    /* Loan transferred; this PD's part of the Task 2 demonstration is
     * done. Revocation is exercised by Task 3's externally-driven proof,
     * which can call aos_cap_lend_revoke(AOS_CAP_LEND_FRAME_SLOT) through
     * whatever control channel that proof wires up. Park rather than spin. */
    park(xfer_ep);
}
