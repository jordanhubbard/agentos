/*
 * system_desc_x86_64.c - reduced smoke and opt-in VMX qualification topologies.
 *
 * The normal x86_64_generic board starts the five device-free foundation PDs
 * described above the `#else` branch below (it started none until this file
 * was changed, which is why its boot gate proved nothing).  The separate
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
#ifdef AGENTOS_GUEST_INPUT
        + 1u
#endif
#ifdef AGENTOS_GUEST_GRAPHICS
        + 1u
#endif
#ifdef AGENTOS_X86_DUAL_GUEST
        + 3u
#endif
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
#ifdef AGENTOS_GUEST_INPUT
        {
            .name = "input_virt",
            .elf_path = "input_virt.elf",
            .stack_size = 0x4000u,
            .cnode_size_bits = 10u,
            .priority = 215u,
            .self_svc_id = SVC_ID_INPUT_VIRT,
        },
#endif
#ifdef AGENTOS_GUEST_GRAPHICS
        {
            .name = "framebuffer_queue",
            .elf_path = "framebuffer_queue.elf",
            .stack_size = 0x4000u,
            .cnode_size_bits = 10u,
            .priority = 215u,
            .self_svc_id = SVC_ID_FRAMEBUFFER_QUEUE,
        },
#endif
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
#ifdef AGENTOS_X86_DUAL_GUEST
        {
            .name = "x86_secondary_runner",
            .elf_path = "x86_secondary_runner.elf",
            .stack_size = 0x8000u,
            .cnode_size_bits = 10u,
            .priority = 250u,
            .self_svc_id = SVC_ID_X86_SECONDARY_RUNNER,
        },
        {
            .name = "x86_secondary_runner_ap",
            .elf_path = "x86_secondary_runner_ap.elf",
            .stack_size = 0x8000u,
            .cnode_size_bits = 10u,
            .priority = 250u,
            .self_svc_id = SVC_ID_X86_SECONDARY_AP_RUNNER,
        },
        {
            .name = "guest_vmm_secondary",
            .elf_path = "guest_vmm_secondary.elf",
            .stack_size = 0x10000u,
            .cnode_size_bits = AOS_GUEST_RAM_CNODE_BITS,
            .priority = 250u,
            .self_svc_id = SVC_ID_GUEST_VMM_SECONDARY,
            .init_ep_count = 3u,
            .init_eps = {
                { SVC_ID_SERIAL_VIRT, PD_CNODE_SLOT_SERIAL_VIRT_EP },
                { SVC_ID_BLK_VIRT, PD_CNODE_SLOT_BLK_VIRT_EP },
                { SVC_ID_NET_VIRT, PD_CNODE_SLOT_NET_VIRT_EP },
            },
        },
#endif
        {
            .name = "vm_manager",
            .elf_path = "vm_manager.elf",
            .stack_size = 0x8000u,
            .cnode_size_bits = 10u,
            .priority = 220u,
            .self_svc_id = SVC_ID_VM_MANAGER,
#ifdef AGENTOS_X86_DUAL_GUEST
            .init_ep_count = 2u,
            .init_eps = {
                { SVC_ID_GUEST_VMM_PRIMARY, PD_CNODE_SLOT_GUEST_VMM_PRIMARY_EP },
                { SVC_ID_GUEST_VMM_SECONDARY, PD_CNODE_SLOT_GUEST_VMM_SECONDARY_EP },
            },
#else
            .init_ep_count = 1u,
            .init_eps = {{ SVC_ID_GUEST_VMM_PRIMARY, PD_CNODE_SLOT_GUEST_VMM_PRIMARY_EP }},
#endif
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

/* ── x86_64_generic: the device-free foundation set ──────────────────────────
 *
 * This table used to be `.pd_count = 0u, .pds = {}`, and the only CI check
 * that ever booted x86_64_generic asserted `[rt] boot complete`.  An image
 * that starts no protection domain at all reaches that marker, so the gate
 * proved that the root task ran and nothing else.  It is non-empty now, and
 * xtask asserts the exact count (X86_64_EXPECTED_PDS in xtask/src/cmd_test.rs,
 * the same discipline as RISCV64_EXPECTED_PDS and verify_inspect()'s 15).
 *
 * boards/qemu-x86_64/agentos.toml must list exactly these names and nothing
 * else: a name here with no bundle entry prints "[rt] pd elf <name> NOT FOUND"
 * and refuses boot, and a bundle entry with no row here is never started.
 * ***  REWRITE THAT FILE AND THIS TABLE IN LOCKSTEP.  ***
 *
 * ── Why these five and not the AArch64 fifteen ───────────────────────────────
 *
 * Same method as system_desc_riscv64.c: every PD whose driver backend exists
 * on this machine, and nothing whose contract would have no server.
 *
 * net_pd and virtio_blk are excluded because they CANNOT run here, and the
 * root task already says so.  On x86_64 the host virtio transports are PCI
 * functions, and PCI discovery (src/x86_host_pci.c, g_x86_net_frames /
 * g_x86_blk_frames) is compiled in only under AGENTOS_X86_FIRMWARE_RESET,
 * which x86_64_generic does not define — there are no BARs to map.  main.c's
 * net_pd arm for this configuration is literally
 *     dbg_puts("[rt] net_pd: no host NIC MMIO path on this target; not
 *               started\n"); continue;
 * and the virtio_blk host-MMIO arm is `#if defined(__aarch64__) ||
 * defined(__riscv)`.  Listing either here would produce a PD the root task
 * refuses to start, i.e. a descriptor that lies about the image.  The board
 * also attaches no virtio-blk and an e1000 rather than a virtio-net
 * (boards/qemu-x86_64/board.mk), so there is nothing for them to drive.
 *
 * blk_virt and net_virt are excluded for the reason riscv64 gives: a
 * virtualizer muxing a driver that does not exist, with no frontend to serve,
 * is the contract-with-no-caller CLAUDE.md calls "not an API".
 *
 * serial_pd is excluded because on this board it is an ARM PL011 driver.
 * kernel/agentos-root-task/Makefile builds serial_pd.o from
 * services/serial-mux/serial_x86.c only when X86_FIRMWARE_RESET=1; otherwise
 * it builds services/serial-mux/serial_pd.c, which is pl011_init /
 * pl011_putc against PL011 register offsets.  QEMU q35 has an NS16550 COM1 at
 * I/O port 0x3F8, which the root task owns and writes directly (board.mk,
 * BOARD_UART_TYPE := ns16550) — and which carries the boot log this gate
 * counts PDs from.  serial_virt would then be muxing that absent driver.
 *
 * cc_pd / vm_manager / guest_vmm_* belong to the VTX branch above: they need
 * VMX/EPT, which x86_64_generic is not built for.
 *
 * ── What this set does prove ─────────────────────────────────────────────────
 *
 * That on x86_64 the root task verifies the signed PD manifest and then, five
 * times over, retypes a TCB/CNode/VSpace, loads a PD ELF out of the embedded
 * bundle, maps its image and IPC buffer, mints the initial endpoint caps,
 * binds a scheduling context and starts the thread — and that the five PDs
 * then run without faulting.  It is not an I/O claim: no PD here owns a
 * device frame or an IRQ on this board.  x86_64 guest I/O lives on
 * x86_64_generic_vtx; see docs/TCB.md for what is and is not automated.
 *
 * ── Priority DAG ─────────────────────────────────────────────────────────────
 *
 * Same numbers as AArch64 and riscv64; a callee outranks its callers so a
 * blocking IPC Call can never be stuck behind its own server.
 *
 *   255  fault_handler   — fault delivery must preempt everything
 *   245  nameserver      — foundation: every PD resolves caps through it
 *   235  log_drain       — nearly every PD logs
 *   213  block_pd        — OS-neutral block contract
 *   166  entropy_pd      — polls; no device on this machine
 */

