/*
 * freebsd_vmm.c — FreeBSD guest VMM for agentOS
 *
 * Boots a FreeBSD/RISC-V guest as an seL4 process-in-PD.  Architecture:
 *
 *   RISC-V 64:  Process-in-PD model (no H extension in Microkit 2.1.0).
 *               FreeBSD/RISC-V executes at seL4 U-mode with SBI ecall
 *               emulation via fault_handler.elf.  The VMM loads the FreeBSD
 *               flat kernel Image, places a minimal FDT, and jumps to entry.
 *
 *   AArch64:    Reserved for future libvmm-based implementation.
 *               FreeBSD has EFI/EDK2 support on AArch64; a full VMM would
 *               use libvmm + EDK2 firmware (similar to the freebsd-vmm/ dir).
 *
 * FDT is built into a local static buffer using fdt_builder.  Guest RAM copy
 * requires the root task to have mapped 256 MB at FREEBSD_IMAGE_BASE into
 * this PD's VSpace; without that mapping the copy faults and fault_handler
 * catches it.  The full E2E path requires xtask gen-image to embed the
 * FreeBSD flat kernel as _guest_kernel_image.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stddef.h>
#include <stdint.h>
#include "sel4_boot.h"
#include "fdt_builder.h"

/* Raw debug output without IPC (works before nameserver is available) */
static inline void dbg_puts(const char *s)
{
    for (; *s; s++) {
        seL4_DebugPutChar(*s);
    }
}

/* ── Guest memory layout (must match FDT and kernel config) ──────────────── */

#define FREEBSD_IMAGE_BASE  0x80200000UL  /* FreeBSD kernel entry            */
#define FREEBSD_RAM_BASE    0x80000000UL
#define FREEBSD_RAM_SIZE    0x10000000UL  /* 256 MB                          */

/* QEMU virt RISC-V peripheral addresses (shared with linux_vmm layout) */
#define PLIC_BASE           0x0c000000UL
#define PLIC_SIZE           0x00600000UL
#define VIRTIO_NET_BASE     0x10001000UL  /* slot 0: virtio-net  */
#define VIRTIO_BLK_BASE     0x10002000UL  /* slot 1: virtio-blk  */
#define VIRTIO_MMIO_SIZE    0x00001000UL
#define VIRTIO_NET_IRQ      1u
#define VIRTIO_BLK_IRQ      2u

/*
 * Guest kernel image linked by package_guest_images.S.  Weak so freebsd_vmm.elf
 * links without an embedded kernel; _guest_kernel_image == NULL in that case.
 */
extern char _guest_kernel_image[]     __attribute__((weak));
extern char _guest_kernel_image_end[] __attribute__((weak));

/* Static FDT buffer — always accessible, within this PD's data segment. */
static uint8_t s_fdt_buf[4096] __attribute__((aligned(8)));

/* ── FDT builder ──────────────────────────────────────────────────────────── */

