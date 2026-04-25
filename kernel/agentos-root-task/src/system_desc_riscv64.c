/*
 * system_desc_riscv64.c — compile-time system topology for RISC-V 64 / QEMU virt
 *
 * Defines the system_desc_riscv64 constant used by the root task boot sequence
 * on RISC-V 64 targets (qemu_virt_riscv64 and compatible).
 *
 * PD ordering is critical: the nameserver MUST be first so that subsequent
 * PDs can register themselves with it before any inter-PD communication.
 *
 * QEMU virt RISC-V MMIO device layout (virtio-mmio):
 *   0x10001000  virtio-mmio[0]  virtio-net  PLIC IRQ 1
 *   0x10008000  virtio-mmio[7]  virtio-blk  PLIC IRQ 8
 *
 * Each virtio-mmio device region is 0x200 bytes (rounds up to one 4K page).
 *
 * PD CNode size notes:
 *   cnode_size_bits = 6 → 64 slots (for PDs with no IRQs or device frames)
 *   cnode_size_bits = 7 → 128 slots (for PDs with IRQs needing slots ≥ 64)
 *   PD_IRQHANDLER_SLOT_BASE = 64, so IRQ PDs need cnode_size_bits ≥ 7.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "system_desc.h"

/* ── RISC-V 64 system description ─────────────────────────────────────────── */

