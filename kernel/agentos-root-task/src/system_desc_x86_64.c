/*
 * system_desc_x86_64.c - reduced smoke and opt-in VMX qualification topologies.
 *
 * The normal x86_64_generic board intentionally starts no PDs.  The separate
 * x86_64_generic_vtx board starts exactly one VMM PD for a one-instruction,
 * EPT-backed HLT-exit proof.  It is not a general x86 guest topology.
 */

#include "system_desc.h"

#if defined(AGENTOS_X86_VTX)
const system_desc_t system_desc_x86_64 = {
    .pd_count = 1u,
    .pds = {
        {
            .name = "guest_vmm_primary",
            .elf_path = "guest_vmm_primary.elf",
            .stack_size = 0x10000u,
            .cnode_size_bits = 10u,
            .priority = 250u,
            .self_svc_id = SVC_ID_GUEST_VMM_PRIMARY,
            .init_ep_count = 0u,
            .irq_count = 0u,
            .device_frame_count = 0u,
            .mr_count = 0u,
        },
    },
};
#else
const system_desc_t system_desc_x86_64 = {
    .pd_count = 0u,
    .pds = {},
};
#endif