static size_t build_guest_fdt(void)
{
    fdt_ctx_t ctx;
    fdt_init(&ctx, s_fdt_buf, sizeof(s_fdt_buf));

    /* Root */
    fdt_begin_node(&ctx, "");
    fdt_prop_u32(&ctx, "#address-cells", 2u);
    fdt_prop_u32(&ctx, "#size-cells",    2u);
    fdt_prop_string(&ctx, "compatible",  "riscv-virtio");
    fdt_prop_string(&ctx, "model",       "riscv-virtio,qemu");

    /* /cpus */
    fdt_begin_node(&ctx, "cpus");
    fdt_prop_u32(&ctx, "#address-cells",     1u);
    fdt_prop_u32(&ctx, "#size-cells",        0u);
    fdt_prop_u32(&ctx, "timebase-frequency", 10000000u);  /* 10 MHz */

    fdt_begin_node(&ctx, "cpu@0");
    fdt_prop_string(&ctx, "device_type", "cpu");
    fdt_prop_string(&ctx, "compatible",  "riscv");
    fdt_prop_string(&ctx, "riscv,isa",   "rv64imafdc");
    fdt_prop_string(&ctx, "mmu-type",    "riscv,sv48");
    fdt_prop_u32(&ctx, "reg",            0u);
    fdt_prop_string(&ctx, "status",      "okay");

    /* INTC (phandle 1) */
    fdt_begin_node(&ctx, "interrupt-controller");
    fdt_prop_u32(&ctx, "#interrupt-cells", 1u);
    fdt_prop_string(&ctx, "compatible",    "riscv,cpu-intc");
    fdt_prop_u32_array(&ctx, "interrupt-controller", NULL, 0u);
    fdt_prop_u32(&ctx, "phandle",          1u);
    fdt_end_node(&ctx);

    fdt_end_node(&ctx);  /* cpu@0 */
    fdt_end_node(&ctx);  /* cpus */

    /* /memory@80000000 */
    fdt_begin_node(&ctx, "memory@80000000");
    fdt_prop_string(&ctx, "device_type", "memory");
    fdt_prop_reg64(&ctx, FREEBSD_RAM_BASE, FREEBSD_RAM_SIZE);
    fdt_end_node(&ctx);

    /* /soc — simple-bus with identity ranges */
    fdt_begin_node(&ctx, "soc");
    fdt_prop_u32(&ctx, "#address-cells",   2u);
    fdt_prop_u32(&ctx, "#size-cells",      2u);
    fdt_prop_u32(&ctx, "#interrupt-cells", 1u);
    fdt_prop_string(&ctx, "compatible",    "simple-bus");
    fdt_prop_u32_array(&ctx, "ranges",     NULL, 0u);

    /* PLIC (phandle 2) */
    fdt_begin_node(&ctx, "plic@c000000");
    fdt_prop_string(&ctx, "compatible",    "sifive,plic-1.0.0");
    fdt_prop_u32(&ctx, "#interrupt-cells", 1u);
    fdt_prop_u32(&ctx, "#address-cells",   0u);
    fdt_prop_u32_array(&ctx, "interrupt-controller", NULL, 0u);
    fdt_prop_reg64(&ctx, PLIC_BASE, PLIC_SIZE);
    fdt_prop_u32(&ctx, "riscv,ndev",       31u);
    {
        /* hart 0 M-EI (11) and S-EI (9) via INTC phandle 1 */
        uint32_t ix[4] = { 1u, 11u, 1u, 9u };
        fdt_prop_u32_array(&ctx, "interrupts-extended", ix, 4u);
    }
    fdt_prop_u32(&ctx, "phandle",          2u);
    fdt_end_node(&ctx);  /* plic */

    /* virtio-net (slot 0, IRQ 1) */
    fdt_begin_node(&ctx, "virtio_mmio@10001000");
    fdt_prop_string(&ctx, "compatible",    "virtio,mmio");
    fdt_prop_reg64(&ctx, VIRTIO_NET_BASE, VIRTIO_MMIO_SIZE);
    {
        uint32_t irq = VIRTIO_NET_IRQ;
        fdt_prop_u32_array(&ctx, "interrupts", &irq, 1u);
    }
    fdt_prop_u32(&ctx, "interrupt-parent", 2u);
    fdt_end_node(&ctx);

    /* virtio-blk (slot 1, IRQ 2) */
    fdt_begin_node(&ctx, "virtio_mmio@10002000");
    fdt_prop_string(&ctx, "compatible",    "virtio,mmio");
    fdt_prop_reg64(&ctx, VIRTIO_BLK_BASE, VIRTIO_MMIO_SIZE);
    {
        uint32_t irq = VIRTIO_BLK_IRQ;
        fdt_prop_u32_array(&ctx, "interrupts", &irq, 1u);
    }
    fdt_prop_u32(&ctx, "interrupt-parent", 2u);
    fdt_end_node(&ctx);

    fdt_end_node(&ctx);  /* soc */

    /* /chosen — FreeBSD loader expects loader.conf-style args, not Linux style */
    fdt_begin_node(&ctx, "chosen");
    fdt_prop_string(&ctx, "bootargs",
                    "vfs.root.mountfrom=ufs:/dev/vtbd0 kern.console=hvc0");
    fdt_end_node(&ctx);

    fdt_end_node(&ctx);  /* root */

    return fdt_finish(&ctx);
}

/* ── Kernel entry jump ────────────────────────────────────────────────────── */

static void __attribute__((noreturn))
jump_to_kernel(unsigned long entry, unsigned long hart_id, unsigned long dtb_va)
{
    register unsigned long a0 __asm__("a0") = hart_id;
    register unsigned long a1 __asm__("a1") = dtb_va;
    register unsigned long t0 __asm__("t0") = entry;
    __asm__ volatile (
        "jalr zero, 0(%0)"
        :
        : "r"(t0), "r"(a0), "r"(a1)
        : "memory"
    );
    __builtin_unreachable();
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

void freebsd_vmm_main(seL4_CPtr ep, seL4_CPtr ns_ep)
{
    (void)ns_ep;

    dbg_puts("[freebsd_vmm] RISC-V: process-in-PD VMM starting.\n");

    /* ── Build FDT ──────────────────────────────────────────────────────── */
    size_t fdt_sz = build_guest_fdt();
    if (fdt_sz == 0u) {
        dbg_puts("[freebsd_vmm] RISC-V: FDT build FAILED (buffer overflow).\n");
        while (1) { seL4_Word b; seL4_Wait(ep, &b); }
    }
    dbg_puts("[freebsd_vmm] RISC-V: FDT built OK.\n");

    /* ── Check for embedded kernel ──────────────────────────────────────── */
    if (!_guest_kernel_image || (_guest_kernel_image == _guest_kernel_image_end)) {
        dbg_puts("[freebsd_vmm] RISC-V: no guest kernel linked"
                 " (xtask gen-image step required).\n");
        dbg_puts("[freebsd_vmm] RISC-V: running as passive stub.\n");
        while (1) { seL4_Word b; seL4_Wait(ep, &b); }
    }

    size_t ksize = (size_t)(_guest_kernel_image_end - _guest_kernel_image);
    (void)ksize;

    /* ── Copy kernel to FREEBSD_IMAGE_BASE ──────────────────────────────── */
    {
        uint8_t       *dst = (uint8_t *)FREEBSD_IMAGE_BASE;
        const uint8_t *src = (const uint8_t *)_guest_kernel_image;
        for (size_t i = 0u; i < ksize; i++) dst[i] = src[i];
    }
    dbg_puts("[freebsd_vmm] RISC-V: kernel copied to 0x80200000.\n");

    /* ── Jump to kernel ─────────────────────────────────────────────────── */
    dbg_puts("[freebsd_vmm] RISC-V: jumping to kernel entry.\n");
    jump_to_kernel(FREEBSD_IMAGE_BASE, 0UL, (unsigned long)s_fdt_buf);
}