const system_desc_t system_desc_riscv64 = {
    .pd_count = 18u,
    .pds = {

        /* pd[0] — nameserver (MUST be first; prio 130) */
        {
            .name            = "nameserver",
            .elf_path        = "nameserver.elf",
            .stack_size      = 0x4000u,
            .cnode_size_bits = 6u,
            .priority        = 130u,
            .init_ep_count   = 0u,
            .init_eps        = {},
        },

        /* pd[1] — log_drain (prio 120) */
        {
            .name            = "log_drain",
            .elf_path        = "log_drain.elf",
            .stack_size      = 0x4000u,
            .cnode_size_bits = 6u,
            .priority        = 120u,
            .init_ep_count   = 1u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
            },
        },

        /* pd[2] — serial_pd (prio 180) */
        {
            .name            = "serial_pd",
            .elf_path        = "serial_pd.elf",
            .stack_size      = 0x4000u,
            .cnode_size_bits = 6u,
            .priority        = 180u,
            .init_ep_count   = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[3] — event_bus (prio 200) */
        {
            .name            = "event_bus",
            .elf_path        = "event_bus.elf",
            .stack_size      = 0x4000u,
            .cnode_size_bits = 6u,
            .priority        = 200u,
            .init_ep_count   = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[4] — irq_pd (prio 230) — highest non-fault priority */
        {
            .name            = "irq_pd",
            .elf_path        = "irq_pd.elf",
            .stack_size      = 0x4000u,
            .cnode_size_bits = 6u,
            .priority        = 230u,
            .init_ep_count   = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[5] — timer_pd (prio 225) */
        {
            .name            = "timer_pd",
            .elf_path        = "timer_pd.elf",
            .stack_size      = 0x4000u,
            .cnode_size_bits = 6u,
            .priority        = 225u,
            .init_ep_count   = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[6] — controller (prio 50) */
        {
            .name            = "controller",
            .elf_path        = "controller.elf",
            .stack_size      = 0x10000u,
            .cnode_size_bits = 6u,
            .priority        = 50u,
            .init_ep_count   = 3u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_EVENTBUS,   PD_CNODE_SLOT_EVENTBUS_EP   },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[7] — init_agent (prio 100) */
        {
            .name            = "init_agent",
            .elf_path        = "init_agent.elf",
            .stack_size      = 0x8000u,
            .cnode_size_bits = 6u,
            .priority        = 100u,
            .init_ep_count   = 3u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_EVENTBUS,   PD_CNODE_SLOT_EVENTBUS_EP   },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[8] — agentfs (prio 150) */
        {
            .name            = "agentfs",
            .elf_path        = "agentfs.elf",
            .stack_size      = 0x8000u,
            .cnode_size_bits = 6u,
            .priority        = 150u,
            .init_ep_count   = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[9] — vibe_engine (prio 140) */
        {
            .name            = "vibe_engine",
            .elf_path        = "vibe_engine.elf",
            .stack_size      = 0x8000u,
            .cnode_size_bits = 6u,
            .priority        = 140u,
            .init_ep_count   = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[10] — vfs_server (prio 160) */
        {
            .name            = "vfs_server",
            .elf_path        = "vfs_server.elf",
            .stack_size      = 0x8000u,
            .cnode_size_bits = 6u,
            .priority        = 160u,
            .init_ep_count   = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[11] — net_pd (virtio-net driver, prio 175)
         * Owns the virtio-mmio[0] device frame (0x10001000) and PLIC IRQ 1.
         * cnode_size_bits = 7 so IRQ handler cap fits at slot 64.          */
        {
            .name            = "net_pd",
            .elf_path        = "net_pd.elf",
            .stack_size      = 0x8000u,
            .cnode_size_bits = 7u,
            .priority        = 175u,
            .init_ep_count   = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
            .irq_count = 1u,
            .irqs = {
                { .irq_number = 1u, .ntfn_badge = 1u, .name = "virtio-net" },
            },
            .device_frame_count = 1u,
            .device_frames = {
                { .paddr = 0x10001000ULL, .size_bits = 12u,
                  .cnode_slot = 8u, .name = "vnet-mmio" },
            },
        },

        /* pd[12] — virtio_blk (block device driver, prio 190)
         * Owns the virtio-mmio[1] device frame (0x10002000) and PLIC IRQ 2.
         * Placed second on the QEMU command line so QEMU assigns it slot 1. */
        {
            .name            = "virtio_blk",
            .elf_path        = "virtio_blk.elf",
            .stack_size      = 0x4000u,
            .cnode_size_bits = 7u,
            .priority        = 190u,
            .init_ep_count   = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
            .irq_count = 1u,
            .irqs = {
                { .irq_number = 2u, .ntfn_badge = 2u, .name = "virtio-blk" },
            },
            .device_frame_count = 1u,
            .device_frames = {
                { .paddr = 0x10002000ULL, .size_bits = 12u,
                  .cnode_slot = 8u, .name = "vblk-mmio" },
            },
        },

        /* pd[13] — net_server (prio 170) — lwIP network stack over net_pd */
        {
            .name            = "net_server",
            .elf_path        = "net_server.elf",
            .stack_size      = 0x8000u,
            .cnode_size_bits = 6u,
            .priority        = 170u,
            .init_ep_count   = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[14] — framebuffer_pd (prio 175, optional) */
        {
            .name            = "framebuffer_pd",
            .elf_path        = "framebuffer_pd.elf",
            .stack_size      = 0x8000u,
            .cnode_size_bits = 6u,
            .priority        = 175u,
            .init_ep_count   = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[15] — fault_handler (prio 255) — highest, handles all faults */
        {
            .name            = "fault_handler",
            .elf_path        = "fault_handler.elf",
            .stack_size      = 0x4000u,
            .cnode_size_bits = 6u,
            .priority        = 255u,
            .init_ep_count   = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[16] — linux_vmm (prio 90) — Linux guest process-in-PD */
        {
            .name            = "linux_vmm",
            .elf_path        = "linux_vmm.elf",
            .stack_size      = 0x10000u,
            .cnode_size_bits = 6u,
            .priority        = 90u,
            .init_ep_count   = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[17] — freebsd_vmm (prio 85) — FreeBSD guest process-in-PD */
        {
            .name            = "freebsd_vmm",
            .elf_path        = "freebsd_vmm.elf",
            .stack_size      = 0x10000u,
            .cnode_size_bits = 6u,
            .priority        = 85u,
            .init_ep_count   = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },
    },
};
