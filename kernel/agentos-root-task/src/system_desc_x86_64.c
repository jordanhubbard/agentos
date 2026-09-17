/*
 * system_desc_x86_64.c - reduced smoke and opt-in VMX qualification topologies.
 *
 * The normal x86_64_generic board intentionally starts no PDs.  The separate
 * x86_64_generic_vtx board starts exactly one VMM PD for a one-instruction,
 * EPT-backed HLT-exit proof. The firmware composition starts the COM2 serial
 * driver and serial_virt, plus virtio_blk and blk_virt. Host register/DMA
 * mappings belong to storage PDs; the VMM maps only its own queue page.
 */

#include "system_desc.h"

#if defined(AGENTOS_X86_VTX)
const system_desc_t system_desc_x86_64 = {
#ifdef AGENTOS_X86_FIRMWARE_RESET
    .pd_count = 5u,
#else
    .pd_count = 1u,
#endif
    .pds = {
#ifdef AGENTOS_X86_FIRMWARE_RESET
        {
            .name = "virtio_blk",
            .elf_path = "virtio_blk.elf",
            .stack_size = 0x4000u,
            .cnode_size_bits = 10u,
            .priority = 215u,
            .self_svc_id = SVC_ID_VIRTIO_BLK,
        },
        {
            .name = "blk_virt",
            .elf_path = "blk_virt.elf",
            .stack_size = 0x8000u,
            .cnode_size_bits = 10u,
            .priority = 210u,
            .self_svc_id = SVC_ID_BLK_VIRT,
            .init_ep_count = 1u,
            .init_eps = {{ SVC_ID_VIRTIO_BLK, PD_CNODE_SLOT_VIRTIO_BLK_EP }},
        },
        {
            .name = "serial_pd",
            .elf_path = "serial_pd.elf",
            .stack_size = 0x8000u,
            .cnode_size_bits = 10u,
            .priority = 180u,
            .self_svc_id = SVC_ID_SERIAL,
            .init_ep_count = 1u,
            .init_eps = {{ SVC_ID_SERIAL_VIRT, PD_CNODE_SLOT_SERIAL_VIRT_EP }},
        },
        {
            .name = "serial_virt",
            .elf_path = "serial_virt.elf",
            .stack_size = 0x8000u,
            .cnode_size_bits = 10u,
            .priority = 203u,
            .self_svc_id = SVC_ID_SERIAL_VIRT,
        },
#endif
        {
            .name = "guest_vmm_primary",
            .elf_path = "guest_vmm_primary.elf",
            .stack_size = 0x10000u,
            .cnode_size_bits = 10u,
            .priority = 250u,
            .self_svc_id = SVC_ID_GUEST_VMM_PRIMARY,
#ifdef AGENTOS_X86_FIRMWARE_RESET
            .init_ep_count = 2u,
            .init_eps = {
                { SVC_ID_SERIAL_VIRT, PD_CNODE_SLOT_SERIAL_VIRT_EP },
                { SVC_ID_BLK_VIRT, PD_CNODE_SLOT_BLK_VIRT_EP },
            },
#else
            .init_ep_count = 0u,
#endif
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
