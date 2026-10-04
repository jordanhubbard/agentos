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
 * use succeeding, then faulting after aos_cap_lend_revoke -- on this pair.
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
