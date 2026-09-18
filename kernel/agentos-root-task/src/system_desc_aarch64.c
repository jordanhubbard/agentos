/*
 * system_desc_aarch64.c — compile-time system topology for AArch64 / QEMU virt
 *
 * Defines the system_desc_aarch64 constant used by the root task boot sequence
 * on AArch64 targets (qemu_virt_aarch64, ZCU102, RPi5, etc.).
 *
 * PD ordering is critical: the nameserver MUST be first so that subsequent
 * PDs can register themselves with it before any inter-PD communication.
 *
 * This table is the complete set of PDs the root task spawns.  agentos.toml
 * (consumed by xtask gen-pd-bundle) must list exactly these names plus the
 * Makefile-appended variants (guest_vmm_secondary, fault_inject,
 * test_runner + event_bus); a bundle entry with no row here is never started,
 * and a row here with no bundle entry fails at ELF load.  Museum PDs
 * (docs/TCB.md) are intentionally absent from both
 * (MAC task_56eae59d9aa94d2d9d047f03fc9d22ad), and the non-TCB service PDs
 * that used to ride along (controller, event_bus, init_agent, agentfs,
 * vfs_server, net_server, framebuffer_pd, usb_pd) were dropped by MAC
 * task_f95d118416a24fa484c2c43f0d955b56. CC now calls vm_manager directly.
 * The native operator session is a bounded read-only serial client.
 *
 * ── Priority DAG ──────────────────────────────────────────────────────────────
 *
 * Priorities follow the dependency DAG: a service that is called by others
 * (a "provider") runs at HIGHER priority than the services that call it (the
 * "clients").  This prevents priority inversion on blocking IPC calls and
 * ensures providers can respond quickly even when high-priority clients block
 * waiting for them.
 *
 * DAG level → priority range:
 *
 *   255  fault_handler      — fault delivery must preempt everything
 *   250  guest_vmm          — VM-exit latency is latency-critical
 *   245  nameserver         — foundation: every PD does cap lookup at boot/runtime
 *   235  log_drain          — nearly every PD logs; must respond before callers time-out
 *   225  serial_pd          — UART hardware driver; log_drain and others may call it
 *   215  virtio_blk         — host block device driver; blk_virt calls into it
 *   213  block_pd           — OS-neutral block contract
 *   210  blk_virt           — block virtualizer: the only blk mux; calls virtio_blk,
 *                            is kicked (NBSend) by the VMMs, never called by them
 *                            per request
 *   207  net_pd             — host virtio-net driver; net_virt calls into it
 *   205  net_virt           — network virtualizer: the only net mux; calls net_pd,
 *                            is kicked (NBSend) by the VMMs, never called by them
 *                            per frame
 *   170  vm_manager         — VM lifecycle; downstream of guest-control relays
 *   180  operator_session   — read-only native serial client
 *   164  cc_pd              — CC relay; lowest PD, so it announces boot complete
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "system_desc.h"
#include "contracts/native_rust_probe.h"
#include <platform/guest_memory_layout.h>
#ifdef AGENTOS_GUEST_INPUT
#define AOS_INPUT_PD_EXTRA 1u
#else
#define AOS_INPUT_PD_EXTRA 0u
#endif
#ifdef AGENTOS_DISPLAY_RAMFB
#define AOS_DISPLAY_PD_EXTRA 1u
#else
#define AOS_DISPLAY_PD_EXTRA 0u
#endif

/* agentos-8f5: a target contract-runner PD is appended only in test images,
 * together with the event_bus PD whose contract it exercises. */
#ifdef AGENTOS_FRAMEBUFFER_TEST
#define AOS_TEST_PD_EXTRA (3u + AOS_INPUT_PD_EXTRA + AOS_DISPLAY_PD_EXTRA)
#elif defined(AGENTOS_GUEST_GRAPHICS)
#define AOS_TEST_PD_EXTRA (1u + AOS_INPUT_PD_EXTRA + AOS_DISPLAY_PD_EXTRA)
#elif defined(AGENTOS_SEL4_TEST_IMAGE)
#define AOS_TEST_PD_EXTRA (2u + AOS_INPUT_PD_EXTRA)
#elif defined(AGENTOS_NATIVE_RUST_TEST)
#define AOS_TEST_PD_EXTRA (2u + AOS_INPUT_PD_EXTRA)
#else
#define AOS_TEST_PD_EXTRA AOS_INPUT_PD_EXTRA
#endif

