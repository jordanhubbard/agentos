/*
 * system_desc_riscv64.c — compile-time system topology for RISC-V 64 / QEMU virt
 *
 * Defines the system_desc_riscv64 constant used by the root task boot sequence
 * on RISC-V 64 targets (qemu_virt_riscv64 and compatible).
 *
 * PD ordering is critical: the nameserver MUST be first so that subsequent
 * PDs can register themselves with it before any inter-PD communication.
 *
 * This table is the complete set of PDs the root task spawns on riscv64, and
 * boards/qemu-riscv64/agentos.toml must list exactly these names and nothing
 * else: a name here with no bundle entry prints "[rt] pd elf <name> NOT FOUND"
 * and refuses boot, and a bundle entry with no row here is never started.
 * The two files are rewritten together, always.
 *
 * ── What this table is modelled on ───────────────────────────────────────────
 *
 * src/system_desc_aarch64.c, entry for entry: a driver PD owns exactly one
 * device class and a virtualizer PD is the only mux for that class
 * (docs/TCB.md invariants 1 and 2).  Until the arch-parity work this file was
 * the pre-virtualizer topology plus HURD-era service PDs (event_bus, irq_pd,
 * timer_pd, controller, init_agent, agentfs, vibe_engine, vfs_server,
 * net_server, framebuffer_pd) and had no virtualizer at all.  Those are the
 * museum PDs docs/TCB.md lists and CLAUDE.md forbids re-adding; they are gone,
 * and with them the five post-boot PD faults they caused (each was a museum PD
 * dereferencing a hardcoded AArch64 address or a NULL service pointer).
 *
 * ── What riscv64 deliberately does NOT have, and why ─────────────────────────
 *
 * serial_pd / serial_virt / operator_session.  services/serial-mux/serial_pd.c
 * is an ARM PL011 driver (pl011_init / pl011_putc, PL011 register offsets);
 * QEMU virt RISC-V has an NS16550A at 0x10000000.  Handing serial_pd that frame
 * would make it program the wrong register file, and because the hand-over in
 * main.c unmaps the root task's own console mapping it would also take the boot
 * log — and with it the only automated PD-count proof this architecture has —
 * off the console.  The root task therefore keeps the NS16550A console on
 * riscv64 (dbg_puts' g_uart_thr/g_uart_lsr path).  A serial_virt muxing a
 * driver that does not exist, with no frontend to serve, would be exactly the
 * contract-with-no-caller CLAUDE.md calls "not an API".  Porting serial_pd to
 * NS16550A is the next piece of riscv64 device work; see docs/TCB.md.
 *
 * cc_pd / vm_manager / guest_vmm_*.  riscv64 runs no guest operating system —
 * upstream seL4 has no RISC-V hypervisor extension (docs/TCB.md, "RISC-V and
 * guest operating systems").  There is no VM to manage and no VMM to relay to,
 * and cc_pd's transport is a virtio-serial device bound to the AArch64 board's
 * virtio-mmio slot 2.
 *
 * ── Device map (QEMU virt RISC-V, -machine virt) ─────────────────────────────
 *
 * The eight virtio-mmio transports live at 0x10001000 + N*0x1000, one 4 KiB
 * page each, with PLIC IRQ 1+N; QEMU names them virtio-mmio-bus.N in that
 * order, and xtask binds the test devices to fixed buses so the addresses
 * below are not left to QEMU's attach order:
 *
 *   0x10002000  virtio-mmio-bus.1  host virtio-net   → net_pd
 *   0x10003000  virtio-mmio-bus.2  host virtio-blk   → virtio_blk
 *   0x10000000  NS16550A UART                        → root task (see above)
 *
 * virtio-mmio-bus.0 is left unattached, as on every other machine here: no
 * QEMU launch plan in this repository binds a device to it (TCB invariant 5,
 * enforced by tests/platform/lint_source_invariants.c).
 *
 * Neither driver carries a device_frames[] row, exactly as on AArch64: the
 * root task retypes those frames once into its own CSpace and maps a copy
 * into the owning driver's VSpace (main.c, AGENTOS_HOST_{BLK,NET}_MMIO_PA).
 * Both poll; neither takes an IRQ in this image, so irq_count stays 0 and
 * `make test-authority`'s "virtualizers hold no IRQ handler" invariant is
 * vacuously true here.
 *
 * ── Priority DAG ─────────────────────────────────────────────────────────────
 *
 * Same principle and the same numbers as AArch64: a service that is called by
 * others runs at HIGHER priority than its callers, so a blocking IPC Call can
 * never be stuck behind its own server.
 *
 *   255  fault_handler   — fault delivery must preempt everything
 *   245  nameserver      — foundation: every PD resolves caps through it
 *   235  log_drain       — nearly every PD logs
 *   215  virtio_blk      — host block driver; blk_virt calls into it
 *   213  block_pd        — OS-neutral block contract
 *   210  blk_virt        — block virtualizer: the only blk mux
 *   207  net_pd          — host virtio-net driver; net_virt calls into it
 *   205  net_virt        — network virtualizer: the only net mux
 *   166  entropy_pd      — polls; no device on this machine (see below)
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "system_desc.h"

/* Nine PDs, unconditionally: riscv64 has no guest, test or fault-injection
 * image variants, so unlike AArch64 there is nothing to add here under an
 * #ifdef.  xtask's --assert-riscv64 proof asserts exactly this number. */
#define AOS_RISCV64_PD_COUNT 9u

/* ── RISC-V 64 system description ─────────────────────────────────────────── */

