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
 * ── Priority DAG ──────────────────────────────────────────────────────────────
 *
 * Same principle as AArch64: providers run at higher priority than callers.
 * RISC-V adds irq_pd (PLIC routing) and timer_pd above hardware drivers.
 *
 *   255  fault_handler   — fault delivery must preempt everything
 *   250  irq_pd          — PLIC IRQ routing; hardware interrupt demuxer
 *   245  timer_pd        — timer interrupt handler
 *   240  nameserver      — foundation: every PD resolves caps through it
 *   230  log_drain       — nearly every PD logs
 *   220  serial_pd       — UART hardware driver
 *   215  virtio_blk      — block device driver
 *   210  net_pd          — virtio-net hardware driver
 *   205  net_server      — lwIP network stack over net_pd
 *   195  event_bus       — pub/sub backbone
 *   185  vfs_server      — virtual filesystem
 *   175  agentfs         — content-addressed object store
 *   170  framebuffer_pd  — framebuffer driver (optional)
 *   165  vibe_engine     — WASM hot-swap engine
 *   160  linux_vmm       — Linux guest VMM
 *   155  freebsd_vmm     — FreeBSD guest VMM
 *   110  init_agent      — agent-ecosystem bootstrapper
 *    50  controller      — policy coordinator; calls everything above it
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "system_desc.h"

/* ── RISC-V 64 system description ─────────────────────────────────────────── */

const system_desc_t system_desc_riscv64 = {
    .pd_count = 18u,
    .pds = {

        /* pd[0] — nameserver (MUST be first; prio 240) */
        {
            .name            = "nameserver",
            .elf_path        = "nameserver.elf",
            .stack_size      = 0x4000u,
            .cnode_size_bits = 10u,
            .priority        = 240u,
            .init_ep_count   = 0u,
            .init_eps        = {},
        },

        /* pd[1] — log_drain (prio 230) */
        {
            .name            = "log_drain",
            .elf_path        = "log_drain.elf",
            .stack_size      = 0x4000u,
            .cnode_size_bits = 10u,
            .priority        = 230u,
            .init_ep_count   = 1u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
            },
        },

        /* pd[2] — serial_pd (prio 220; UART hardware driver) */
        {
            .name            = "serial_pd",
            .elf_path        = "serial_pd.elf",
            .stack_size      = 0x4000u,
            .cnode_size_bits = 10u,
            .priority        = 220u,
            .init_ep_count   = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[3] — event_bus (prio 195; pub/sub backbone) */
        {
            .name            = "event_bus",
            .elf_path        = "event_bus.elf",
            .stack_size      = 0x4000u,
            .cnode_size_bits = 10u,
            .priority        = 195u,
            .init_ep_count   = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[4] — irq_pd (prio 250; PLIC IRQ routing)
         * Highest non-fault priority: hardware interrupt demultiplexer.
         * All PLIC IRQs arrive here first before being forwarded to drivers. */
        {
            .name            = "irq_pd",
            .elf_path        = "irq_pd.elf",
            .stack_size      = 0x4000u,
            .cnode_size_bits = 10u,
            .priority        = 250u,
            .init_ep_count   = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[5] — timer_pd (prio 245; timer interrupt handler) */
        {
            .name            = "timer_pd",
            .elf_path        = "timer_pd.elf",
            .stack_size      = 0x4000u,
            .cnode_size_bits = 10u,
            .priority        = 245u,
            .init_ep_count   = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[6] — controller (prio 50; policy coordinator) */
        {
            .name            = "controller",
            .elf_path        = "controller.elf",
            .stack_size      = 0x10000u,
            .cnode_size_bits = 10u,
            .priority        = 50u,
            .init_ep_count   = 3u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_EVENTBUS,   PD_CNODE_SLOT_EVENTBUS_EP   },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[7] — init_agent (prio 110; agent-ecosystem bootstrapper) */
        {
            .name            = "init_agent",
            .elf_path        = "init_agent.elf",
            .stack_size      = 0x8000u,
            .cnode_size_bits = 10u,
            .priority        = 110u,
            .init_ep_count   = 3u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_EVENTBUS,   PD_CNODE_SLOT_EVENTBUS_EP   },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[8] — agentfs (prio 175; content-addressed object store) */
        {
            .name            = "agentfs",
            .elf_path        = "agentfs.elf",
            .stack_size      = 0x8000u,
            .cnode_size_bits = 10u,
            .priority        = 175u,
            .init_ep_count   = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[9] — vibe_engine (prio 165; WASM hot-swap lifecycle) */
        {
            .name            = "vibe_engine",
            .elf_path        = "vibe_engine.elf",
            .stack_size      = 0x8000u,
            .cnode_size_bits = 10u,
            .priority        = 165u,
            .init_ep_count   = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[10] — vfs_server (prio 185; virtual filesystem multiplexer) */
        {
            .name            = "vfs_server",
            .elf_path        = "vfs_server.elf",
            .stack_size      = 0x8000u,
            .cnode_size_bits = 10u,
            .priority        = 185u,
            .init_ep_count   = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[11] — net_pd (prio 210; virtio-net hardware driver)
         * Owns the virtio-mmio[0] device frame (0x10001000) and PLIC IRQ 1.
         * Runs above net_server (205) which calls it for packet I/O. */
        {
            .name            = "net_pd",
            .elf_path        = "net_pd.elf",
            .stack_size      = 0x8000u,
            .cnode_size_bits = 10u,
            .priority        = 210u,
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

        /* pd[12] — virtio_blk (prio 215; block device driver)
         * Owns the virtio-mmio[1] device frame (0x10002000) and PLIC IRQ 2.
         * Runs above all storage consumers (agentfs 175, vfs_server 185). */
        {
            .name            = "virtio_blk",
            .elf_path        = "virtio_blk.elf",
            .stack_size      = 0x4000u,
            .cnode_size_bits = 10u,
            .priority        = 215u,
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

        /* pd[13] — net_server (prio 205; lwIP network stack over net_pd)
         * Runs above callers (vfs_server 185, controller 50) but below
         * net_pd (210) so the hardware driver can preempt the stack. */
        {
            .name            = "net_server",
            .elf_path        = "net_server.elf",
            .stack_size      = 0x8000u,
            .cnode_size_bits = 10u,
            .priority        = 205u,
            .init_ep_count   = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[14] — framebuffer_pd (prio 170; framebuffer driver, optional) */
        {
            .name            = "framebuffer_pd",
            .elf_path        = "framebuffer_pd.elf",
            .stack_size      = 0x8000u,
            .cnode_size_bits = 10u,
            .priority        = 170u,
            .init_ep_count   = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[15] — fault_handler (prio 255; highest, handles all faults) */
        {
            .name            = "fault_handler",
            .elf_path        = "fault_handler.elf",
            .stack_size      = 0x4000u,
            .cnode_size_bits = 10u,
            .priority        = 255u,
            .init_ep_count   = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[16] — linux_vmm (prio 160; Linux guest VMM)
         * Runs above init_agent (110) and controller (50) so that VM operations
         * requested by those orchestrators complete without starving. */
        {
            .name            = "linux_vmm",
            .elf_path        = "linux_vmm.elf",
            .stack_size      = 0x10000u,
            .cnode_size_bits = 10u,
            .priority        = 160u,
            .init_ep_count   = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[17] — freebsd_vmm (prio 155; FreeBSD guest VMM) */
        {
            .name            = "freebsd_vmm",
            .elf_path        = "freebsd_vmm.elf",
            .stack_size      = 0x10000u,
            .cnode_size_bits = 10u,
            .priority        = 155u,
            .init_ep_count   = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },
    },
};
