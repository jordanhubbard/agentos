/*
 * borrower_pd.c — cap-lend demonstration pair: borrower side (test image
 * only). See lender_pd.c for the full picture; this PD receives the
 * rights-reduced derivative cap_lend_lender mints and transfers, maps it,
 * and reads back the byte pattern the lender wrote -- proof the loan was
 * actually usable, not merely transferred (a loan that was never used
 * would make any later revocation proof vacuous; see Task 3).
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

    if (sum_ok) {
        serial_log_puts(&log_channel, AOS_CAP_LEND_MARKER_BORROWER_OK);
    } else {
        serial_log_puts(&log_channel, AOS_CAP_LEND_MARKER_BORROWER_FAIL_VERIFY);
    }

    park(xfer_ep);
}