/* Five PDs, unconditionally.  The x86_64_generic image has no guest, test or
 * fault-injection variants — every one of those is a VTX board — so unlike
 * AArch64 there is nothing to add here under an #ifdef.  xtask's
 * X86_64_EXPECTED_PDS asserts exactly this number. */
#define AOS_X86_64_GENERIC_PD_COUNT 5u

const system_desc_t system_desc_x86_64 = {
    .pd_count = AOS_X86_64_GENERIC_PD_COUNT,
    .pds = {

        /* pd[0] — nameserver (MUST be first; prio 245) */
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
         * No SVC_ID_SERIAL init EP, for the same reason as riscv64: this
         * board starts no serial driver PD, so an endpoint for it would
         * have no server and the first log Call would block forever. */
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

        /* pd[2] — block_pd (prio 213; OS-neutral block API) */
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

        /* pd[3] — entropy_pd (prio 166; virtio-rng driver)
         * Owns no device frame here, as on riscv64: no virtio-rng is
         * attached, and the private queue frame is provisioned only on
         * AArch64 (main.c step 4g.4.6d), so entropy_svc reports the device
         * absent (AOS_ENTROPY_ERR_UNAVAILABLE) instead of touching memory
         * that is not there.  Polls; no IRQ. */
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

        /* pd[4] — fault_handler (prio 255; must preempt everything)
         * No self_svc_id: it receives fault IPC on a TCB fault endpoint, not
         * a registered service endpoint.  Root provisions its private fault
         * ring by name (provision_fault_ring() in main.c), which is
         * arch-blind, so x86_64 gets it with no x86-specific change. */
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
#endif