/* Default image: nameserver, log_drain, serial_pd, virtio_blk,
 * block_pd, blk_virt, net_pd, net_virt, serial_virt, guest_vmm_primary, vm_manager,
 * cc_pd, fault_handler. */
#if defined(AGENTOS_FAULT_INJECT) && defined(AGENTOS_GUEST_DUAL)
#define AOS_AARCH64_PD_COUNT (16u + AOS_TEST_PD_EXTRA)
#define AOS_CC_INIT_EP_COUNT 7u
#elif defined(AGENTOS_FAULT_INJECT)
#define AOS_AARCH64_PD_COUNT (15u + AOS_TEST_PD_EXTRA)
#define AOS_CC_INIT_EP_COUNT 7u
#elif defined(AGENTOS_GUEST_DUAL)
#define AOS_AARCH64_PD_COUNT (15u + AOS_TEST_PD_EXTRA)
#define AOS_CC_INIT_EP_COUNT 6u
#else
#define AOS_AARCH64_PD_COUNT (14u + AOS_TEST_PD_EXTRA)
#define AOS_CC_INIT_EP_COUNT 6u
#endif

/* VMM wakeups use send-only notifications granted by root, not their
 * lifecycle endpoints. */
#define AOS_BLK_VIRT_INIT_EP_COUNT 2u

/* Guest data wakeups use root-minted send-only notification caps. */
#define AOS_NET_VIRT_INIT_EP_COUNT 2u

#if defined(AGENTOS_GUEST_DUAL)
#define AOS_VM_MANAGER_INIT_EP_COUNT 4u
#elif defined(AGENTOS_GUEST_PRIMARY) || defined(AGENTOS_GUEST_SECONDARY)
#define AOS_VM_MANAGER_INIT_EP_COUNT 3u
#else
#define AOS_VM_MANAGER_INIT_EP_COUNT 2u
#endif

/* ── AArch64 system description ───────────────────────────────────────────── */

