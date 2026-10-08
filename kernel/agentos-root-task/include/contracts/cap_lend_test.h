/*
 * cap_lend_test.h — slot/badge layout for the cap-lend demonstration pair.
 *
 * Test-image-only. Built only when AGENTOS_CAP_LEND_TEST is defined (see
 * kernel/agentos-root-task/Makefile and system_desc_aarch64.c); absent from
 * the default PD set and from AGENTOS_SEL4_TEST_IMAGE /
 * AGENTOS_NATIVE_RUST_TEST / AGENTOS_FRAMEBUFFER_TEST, which are separate
 * image variants (see the Makefile mutual-exclusion checks).
 *
 * Two PDs, cap_lend_lender and cap_lend_borrower, exercise libs/pd-support/
 * cap_lend.c end-to-end: the lender mints a rights-reduced, badged
 * derivative of a frame it owns and transfers it to the borrower over
 * AOS_CAP_LEND_XFER_EP by IPC capability transfer, following the
 * delete-receive-slot-before-SetCapReceivePath pattern from
 * net_virt.c:567-570. Task 3 (see docs/superpowers/plans/
 * 2026-10-04-t5-capability-lending.md) builds the target proof -- borrower
 * use succeeding, revocation faulting it, and a sub-delegated copy dying
 * too -- on this pair.
 *
 * Slot numbers below are chosen in the range above the well-known
 * PD_CNODE_SLOT_* assignments (system_desc.h, which top out at 42) and
 * below PD_IRQHANDLER_SLOT_BASE (64); neither PD in this pair uses IRQs or
 * the standard service slots, so there is no collision.
 */
#pragma once

/* Shared IPC endpoint the lender sends the derivative over and the
 * borrower receives it on. Slot within each PD's own CNode. */
#define AOS_CAP_LEND_XFER_EP_SLOT        45u

/* Self-reference capability to the lender/borrower's own CNode, copied in
 * by the root task at boot (same pattern as AOS_QUEUE_SERVICE_CNODE /
 * AOS_GUEST_RAM_SELF_CNODE elsewhere in this tree). Required as the root
 * argument to seL4_CNode_Mint / seL4_CNode_Revoke / seL4_CNode_Delete /
 * seL4_SetCapReceivePath when a PD operates on slots in its own CSpace. */
#define AOS_CAP_LEND_SELF_CNODE_SLOT     46u

/* Frame the lender owns outright (root-task-retyped directly into the
 * lender's CNode at boot, full rights) -- the object being lent. */
#define AOS_CAP_LEND_FRAME_SLOT          47u

/* Slot in the lender's own CNode where aos_cap_lend() mints the
 * badged, rights-reduced derivative, ready for IPC transfer. */
#define AOS_CAP_LEND_DERIVED_SLOT        48u

/* Slot in the borrower's own CNode that receives the derivative over IPC.
 * Must be deleted (seL4_CNode_Delete) immediately before each
 * seL4_SetCapReceivePath, never left set across a Recv -- an occupied
 * receive slot silently fails to deliver and the borrower would proceed
 * with whatever stale capability was already there. */
#define AOS_CAP_LEND_BORROWER_RECV_SLOT  49u

/* Badge the lender mints the derivative with; the borrower can verify the
 * capability it received by badge via seL4_CNode_SaveCaller-style checks
 * if needed, and it doubles as the lease's recorded borrower_pd value. */
#define AOS_CAP_LEND_BADGE               0xCA9u

/* Byte pattern the lender writes into the lent frame before transfer, so a
 * probe can assert the borrower reads back the exact bytes (proof that the
 * loan was actually used, not merely transferred). */
#define AOS_CAP_LEND_PATTERN_BYTE        0xA5u

/* Self-reference to the lender/borrower's own VSpace, copied in by the root
 * task alongside AOS_CAP_LEND_SELF_CNODE_SLOT (same pattern as
 * AOS_QUEUE_SERVICE_VSPACE) so each PD can map the frame it owns or
 * receives without a service PD doing it on its behalf. */
#define AOS_CAP_LEND_SELF_VSPACE_SLOT    50u

/* Radix, in bits, of cap_lend_lender's and cap_lend_borrower's own CNode
 * (pd_desc_t.cnode_size_bits for both PDs in system_desc_aarch64.c). Used
 * as the depth argument to CNode/Mint/Revoke/Delete/SetCapReceivePath
 * invocations each PD makes on itself. */
#define AOS_CAP_LEND_CNODE_BITS          8u

/* Virtual address, in each PD's own VSpace, at which the lent frame is
 * mapped -- by the lender (its own copy) and by the borrower (the
 * received derivative). Arbitrary but must not collide with the fixed
 * low-memory mappings (IPC buffer, ELF image) every PD already has. */
#define AOS_CAP_LEND_FRAME_VA            0x30000000UL

