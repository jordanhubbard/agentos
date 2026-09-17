/*
 * x86_vtx_proof.h — wire protocol for the isolated VMX/EPT smoke proof.
 *
 * This is intentionally a narrow, board-specific qualification protocol.
 * It reports one VMX non-root HLT exit from the VMM PD to the root task.
 * It is not a guest lifecycle, device, or operating-system ABI.
 */

#ifndef AGENTOS_X86_VTX_PROOF_H
#define AGENTOS_X86_VTX_PROOF_H

#define AOS_X86_VTX_PROOF_LABEL       0x5856u /* "XV" */
#define AOS_X86_VTX_PROOF_PASS         1u
#define AOS_X86_VTX_PROOF_FAIL         2u
#define AOS_X86_VTX_MODES_PASS         3u
#define AOS_X86_VTX_RESET_EXIT         4u
#define AOS_X86_VTX_FIRMWARE_LONG      5u
#define AOS_X86_VTX_FIRMWARE_CONFIG    6u
/* Firmware report: MR0..3 retain status/reason/RIP/detail; MR4..9 contain
 * timer exits, interrupt injections, EOI writes, VMX rate shift, TSC Hz,
 * and HLT exits. Counters are diagnostics, not an aggregate success claim. */
/* Failure snapshot: code and stack virtual bases, validity bitmaps, then
 * 10 code and 32 stack qwords. Invalid words are zero, never device reads.
 * Two sets describe the returned budget exit and the most recent HLT exit
 * (or PM timer poll before any HLT).
 * Snapshots are present only on the diagnostic budget failure. */
#define AOS_X86_FIRMWARE_CODE_WORDS 10u
#define AOS_X86_FIRMWARE_STACK_WORDS 32u
#define AOS_X86_FIRMWARE_SNAPSHOT_SET_WORDS 46u
#define AOS_X86_FIRMWARE_SNAPSHOT_WORDS 92u
/* MR102..115: RBP, valid frame count, six {previous RBP, return RIP}
 * pairs at last HLT/PM poll, or at budget exit without either observation.
 * Optional diagnostic chain only; no unwind/success guarantee. */
#define AOS_X86_FIRMWARE_CHAIN_FRAMES 6u
#define AOS_X86_FIRMWARE_CHAIN_WORDS 14u
/* MR116..119: bytes consumed from kernel/initrd/cmdline and last exit
 * qualification. Transfer counters are not EFI entry or Linux boot proof. */
#define AOS_X86_FIRMWARE_REPORT_WORDS 120u
#define AOS_X86_FIRMWARE_BASE     0xffc00000u
#define AOS_X86_FIRMWARE_BYTES    0x00400000u
#ifndef AOS_X86_FIRMWARE_RAM
#define AOS_X86_FIRMWARE_RAM      0x02000000u
#endif
#define AOS_X86_FIRMWARE_RAM_VA   0x80000000u
#define AOS_X86_FIRMWARE_ROM_VA   0x90000000u
#define AOS_X86_VTX_GUEST_RIP       0x1000u
#define AOS_X86_VTX_GUEST_PML4_GPA  0x2000u
#define AOS_X86_VTX_GUEST_PDPT_GPA  0x3000u
#define AOS_X86_VTX_GUEST_PD_GPA    0x4000u
#define AOS_X86_VTX_GUEST_PT_GPA    0x5000u
#define AOS_X86_VTX_HLT_EXIT_REASON   12u
#define AOS_X86_VTX_HLT_INSTRUCTION_LEN 1u

#endif /* AGENTOS_X86_VTX_PROOF_H */