const system_desc_t system_desc_aarch64 = {
    .pd_count = AOS_AARCH64_PD_COUNT,
    .pds = {

        /* pd[0] — nameserver (MUST be first; prio 245)
         * Foundation service: every PD resolves endpoint caps through it.
         * Runs above all drivers and servers so that boot-time registration
         * and runtime lookups complete without blocking the calling PD. */
        {
            .name           = "nameserver",
            .elf_path       = "nameserver.elf",
            .stack_size     = 0x4000u,   /* 16 KB */
            .cnode_size_bits = 10u,       /* 1024 slots */
            .priority       = 245u,
            .self_svc_id    = SVC_ID_NAMESERVER,
            .init_ep_count  = 0u,
            .init_eps       = {},
        },

        /* pd[1] — log_drain (prio 235; just below nameserver)
         * Nearly every PD makes blocking log IPC calls.  Running above all
         * hardware drivers and application servers prevents priority inversion
         * where a caller blocks on a log call while the log server is starved. */
        {
            .name           = "log_drain",
            .elf_path       = "log_drain.elf",
            .stack_size     = 0x4000u,
            .cnode_size_bits = 10u,
            .priority       = 235u,
            .self_svc_id    = SVC_ID_LOG_DRAIN,
            .init_ep_count  = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_SERIAL,     PD_CNODE_SLOT_SERIAL_EP     },
            },
        },

        /* pd[2] — serial_pd (prio 225; UART hardware driver)
         * Provides low-latency serial I/O.  Runs above net/storage servers
         * that may call it for console output, but below log_drain which
         * drives it. */
        {
            .name           = "serial_pd",
            .elf_path       = "serial_pd.elf",
            .stack_size     = 0x4000u,
            .cnode_size_bits = 10u,
            .priority       = 225u,
            .self_svc_id    = SVC_ID_SERIAL,
            .init_ep_count  = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
            .irq_count = 1u,
            .irqs = {
                { .irq_number = 33u, .ntfn_badge = 0x1u, .name = "pl011-uart" },
            },
            .device_frame_count = 1u,
            .device_frames = {
                { .paddr = 0x09000000ULL, .size_bits = 12u,
                  .cnode_slot = 10u, .name = "pl011-mmio" },
            },
        },

        /* pd[4] — virtio_blk (prio 215; virtio block device driver)
         * Lowest-level I/O provider; blk_virt relays block requests to it.
         * Runs above all storage consumers so block I/O completions are
         * processed before the callers time out. */
        {
            .name           = "virtio_blk",
            .elf_path       = "virtio_blk.elf",
            .stack_size     = 0x4000u,
            .cnode_size_bits = 10u,
            .priority       = 215u,
            .self_svc_id    = SVC_ID_VIRTIO_BLK,
            .init_ep_count  = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[5] — block_pd (prio 213; OS-neutral block API)
         * Exposes the block contract to VMMs and native services. */
        {
            .name           = "block_pd",
            .elf_path       = "block_pd.elf",
            .stack_size     = 0x4000u,
            .cnode_size_bits = 10u,
            .priority       = 213u,
            .self_svc_id    = SVC_ID_BLOCK_SERVICE,
            .init_ep_count  = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[6] — blk_virt (prio 210; block virtualizer, the only blk mux)
         * Owns no device frame and no IRQ (docs/TCB.md invariant 1 stays with
         * virtio_blk).  Moves requests between the guest sDDF queues in the
         * shared block region and virtio_blk's chunked DMA-window contract.
         * Sits just below virtio_blk / block_pd because it Calls into the
         * driver, and above every guest-control PD.  VMMs reach it only by
         * ATTACH (once) and persistent kicks; root grants send-only
         * notification caps for responses to each configured VMM. */
        {
            .name           = "blk_virt",
            .elf_path       = "blk_virt.elf",
            .stack_size     = 0x8000u,
            .cnode_size_bits = 10u,
            .priority       = 210u,
            .self_svc_id    = SVC_ID_BLK_VIRT,
            .init_ep_count  = AOS_BLK_VIRT_INIT_EP_COUNT,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_VIRTIO_BLK, PD_CNODE_SLOT_VIRTIO_BLK_EP },
            },
            .irq_count = 0u,
            .irqs = { },
            .device_frame_count = 0u,
            .device_frames = { },
        },

        /* pd[7] — net_pd (prio 207; host virtio-net driver)
         * Owns the host virtio-net device (frame + IRQ).  Its only client is
         * net_virt, whose listen EP it holds to NBSend RX_READY; it never
         * talks to a VMM. */
        {
            .name           = "net_pd",
            .elf_path       = "net_pd.elf",
            .stack_size     = 0x8000u,
            .cnode_size_bits = 10u,
            .priority       = 207u,
            .self_svc_id    = SVC_ID_NET_PD,
            .init_ep_count  = 3u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
                { SVC_ID_NET_VIRT,   PD_CNODE_SLOT_NET_VIRT_EP   },
            },
            .irq_count =
#if defined(AGENTOS_GUEST_PRIMARY) || defined(AGENTOS_GUEST_SECONDARY)
                              1u,
#else
                              0u,