const system_desc_t system_desc_riscv64 = {
    .pd_count = AOS_RISCV64_PD_COUNT,
    .pds = {

        /* pd[0] — nameserver (MUST be first; prio 245)
         * Foundation service: every PD resolves endpoint caps through it. */
        {
            .name            = "nameserver",
            .elf_path        = "nameserver.elf",
            .stack_size      = 0x4000u,
            .cnode_size_bits = 10u,
            .priority        = 245u,
            .self_svc_id     = SVC_ID_NAMESERVER,
            .init_ep_count   = 0u,
            .init_eps        = {},
        },

        /* pd[1] — log_drain (prio 235)
         * No SVC_ID_SERIAL init EP, unlike AArch64: riscv64 has no serial
         * driver PD, so an endpoint for it would have no server and the
         * first log Call would block forever. */
        {
            .name            = "log_drain",
            .elf_path        = "log_drain.elf",
            .stack_size      = 0x4000u,
            .cnode_size_bits = 10u,
            .priority        = 235u,
            .self_svc_id     = SVC_ID_LOG_DRAIN,
            .init_ep_count   = 1u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
            },
        },

        /* pd[2] — virtio_blk (prio 215; host block device driver)
         * Owns the host virtio-mmio block transport (bus.2, 0x10003000);
         * blk_virt relays every block request to it. */
        {
            .name            = "virtio_blk",
            .elf_path        = "virtio_blk.elf",
            .stack_size      = 0x4000u,
            .cnode_size_bits = 10u,
            .priority        = 215u,
            .self_svc_id     = SVC_ID_VIRTIO_BLK,
            .init_ep_count   = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[3] — block_pd (prio 213; OS-neutral block API) */
        {
            .name            = "block_pd",
            .elf_path        = "block_pd.elf",
            .stack_size      = 0x4000u,
            .cnode_size_bits = 10u,
            .priority        = 213u,
            .self_svc_id     = SVC_ID_BLOCK_SERVICE,
            .init_ep_count   = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },

        /* pd[4] — blk_virt (prio 210; block virtualizer, the only blk mux)
         * Owns no device frame and no IRQ: docs/TCB.md invariant 1 keeps those
         * with virtio_blk.  cnode_size_bits must stay 10 — root installs the
         * queue-reconstruction authority at AOS_QUEUE_SERVICE_CNODE_BITS. */
        {
            .name            = "blk_virt",
            .elf_path        = "blk_virt.elf",
            .stack_size      = 0x8000u,
            .cnode_size_bits = 10u,
            .priority        = 210u,
            .self_svc_id     = SVC_ID_BLK_VIRT,
            .init_ep_count   = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_VIRTIO_BLK, PD_CNODE_SLOT_VIRTIO_BLK_EP },
            },
            .irq_count = 0u,
            .irqs = { },
            .device_frame_count = 0u,
            .device_frames = { },
        },

        /* pd[5] — net_pd (prio 207; host virtio-net driver)
         * Owns the host virtio-mmio network transport (bus.1, 0x10002000).
         * Its only client is net_virt, whose listen EP it holds so it can
         * NBSend RX_READY. */
        {
            .name            = "net_pd",
            .elf_path        = "net_pd.elf",
            .stack_size      = 0x8000u,
            .cnode_size_bits = 10u,
            .priority        = 207u,
            .self_svc_id     = SVC_ID_NET_PD,
            .init_ep_count   = 3u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
                { SVC_ID_NET_VIRT,   PD_CNODE_SLOT_NET_VIRT_EP   },
            },
            .irq_count = 0u,
            .irqs = { },
            .device_frame_count = 0u,
            .device_frames = { },
        },

        /* pd[6] — net_virt (prio 205; network virtualizer, the only net mux)
         * Owns no device frame and no IRQ.  With no VMM on this architecture
         * it has no guest client; it exists so that the one network mux is
         * the one the rest of the platform already speaks to, and so a native
         * client added later cannot reach net_pd directly. */
        {
            .name            = "net_virt",
            .elf_path        = "net_virt.elf",
            .stack_size      = 0x8000u,
            .cnode_size_bits = 10u,
            .priority        = 205u,
            .self_svc_id     = SVC_ID_NET_VIRT,
            .init_ep_count   = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_NET_PD,     PD_CNODE_SLOT_NET_PD_EP     },
            },
            .irq_count = 0u,
            .irqs = { },
            .device_frame_count = 0u,
            .device_frames = { },
        },

        /* pd[7] — entropy_pd (prio 166; virtio-rng driver)
         * Owns no device frame here, for the same reason as on AArch64: no
         * virtio-rng is attached, so the driver reports the device absent
         * (AOS_ENTROPY_ERR_UNAVAILABLE) instead of touching memory that is
         * not there.  Polls; no IRQ. */
        {
            .name            = "entropy_pd",
            .elf_path        = "entropy_pd.elf",
            .stack_size      = 0x4000u,
            .cnode_size_bits = 10u,
            .priority        = 166u,
            .self_svc_id     = SVC_ID_ENTROPY_PD,
            .init_ep_count   = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
            .irq_count = 0u,
        },

        /* pd[8] — fault_handler (prio 255; must preempt everything)
         * No self_svc_id: it receives fault IPC on a TCB fault endpoint, not
         * a registered service endpoint. */
        {
            .name            = "fault_handler",
            .elf_path        = "fault_handler.elf",
            .stack_size      = 0x4000u,
            .cnode_size_bits = 10u,
            .priority        = 255u,
            .self_svc_id     = 0u,
            .init_ep_count   = 2u,
            .init_eps = {
                { SVC_ID_NAMESERVER, PD_CNODE_SLOT_NAMESERVER_EP },
                { SVC_ID_LOG_DRAIN,  PD_CNODE_SLOT_LOG_DRAIN_EP  },
            },
        },
    },
};