/*
 * ── Task 3 target-proof additions ───────────────────────────────────────
 *
 * A synchronisation pair of Notification objects (the same mechanism
 * tests/platform/framebuffer_client_pd.c uses for its peer rendezvous).
 * Each slot name below is shared between both PDs' descriptors in
 * system_desc_aarch64.c, but the root task grants a DIFFERENT,
 * non-overlapping right to each side (see main.c's AGENTOS_CAP_LEND_TEST
 * provisioning block) -- Wait-only to the receiver, Signal-only to the
 * sender -- so neither side can forge the other's half of the handshake.
 *
 * Without this pair, nothing coordinates the two PDs: the lender could
 * revoke before the borrower finishes using (or sub-delegating) the loan,
 * making "the borrower faults after revoke" true by race rather than by
 * revocation; and the borrower could re-probe the capability before the
 * revoke has actually happened, making Probes 2/3 pass on a timing fluke
 * rather than on the kernel having actually torn anything down. The
 * handshake is exactly two one-shot signals:
 *
 *   1. borrower -> lender on AOS_CAP_LEND_DONE_NTFN_SLOT: "I've used the
 *      loan (Probe 1), minted my own sub-delegated copy of it, and
 *      confirmed that copy is alive (Probe 3's precondition) -- you may
 *      revoke now."
 *   2. lender -> borrower on AOS_CAP_LEND_REVOKED_NTFN_SLOT: "revoked --
 *      your next access, and your sub-delegate's, must now fail."
 *
 * The lender MUST NOT call aos_cap_lend_revoke() before receiving signal
 * 1, and the borrower MUST NOT re-probe the capability before receiving
 * signal 2 -- see lender_pd.c / borrower_pd.c for exactly where each wait
 * sits in the control flow.
 */
#define AOS_CAP_LEND_DONE_NTFN_SLOT          51u
#define AOS_CAP_LEND_REVOKED_NTFN_SLOT       52u

/* Slot in the borrower's own CNode for the capability it sub-delegates
 * from its received derivative (Probe 3: a further-derived copy must die
 * when the lender revokes the ORIGINAL, not just the direct loan). Minted
 * via aos_cap_lend() from AOS_CAP_LEND_BORROWER_RECV_SLOT, same as any
 * other lend -- the borrower acting as its own (test-only) further
 * lender, which is exactly the sub-delegation this probe exists to catch
 * if revocation ever missed it. */
#define AOS_CAP_LEND_SUBDELEGATE_SLOT        53u

/* Scratch slot the borrower uses twice to test whether
 * AOS_CAP_LEND_SUBDELEGATE_SLOT is still a live capability: seL4_CNode_Copy
 * from it into this slot, then immediately seL4_CNode_Delete this slot
 * again. Before revoke this must succeed (proving the sub-delegate was
 * ever alive, mirroring Probe 1's "prove it worked first" discipline);
 * after revoke this must fail -- a kernel-enforced error on an already-
 * deleted capability, not a self-report. Pure CSpace check, no VA or page
 * table involved. */
#define AOS_CAP_LEND_SUBDELEGATE_PROBE_SLOT  54u

/* Badge the borrower mints its sub-delegated copy with. Distinct from
 * AOS_CAP_LEND_BADGE (the lender's own mint) purely so a boot-log/ledger
 * reader can tell the two mints apart; aos_cap_lend()'s subsetting check
 * does not care what badge is requested. */
#define AOS_CAP_LEND_SUBDELEGATE_BADGE        0xCA9Du

/* Badge the root task mints cap_lend_borrower's dedicated fault endpoint
 * with (ROOT_PROBE_BADGE in main.c's AGENTOS_CAP_LEND_TEST branch of the
 * ROOT_FAULT_PROBE chain) -- distinct from every other image variant's
 * probe badge so a fault from an unrelated source can never satisfy this
 * oracle by coincidence.
 *
 * The capability carrying this badge never leaves root's CSpace: root
 * installs it on the borrower's TCB itself, and a TCB fault handler is
 * not addressable from any CSpace. No PD holds a capability to root's
 * fault endpoint at all. So this badge can only appear in root's fault
 * loop on a kernel-generated fault IPC -- the borrower cannot send a
 * fault-shaped message that satisfies the oracle. */
#define AOS_CAP_LEND_PROBE_BADGE               0xCA5Eu

/*
 * Greppable boot-log markers, printed via serial_log (serial_log.h) over
 * the normal serial-contract shared page both PDs are provisioned with
 * (same channel native_rust_client uses for boot diagnostics -- see
 * system_desc_aarch64.c's init_eps and main.c's serial-transfer-page
 * name_eq list, both of which list "cap_lend_lender"/"cap_lend_borrower").
 *
 * A mint/transfer/map/verify failure and success are otherwise
 * indistinguishable in the boot log (both paths just park()), which makes
 * "the borrower faults after revoke" vacuous if the loan never actually
 * worked. These markers exist so Task 3's oracle can assert the loan
 * demonstrably succeeded BEFORE keying off any revoke/fault behavior.
 * Each failure marker names the exact step that failed.
 */