#endif
            .irqs = {
#if defined(AGENTOS_GUEST_PRIMARY) || defined(AGENTOS_GUEST_SECONDARY)
                { .irq_number = 64u, .ntfn_badge = 0x80000000u,
                  .name = "host-net-bus16" },
#endif
            },
        },

        /* pd[8] — net_virt (prio 205; network virtualizer, the only net mux)
         * Owns no device frame and no IRQ (docs/TCB.md invariant 1 stays with
         * net_pd).  Moves frames between the guest sDDF queues in the shared
         * net frame and net_pd's RAW contract.  Sits just below net_pd because
         * it Calls into it, and above every guest-control PD.  VMMs reach it
         * by ATTACH (once); data wakeups use root-granted send-only
         * notification capabilities in both directions. */
        {
            .name           = "net_virt",
            .elf_path       = "net_virt.elf",
            .stack_size     = 0x8000u,
            .cnode_size_bits = 10u,
            .priority       = 205u,
            .self_svc_id    = SVC_ID_NET_VIRT,
            .init_ep_count  = AOS_NET_VIRT_INIT_EP_COUNT,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_NET_PD,     PD_CNODE_SLOT_NET_PD_EP     },
            },
            .irq_count = 0u,
            .irqs = { },
            .device_frame_count = 0u,
            .device_frames = { },
        },

        /* Native client of serial_virt; no device, guest or lifecycle caps. */
        {
            .name = "operator_session",
            .elf_path = "operator_session.elf",
            .stack_size = 0x8000u,
            .cnode_size_bits = 10u,
            .priority = 180u,
            .self_svc_id = SVC_ID_OPERATOR_SESSION,
            .init_ep_count = 1u,
            .init_eps = {{ SVC_ID_SERIAL_VIRT, PD_CNODE_SLOT_SERIAL_VIRT_EP }},
        },
        /* Serial queue virtualizer. */
        {
            .name = "serial_virt",
            .elf_path = "serial_virt.elf",
            .stack_size = 0x8000u,
            .cnode_size_bits = 10u,
            .priority = 203u,
            .self_svc_id = SVC_ID_SERIAL_VIRT,
            /* Diagnostics use the root-provisioned log ring and send-only
             * notification. The mux calls neither UART nor other services. */
            .init_ep_count = 0u,
        },

        /* Guest VMM (prio 250; VM-exit latency is latency-critical).
         * Host device IRQs belong exclusively to driver PDs. Guest virtio
         * interrupts are generated by the emulated devices inside the VMM. */
        {
#if defined(AGENTOS_GUEST_SECONDARY) && !defined(AGENTOS_GUEST_DUAL)
            .name           = "guest_vmm_secondary",
            .elf_path       = "guest_vmm_secondary.elf",
            .stack_size     = 0x10000u,
            .cnode_size_bits = 10u,
            .priority       = 250u,
            .self_svc_id    = SVC_ID_GUEST_VMM_SECONDARY,
            /* No net_pd or virtio_blk EP: the VMM reaches the network only
             * through net_virt and block media only through blk_virt
             * (docs/TCB.md invariant 2). */
            .init_ep_count  = 6u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
                { SVC_ID_SERIAL,     PD_CNODE_SLOT_SERIAL_EP     },
                { SVC_ID_NET_VIRT,   PD_CNODE_SLOT_NET_VIRT_EP },
                { SVC_ID_BLK_VIRT,   PD_CNODE_SLOT_BLK_VIRT_EP },
                { SVC_ID_SERIAL_VIRT, PD_CNODE_SLOT_SERIAL_VIRT_EP },
            },
            .irq_count = 0u,
            .irqs = { },
            .mr_count = 1u,
            .memory_regions = {
                { .vaddr    = AOS_SECONDARY_GUEST_RAM_BASE,
                  .size     = AOS_SECONDARY_GUEST_RAM_SIZE,
                  .writable = 1u,
                  .name     = "guest_ram" },
            },
#else
            .name           = "guest_vmm_primary",
            .elf_path       = "guest_vmm_primary.elf",
            .stack_size     = 0x10000u,
            .cnode_size_bits = 10u,  /* 1024 slots — IRQ handler caps + microkit layout */
            .priority       = 250u,
            .self_svc_id    = SVC_ID_GUEST_VMM_PRIMARY,
            /* No net_pd or virtio_blk EP: the VMM reaches the network only
             * through net_virt and block media only through blk_virt
             * (docs/TCB.md invariant 2). */
            .init_ep_count  = 6u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
                { SVC_ID_SERIAL,     PD_CNODE_SLOT_SERIAL_EP     },
                { SVC_ID_NET_VIRT,   PD_CNODE_SLOT_NET_VIRT_EP },
                { SVC_ID_BLK_VIRT,   PD_CNODE_SLOT_BLK_VIRT_EP },
                { SVC_ID_SERIAL_VIRT, PD_CNODE_SLOT_SERIAL_VIRT_EP },
            },
            .irq_count = 0u,
            .irqs = { },
            /* Net queues are a root-provisioned frame shared with net_virt (and
             * net_pd's slots live in the same frame above NET_SVC_SLOT_BASE). */
            .mr_count = 1u,
            .memory_regions = {
                { .vaddr    = AOS_PRIMARY_GUEST_RAM_BASE,
                  .size     = AOS_PRIMARY_GUEST_RAM_SIZE,
                  .writable = 1u,
                  .name     = "guest_ram" },
            },
