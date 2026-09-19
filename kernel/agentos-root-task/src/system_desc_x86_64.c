/*
 * system_desc_x86_64.c - reduced smoke and opt-in VMX qualification topologies.
 *
 * The normal x86_64_generic board intentionally starts no PDs.  The separate
 * x86_64_generic_vtx board starts exactly one VMM PD for a one-instruction,
 * EPT-backed HLT-exit proof. The firmware composition starts the COM2 serial
 * driver and serial_virt, plus block and network drivers and virtualizers.
 * Host register/DMA mappings belong to drivers; the VMM maps its queue pages.
 */

#include "system_desc.h"
#include "contracts/guest_ram_caps.h"
#include "contracts/x86_vtx_proof.h"

#if defined(AGENTOS_X86_VTX)
const system_desc_t system_desc_x86_64 = {
#ifdef AGENTOS_X86_FIRMWARE_RESET
    .pd_count = 9u
#ifdef AGENTOS_X86_USERSPACE_PROOF
        + 1u
#endif
#ifdef AGENTOS_X86_MANAGED_START
        + 1u
#endif
        ,
#else
    .pd_count = 1u,
#endif
    .pds = {
#ifdef AGENTOS_X86_FIRMWARE_RESET
        {
            .name = "net_pd",
            .elf_path = "net_pd.elf",
            .stack_size = 0x8000u,
            .cnode_size_bits = 10u,
            .priority = 215u,
            .self_svc_id = SVC_ID_NET_PD,
            .init_ep_count = 1u,
            .init_eps = {{ SVC_ID_NET_VIRT, PD_CNODE_SLOT_NET_VIRT_EP }},
        },
        {
            .name = "net_virt",
            .elf_path = "net_virt.elf",
            .stack_size = 0x8000u,
            .cnode_size_bits = 10u,
            .priority = 205u,
            .self_svc_id = SVC_ID_NET_VIRT,
            .init_ep_count = 1u,
            .init_eps = {{ SVC_ID_NET_PD, PD_CNODE_SLOT_NET_PD_EP }},
        },
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
#ifndef AGENTOS_X86_CC_PCI
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
#endif
        {
            .name = "serial_virt",
            .elf_path = "serial_virt.elf",
            .stack_size = 0x8000u,
            .cnode_size_bits = 10u,
            .priority = 203u,
            .self_svc_id = SVC_ID_SERIAL_VIRT,
        },
        {
            .name = "x86_runner",
            .elf_path = "x86_runner.elf",
            .stack_size = 0x8000u,
            .cnode_size_bits = 10u,
            .priority = 250u,
            .self_svc_id = SVC_ID_X86_RUNNER,
        },
        {
            .name = "x86_runner_ap",
            .elf_path = "x86_runner_ap.elf",
            .stack_size = 0x8000u,
            .cnode_size_bits = 10u,
            .priority = 250u,
            .self_svc_id = SVC_ID_X86_AP_RUNNER,
        },
#endif
        {
            .name = "guest_vmm_primary",
            .elf_path = "guest_vmm_primary.elf",
            .stack_size = 0x10000u,
#ifdef AGENTOS_X86_FIRMWARE_RESET
            /* Private RAM/ROM pool grants and future frame/alias ranges. */
            .cnode_size_bits = AOS_GUEST_RAM_CNODE_BITS,
#else
            .cnode_size_bits = 10u,
#endif
            .priority = 250u,
            .self_svc_id = SVC_ID_GUEST_VMM_PRIMARY,
#ifdef AGENTOS_X86_FIRMWARE_RESET
#ifdef AGENTOS_X86_USERSPACE_PROOF
            .init_ep_count = 4u,
#else
            .init_ep_count = 3u,
#endif
            .init_eps = {
                { SVC_ID_SERIAL_VIRT, PD_CNODE_SLOT_SERIAL_VIRT_EP },
                { SVC_ID_BLK_VIRT, PD_CNODE_SLOT_BLK_VIRT_EP },
                { SVC_ID_NET_VIRT, PD_CNODE_SLOT_NET_VIRT_EP },
#ifdef AGENTOS_X86_USERSPACE_PROOF
                { SVC_ID_X86_LIFECYCLE_PROBE, AOS_X86_LIFECYCLE_PROBE_CAP },
#endif
            },
#else
            .init_ep_count = 0u,
#endif
            .irq_count = 0u,
            .device_frame_count = 0u,
            .mr_count = 0u,
        },
#ifdef AGENTOS_X86_USERSPACE_PROOF
        {
            .name = "x86_lifecycle_probe",
            .elf_path = "x86_lifecycle_probe.elf",
            .stack_size = 0x4000u,
            .cnode_size_bits = 10u,
            .priority = 251u,
            .self_svc_id = SVC_ID_X86_LIFECYCLE_PROBE,
            .init_ep_count = 2u,
            .init_eps = {
                { SVC_ID_GUEST_VMM_PRIMARY, PD_CNODE_SLOT_GUEST_VMM_PRIMARY_EP },
                { SVC_ID_VM_MANAGER, PD_CNODE_SLOT_VM_MANAGER_EP },
            },
        },
#endif
#ifdef AGENTOS_X86_MANAGED_START
        {
            .name = "vm_manager",
            .elf_path = "vm_manager.elf",
            .stack_size = 0x8000u,
            .cnode_size_bits = 10u,
            .priority = 220u,
            .self_svc_id = SVC_ID_VM_MANAGER,
            .init_ep_count = 1u,
            .init_eps = {{ SVC_ID_GUEST_VMM_PRIMARY, PD_CNODE_SLOT_GUEST_VMM_PRIMARY_EP }},
        },
#endif
#ifdef AGENTOS_X86_CC_PCI
        /* Replaces serial_pd in this composition, so the count is unchanged.
         * The PCI transport and guest-console frontend have one owner. */
        {
            .name = "cc_pd",
            .elf_path = "cc_pd.elf",
            .stack_size = 0x8000u,
            .cnode_size_bits = 10u,
            .priority = 200u,
            .self_svc_id = SVC_ID_CC_PD,
            .init_ep_count = 2u,
            .init_eps = {
                { SVC_ID_VM_MANAGER, PD_CNODE_SLOT_VM_MANAGER_EP },
                { SVC_ID_SERIAL_VIRT, PD_CNODE_SLOT_SERIAL_VIRT_EP },
            },
        },
#endif
    },
};
#else
const system_desc_t system_desc_x86_64 = {
    .pd_count = 0u,
    .pds = {},
};
#endif
