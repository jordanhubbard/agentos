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
#define AOS_X86_VTX_GUEST_RIP       0x1000u
#define AOS_X86_VTX_GUEST_PML4_GPA  0x2000u
#define AOS_X86_VTX_GUEST_PDPT_GPA  0x3000u
#define AOS_X86_VTX_GUEST_PD_GPA    0x4000u
#define AOS_X86_VTX_GUEST_PT_GPA    0x5000u
#define AOS_X86_VTX_HLT_EXIT_REASON   12u
#define AOS_X86_VTX_HLT_INSTRUCTION_LEN 1u

#endif /* AGENTOS_X86_VTX_PROOF_H */