#endif
        },

#if defined(AGENTOS_GUEST_DUAL)
        /* pd[10] — secondary VMM in dual-profile images.
         *
         * Both profiles retain the conventional 0x40000000 guest GPA while
         * their VMMs use non-overlapping host virtual windows.
         */
        {
            .name           = "guest_vmm_secondary",
            .elf_path       = "guest_vmm_secondary.elf",
            .stack_size     = 0x10000u,
            .cnode_size_bits = 10u,
            .priority       = 250u,
            .self_svc_id    = SVC_ID_GUEST_VMM_SECONDARY,
            /* No net_pd or virtio_blk EP: the VMM reaches the network only
             * through net_virt and block media only through blk_virt
             * (docs/TCB.md invariant 2). */
            .init_ep_count  = 6u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
                { SVC_ID_SERIAL,     PD_CNODE_SLOT_SERIAL_EP     },
                { SVC_ID_NET_VIRT,   PD_CNODE_SLOT_NET_VIRT_EP },
                { SVC_ID_BLK_VIRT,   PD_CNODE_SLOT_BLK_VIRT_EP },
                { SVC_ID_SERIAL_VIRT, PD_CNODE_SLOT_SERIAL_VIRT_EP },
            },
            .irq_count = 0u,
            .irqs = { },
            .mr_count = 1u,
            .memory_regions = {
                { .vaddr    = AOS_SECONDARY_GUEST_RAM_BASE,
                  .size     = AOS_SECONDARY_GUEST_RAM_SIZE,
                  .writable = 1u,
                  .name     = "guest_ram" },
            },
        },

#endif

        /* pd[10/11] — vm_manager (prio 170; multi-VM lifecycle manager)
         * Guest-control calls arrive directly from cc_pd (164).
         * Keep this service above CC and below the VMMs (250). */
        {
            .name           = "vm_manager",
            .elf_path       = "vm_manager.elf",
            .stack_size     = 0x8000u,
            .cnode_size_bits = 10u,
            .priority       = 170u,
            .self_svc_id    = SVC_ID_VM_MANAGER,
            .init_ep_count  = AOS_VM_MANAGER_INIT_EP_COUNT,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
#if defined(AGENTOS_GUEST_PRIMARY)
                { SVC_ID_GUEST_VMM_PRIMARY,  PD_CNODE_SLOT_GUEST_VMM_PRIMARY_EP  },
#endif
#if defined(AGENTOS_GUEST_SECONDARY)
                { SVC_ID_GUEST_VMM_SECONDARY, PD_CNODE_SLOT_GUEST_VMM_SECONDARY_EP },
#endif
            },
        },

        /* pd[11/12] — cc_pd (prio 164; command-and-control relay)
         * Pure IPC relay: receives MSG_CC_* from external callers and routes
         * each to the appropriate service PD.  Passive — woken by PPC.
         * Priority 164: above guest vCPUs (150) and active device services
         * (160), below vm_manager (170) and the VMMs.
         * It is the lowest-priority PD in the image, so it reaches its poll
         * loop only once every other PD has blocked; that is where the
         * "agentOS boot complete" harness marker is printed. */
        {
            .name           = "cc_pd",
            .elf_path       = "cc_pd.elf",
            .stack_size     = 0x4000u,
            .cnode_size_bits = 10u,
            .priority       = 164u,
            .self_svc_id    = SVC_ID_CC_PD,
            .irq_count      = 1u,
            .irqs = {
                { .irq_number = 50u, .ntfn_badge = 1u, .name = "virtio-serial" },
            },
            .init_ep_count  = AOS_CC_INIT_EP_COUNT
#ifdef AGENTOS_NATIVE_RUST_TEST
                + 1u
#endif
                ,
            .init_eps = {
                { SVC_ID_NAMESERVER,  PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,   PD_CNODE_SLOT_LOG_DRAIN_EP  },
                { SVC_ID_SERIAL,      PD_CNODE_SLOT_SERIAL_EP     },
#if defined(AGENTOS_GUEST_SECONDARY) && !defined(AGENTOS_GUEST_DUAL)
                { SVC_ID_GUEST_VMM_SECONDARY, PD_CNODE_SLOT_GUEST_VMM_EP },
#else
                { SVC_ID_GUEST_VMM_PRIMARY,   PD_CNODE_SLOT_GUEST_VMM_EP },
#endif
                { SVC_ID_VM_MANAGER,  PD_CNODE_SLOT_VM_MANAGER_EP  },
                { SVC_ID_SERIAL_VIRT, PD_CNODE_SLOT_SERIAL_VIRT_EP },
                /* No controller EP: the controller PD is not in the image and
                 * an EP with no server would block cc_pd forever. */
#if defined(AGENTOS_FAULT_INJECT)
                { SVC_ID_FAULT_INJECT, PD_CNODE_SLOT_FAULT_INJECT_EP },
#endif
#ifdef AGENTOS_NATIVE_RUST_TEST
                { SVC_ID_NATIVE_RUST_PROBE, NATIVE_RUST_CC_ENDPOINT },
#endif
            },
        },

        /* Optional fault-injection PD used by CI through the CC-PD relay. */
