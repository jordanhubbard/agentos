/* tools/ld/root_task.ld — agentOS root task linker script (AArch64)
 *
 * The root task is the first user-mode process started by the seL4 kernel.
 * On AArch64 QEMU virt, it must be placed in DRAM, not flash.  The loader
 * (kernel/loader/) copies the root_task ELF LOAD segments to their physical
 * addresses before enabling the MMU.
 *
 * Load address 0x68000000:
 *   - Within seL4 avail_p_regs: 0x60000000..0xC0000000
 *   - Above the seL4 kernel end (~0x6022e000)
 *   - Does not overlap the loader (0x44000000) or the image blob (0x48000000)
 *
 * The .pd_bundle section is initially empty (filled by objcopy after the build).
 * root_task reads PD ELFs from this embedded bundle instead of extra BootInfo.
 *
 * NOTE: This script is only used for the AArch64 target.  RISC-V and x86_64
 * builds continue to use tools/ld/agentos.ld (load address 0x400000).
 */

ENTRY(_start)

SECTIONS {
    . = 0x68000000;   /* must be within seL4 avail_p_regs: 0x60000000..0xC0000000 */

    .text : {
        KEEP(*(.text.start))    /* _start / CRT entry must be first */
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    /* Global offset table and PLT (position-independent references) */
    .got : {
        *(.got)
        .got.plt
    }

    /* Ensure read-only data is on its own page before writable sections */
    . = ALIGN(0x1000);
    .relro_padding : { . = ALIGN(0x1000); }

    .data : {
        *(.data .data.*)
    }

    .bss (NOLOAD) : {
        *(.bss .bss.*)
        COMMON
        /*
         * Root task stack — 64 KB, PAGE-aligned.
         * Aligning to 0x1000 here ensures __stack_top (= __stack_bottom +
         * 64 KB) is also page-aligned.  seL4 computes ipcbuf_vptr =
         * ui_v_reg.end, where ui_v_reg.end = highest PT_LOAD p_vaddr+p_memsz.
         * If __stack_top is not page-aligned, ui_v_reg.end is unaligned and
         * seL4 maps the IPC buffer frame at floor(ui_v_reg.end, PAGE) — which
         * collides with the ELF's last stack page.  The subsequent ELF load
         * overwrites that mapping, leaving the TCB's IPC buffer PA inaccessible
         * from any user VA, and every seL4 call with extra caps fails with
         * "Destination cap invalid or read-only".  Page-aligning the stack
         * start guarantees ui_v_reg.end is page-aligned so the IPC buffer
         * gets its own unmapped page.
         */
        . = ALIGN(0x1000);
        __stack_bottom = .;
        . += 0x10000;   /* 64 KB */
        __stack_top = .;
    }

    . = ALIGN(0x1000);

    /* PD bundle: embedded PD ELF data.
     *
     * The bundle is produced by `xtask gen-pd-bundle` and converted to a
     * linkable AArch64 ELF object by objcopy (see kernel/agentos-root-task/Makefile).
     * The objcopy object contributes .pd_bundle and defines:
     *   __pd_bundle_start  — start of bundle data
     *   __pd_bundle_end    — one past end of bundle data
     *
     * We place .pd_bundle in a named output section so it lands at a known,
     * DRAM-resident address.  The symbol assignments here are left out
     * intentionally — the objcopy object provides __pd_bundle_start and
     * __pd_bundle_end directly.
     */
    /*
     * .pd_bundle must end on a page boundary so seL4's ui_v_reg.end
     * (= highest PT_LOAD p_vaddr+p_memsz) is page-aligned.  seL4 places
     * the IPC buffer frame at ipcbuf_vptr = ui_v_reg.end; if that is not
     * page-aligned the IPC buffer frame maps at floor(ui_v_reg.end, PAGE)
     * which collides with the ELF's last page, and the subsequent ELF load
     * overwrites the mapping — breaking every seL4 IPC call.
     * The ALIGN inside the section forces padding that extends the PT_LOAD
     * memsz to the next page, ensuring a clean IPC buffer placement.
     */
    .pd_bundle : {
        KEEP(*(.pd_bundle))
        . = ALIGN(0x1000);
    }

    _end = .;
}
