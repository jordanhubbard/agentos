/*
 * system_desc_aarch64.c — compile-time system topology for AArch64 / QEMU virt
 *
 * Defines the system_desc_aarch64 constant used by the root task boot sequence
 * on AArch64 targets (qemu_virt_aarch64, ZCU102, RPi5, etc.).
 *
 * PD ordering is critical: the nameserver MUST be first so that subsequent
 * PDs can register themselves with it before any inter-PD communication.
 *
 * Priority values match the <protection_domain priority="..."> attributes in
 * agentos-aarch64.system and the agentos_priority_t enum in agentos.h.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "system_desc.h"

/* ── AArch64 system description ───────────────────────────────────────────── */

const system_desc_t system_desc_aarch64 = {
    .pd_count = 14u,
    .pds = {

        /* pd[0] — nameserver (MUST be first; prio 130) */
        {
            .name           = "nameserver",
            .elf_path       = "nameserver.elf",
            .stack_size     = 0x4000u,   /* 16 KB */
            .cnode_size_bits = 6u,        /* 64 slots */
            .priority       = 130u,
            .init_ep_count  = 0u,
            .init_eps       = {},
        },

        /* pd[1] — log_drain (prio 120; started before controller so early
         *          boot messages are not dropped) */
        {
            .name           = "log_drain",
            .elf_path       = "log_drain.elf",
            .stack_size     = 0x4000u,
            .cnode_size_bits = 6u,
            .priority       = 120u,
            .init_ep_count  = 1u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
            },
        },

        /* pd[2] — serial_pd (prio 180; UART driver, high prio for low latency) */
        {
            .name           = "serial_pd",
            .elf_path       = "serial_pd.elf",
            .stack_size     = 0x4000u,
            .cnode_size_bits = 6u,
            .priority       = 180u,
            .init_ep_count  = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[3] — event_bus (prio 200; passive pub/sub backbone) */
        {
            .name           = "event_bus",
            .elf_path       = "event_bus.elf",
            .stack_size     = 0x4000u,
            .cnode_size_bits = 6u,
            .priority       = 200u,
            .init_ep_count  = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[4] — controller / monitor (prio 50; system coordinator) */
        {
            .name           = "controller",
            .elf_path       = "controller.elf",
            .stack_size     = 0x10000u,  /* 64 KB — larger stack for policy work */
            .cnode_size_bits = 6u,
            .priority       = 50u,
            .init_ep_count  = 3u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_EVENTBUS,   PD_CNODE_SLOT_EVENTBUS_EP   },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[5] — init_agent (prio 100; agent ecosystem bootstrapper) */
        {
            .name           = "init_agent",
            .elf_path       = "init_agent.elf",
            .stack_size     = 0x8000u,
            .cnode_size_bits = 6u,
            .priority       = 100u,
            .init_ep_count  = 3u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_EVENTBUS,   PD_CNODE_SLOT_EVENTBUS_EP   },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[6] — agentfs (prio 150; content-addressed object store) */
        {
            .name           = "agentfs",
            .elf_path       = "agentfs.elf",
            .stack_size     = 0x8000u,
            .cnode_size_bits = 6u,
            .priority       = 150u,
            .init_ep_count  = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[7] — vibe_engine (prio 140; WASM hot-swap lifecycle) */
        {
            .name           = "vibe_engine",
            .elf_path       = "vibe_engine.elf",
            .stack_size     = 0x8000u,
            .cnode_size_bits = 6u,
            .priority       = 140u,
            .init_ep_count  = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[8] — vfs_server (prio 160; virtual filesystem multiplexer) */
        {
            .name           = "vfs_server",
            .elf_path       = "vfs_server.elf",
            .stack_size     = 0x8000u,
            .cnode_size_bits = 6u,
            .priority       = 160u,
            .init_ep_count  = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[9] — net_server (prio 170; lwIP-based network stack) */
        {
            .name           = "net_server",
            .elf_path       = "net_server.elf",
            .stack_size     = 0x8000u,
            .cnode_size_bits = 6u,
            .priority       = 170u,
            .init_ep_count  = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[10] — virtio_blk (prio 190; block device driver) */
        {
            .name           = "virtio_blk",
            .elf_path       = "virtio_blk.elf",
            .stack_size     = 0x4000u,
            .cnode_size_bits = 6u,
            .priority       = 190u,
            .init_ep_count  = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[11] — linux_vmm (prio 254; highest-priority VMM) */
        {
            .name           = "linux_vmm",
            .elf_path       = "linux_vmm.elf",
            .stack_size     = 0x10000u,
            .cnode_size_bits = 6u,
            .priority       = 254u,
            .init_ep_count  = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[12] — vm_manager (prio 160; multi-VM lifecycle) */
        {
            .name           = "vm_manager",
            .elf_path       = "vm_manager.elf",
            .stack_size     = 0x8000u,
            .cnode_size_bits = 6u,
            .priority       = 160u,
            .init_ep_count  = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[13] — fault_handler (prio 255; highest priority for fault recovery) */
        {
            .name           = "fault_handler",
            .elf_path       = "fault_handler.elf",
            .stack_size     = 0x4000u,
            .cnode_size_bits = 6u,
            .priority       = 255u,
            .init_ep_count  = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },
    },
};