#define AOS_CAP_LEND_MARKER_LENDER_OK \
    "[cap-lend-lender] OK: lent and transferred\n"
#define AOS_CAP_LEND_MARKER_LENDER_FAIL_MAP \
    "[cap-lend-lender] FAIL: map own frame\n"
#define AOS_CAP_LEND_MARKER_LENDER_FAIL_LEND \
    "[cap-lend-lender] FAIL: aos_cap_lend\n"

#define AOS_CAP_LEND_MARKER_BORROWER_OK \
    "[cap-lend-borrower] OK: received and verified pattern\n"
#define AOS_CAP_LEND_MARKER_BORROWER_FAIL_RECV \
    "[cap-lend-borrower] FAIL: recv delivered no capability\n"
#define AOS_CAP_LEND_MARKER_BORROWER_FAIL_MAP \
    "[cap-lend-borrower] FAIL: map received derivative\n"
#define AOS_CAP_LEND_MARKER_BORROWER_FAIL_VERIFY \
    "[cap-lend-borrower] FAIL: pattern mismatch\n"

/*
 * Task 3 target-proof markers. Probe 1 is AOS_CAP_LEND_MARKER_BORROWER_OK
 * above (the loan was actually usable, exact bytes). These cover Probes 2
 * and 3 -- revocation withdrawing both the direct loan and a sub-delegated
 * copy -- plus the non-vacuity evidence Step 4 of the plan requires: if
 * revoke is ever neutered, the FAIL markers below are what proves it,
 * instead of a bare timeout that could mean anything.
 */
#define AOS_CAP_LEND_MARKER_BORROWER_FAIL_SUBDELEGATE \
    "[cap-lend-borrower] FAIL: aos_cap_lend subdelegate mint\n"
#define AOS_CAP_LEND_MARKER_BORROWER_FAIL_SUBDELEGATE_PRECHECK \
    "[cap-lend-borrower] FAIL: sub-delegated copy not alive before revoke\n"
/* Probe 3 precondition: the sub-delegated copy exists and is demonstrably
 * alive BEFORE revoke -- mirrors Probe 1's "prove it worked first"
 * discipline so "the sub-delegate is dead" means something afterward. */
#define AOS_CAP_LEND_MARKER_BORROWER_SUBDELEGATE_OK \
    "[cap-lend-borrower] OK: sub-delegated copy minted and alive\n"

#define AOS_CAP_LEND_MARKER_LENDER_FAIL_REVOKE \
    "[cap-lend-lender] FAIL: aos_cap_lend_revoke\n"
/* Probe 2 precondition on the lender's side: the revoke call itself
 * returned success. (The borrower's side of Probe 2's pass evidence is
 * NOT a marker from either test PD at all -- see
 * AOS_CAP_LEND_MARKER_ROOT_FAULT_VERIFIED below.) */
#define AOS_CAP_LEND_MARKER_LENDER_REVOKE_OK \
    "[cap-lend-lender] OK: revoked original\n"

/* Probe 3 pass: the sub-delegated copy is dead too -- a kernel-enforced
 * seL4_CNode_Copy failure on a capability that was alive moments earlier,
 * not a self-report. */
#define AOS_CAP_LEND_MARKER_BORROWER_SUBDELEGATE_DEAD_OK \
    "[cap-lend-borrower] OK: sub-delegated copy dead after revoke\n"
/* Probe 3 non-vacuity evidence: would only appear if revoke failed to tear
 * down a sub-delegation (e.g. Step 4's neutered-revoke run). */
#define AOS_CAP_LEND_MARKER_BORROWER_FAIL_SUBDELEGATE_ALIVE \
    "[cap-lend-borrower] FAIL: sub-delegated copy still alive after revoke\n"

/* Probe 2 non-vacuity evidence: would only appear if the borrower's direct
 * read of the full pattern succeeded after the lender's revoke call --
 * i.e. revocation did not actually withdraw the loan. In the normal
 * (revoke genuinely works) case, execution never reaches this check: the
 * read faults on its very first byte and the PD never returns here. */
#define AOS_CAP_LEND_MARKER_BORROWER_STILL_READABLE \
    "[cap-lend-borrower] WARN: read succeeded after revoke call (non-vacuity check)\n"

/* Probe 2 pass, emitted by the ROOT TASK (main.c's ROOT_FAULT_PROBE /
 * ROOT_PROBE_* block), never by the borrower itself: a successful fault
 * never returns control to the faulting PD, so the pass evidence has to
 * come from the kernel-enforced fault path the root task observes
 * independently, not a self-report from the PD that just got cut off. */
#define AOS_CAP_LEND_MARKER_ROOT_FAULT_VERIFIED \
    "[cap-lend-probe] OK: borrower access faulted after revoke\n"