#if defined(AGENTOS_FAULT_INJECT)
        {
            .name           = "fault_inject",
            .elf_path       = "fault_inject.elf",
            .stack_size     = 0x4000u,
            .cnode_size_bits = 10u,
            .priority       = 254u,
            .self_svc_id    = SVC_ID_FAULT_INJECT,
            .init_ep_count  = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },
#endif

        /* pd[12..14] — fault_handler (prio 255; highest priority for fault recovery)
         * Must preempt every other PD to handle seL4 fault IPC promptly.
         * No self_svc_id: receives fault IPC via TCB fault endpoint, not a
         * registered service endpoint. */
        {
            .name           = "fault_handler",
            .elf_path       = "fault_handler.elf",
            .stack_size     = 0x4000u,
            .cnode_size_bits = 10u,
            .priority       = 255u,
            .self_svc_id    = 0u,
            .init_ep_count  = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

#ifdef AGENTOS_GUEST_INPUT
        {
            .name = "input_virt",
            .elf_path = "input_virt.elf",
            .stack_size = 0x4000u,
            .cnode_size_bits = 8u,
            .priority = 215u,
            .self_svc_id = SVC_ID_INPUT_VIRT,
        },
#endif
#if defined(AGENTOS_FRAMEBUFFER_TEST) || defined(AGENTOS_GUEST_GRAPHICS)
        {
            .name = "framebuffer_queue",
            .elf_path = "framebuffer_queue.elf",
            .stack_size = 0x4000u,
            .cnode_size_bits = 8u,
            .priority = 215u,
            .self_svc_id = SVC_ID_FRAMEBUFFER_QUEUE,
#ifdef AGENTOS_DISPLAY_RAMFB
            .init_ep_count = 1u,
            .init_eps = {{ SVC_ID_SERIAL, PD_CNODE_SLOT_SERIAL_EP }},
#endif
        },
#ifdef AGENTOS_DISPLAY_RAMFB
        {
            .name = "display_ramfb", .elf_path = "display_ramfb.elf",
            .stack_size = 0x4000u, .cnode_size_bits = 8u,
            .priority = 216u, .self_svc_id = SVC_ID_DISPLAY_RAMFB,
            .init_ep_count = 1u,
            .init_eps = {{ SVC_ID_SERIAL, PD_CNODE_SLOT_SERIAL_EP }},
        },
#endif
#endif
#ifdef AGENTOS_FRAMEBUFFER_TEST
        {
            .name = "framebuffer_client0",
            .elf_path = "framebuffer_client0.elf",
            .stack_size = 0x4000u,
            .cnode_size_bits = 8u,
            .priority = 214u,
            .self_svc_id = SVC_ID_FRAMEBUFFER_TEST0,
            .init_ep_count = 1u,
            .init_eps = {{ SVC_ID_SERIAL, PD_CNODE_SLOT_SERIAL_EP }},
        },
        {
            .name = "framebuffer_client1",
            .elf_path = "framebuffer_client1.elf",
            .stack_size = 0x4000u,
            .cnode_size_bits = 8u,
            .priority = 214u,
            .self_svc_id = SVC_ID_FRAMEBUFFER_TEST1,
            .init_ep_count = 1u,
            .init_eps = {{ SVC_ID_SERIAL, PD_CNODE_SLOT_SERIAL_EP }},
        },
#endif

#ifdef AGENTOS_NATIVE_RUST_TEST
        /* No device frames, IRQs or guest capabilities in either test PD. */
        {
            .name = "native_rust_probe",
            .elf_path = "native_rust_probe.elf",
            .stack_size = 0x4000u,
            .cnode_size_bits = 8u,
            .priority = 245u,
            .self_svc_id = SVC_ID_NATIVE_RUST_PROBE,
            .init_ep_count = 1u,
            .init_eps = {
                { SVC_ID_NET_VIRT, PD_CNODE_SLOT_NET_VIRT_EP },
            },
        },
        {
            .name = "native_rust_client",
            .elf_path = "native_rust_client.elf",
            .stack_size = 0x4000u,
            .cnode_size_bits = 8u,
            .priority = 250u,
            .init_ep_count = 2u,
            .init_eps = {
                { SVC_ID_SERIAL, PD_CNODE_SLOT_SERIAL_EP },
                { SVC_ID_NATIVE_RUST_PROBE, 16u },
            },
        },
#endif

#ifdef AGENTOS_SEL4_TEST_IMAGE
        /* event_bus (prio 195) — test image only.  Not TCB; it is spawned
         * here solely so the contract runner below can exercise the EventBus
         * contract against a live server (an EP with no server would block
         * the runner forever). */
        {
            .name           = "event_bus",
            .elf_path       = "event_bus.elf",
            .stack_size     = 0x4000u,
            .cnode_size_bits = 10u,
            .priority       = 195u,
            .self_svc_id    = SVC_ID_EVENTBUS,
            .init_ep_count  = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* agentos-8f5 / agentos-0h4: on-target contract TAP runner.  Test image
         * only.  Pure client PD: it issues microkit_ppcall(ch) — i.e.
         * seL4_Call(BASE_ENDPOINT_CAP[74] + ch) — to the live service PDs, so it
         * must hold each service's listen endpoint at CNode slot 74+ch:
         *   EventBus   ch=MONITOR_CH_EVENTBUS(1)  -> slot 75
         *   serial_pd  ch=CH_SERIAL_PD(44)        -> slot 118
         *   log_drain  ch=CH_LOG_DRAIN(55)        -> slot 129
         * cnode_size_bits=9 (512 slots) covers those high slot indices. */
        {
            .name           = "test_runner",
            .elf_path       = "test_runner.elf",
            .stack_size     = 0x4000u,
            .cnode_size_bits = 9u,
            /* High priority (just below fault_handler at 255) so a service that
             * busy-polls — e.g. cc_pd at 160 spinning on its virtio-serial ring
             * when no virtio device is attached in the test QEMU — cannot starve
             * the runner.  Each runner PPC blocks on the target service's Recv,
             * letting the lower-priority services run to answer; ordering vs the
             * controller does not matter since the eventbus assertions tolerate
             * the ring-unmapped state (agentos-gom). */
            .priority       = 250u,
            .self_svc_id    = 0u,
            .init_ep_count  = 4u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_EVENTBUS,   75u  },   /* 74 + MONITOR_CH_EVENTBUS(1) */
                { SVC_ID_SERIAL,     118u },   /* 74 + CH_SERIAL_PD(44)       */
                { SVC_ID_LOG_DRAIN,  129u },   /* 74 + CH_LOG_DRAIN(55)       */
            },
        },
#endif
    },
};
