/* tools/ld/root_task_riscv64.ld — agentOS root task linker script (RISC-V 64)
 *
 * The root task is the first user-mode process started by the seL4 kernel.
 * On QEMU virt riscv64 it must live in DRAM.  kernel/loader/ copies the
 * root_task ELF LOAD segments to their physical addresses before enabling
 * Sv39 paging and jumping to the kernel.
 *
 * Load address 0x90000000:
 *   - Within the kernel's available physical memory
 *     (platform_gen.json: 0x80200000..0x100000000)
 *   - Above the seL4 kernel image (0x80200000..~0x80214000)
 *   - Clear of the loader (0x81000000) and the agentos.img blob (0x88000000)
 *
 * Virtual address == physical address, so the loader passes pv_offset = 0,
 * exactly as on AArch64.  This requires -mcmodel=medany (see
 * kernel/agentos-root-task/Makefile): RISC-V's default medlow model can only
 * address the low 2 GiB, and every DRAM address on this board is above it.
 *
 * RISC-V embeds the PD bundle and signed boot manifest the same way
 * AArch64 does: objcopy turns xtask gen-pd-bundle's output into relocatable
 * objects that this script places in .pd_bundle / .pd_manifest.
 */

ENTRY(_start)

SECTIONS {
    . = 0x90000000;

    .text : {
        KEEP(*(.text.start))    /* _start from start_riscv64.S must be first */
        *(.text .text.*)
    }

    .rodata : {
        *(.srodata .srodata.*)
        *(.rodata .rodata.*)
    }

    /* Global offset table and PLT (position-independent references) */
    .got : {
        *(.got)
        *(.got.plt)
    }

    /* Ensure read-only data is on its own page before writable sections */
    . = ALIGN(0x1000);
    .relro_padding : { . = ALIGN(0x1000); }

    .data : {
        *(.sdata .sdata.*)
        *(.data .data.*)
    }

    .bss (NOLOAD) : {
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        COMMON
        /*
         * Root task stack — 64 KB, PAGE-aligned.  seL4 computes
         * ipcbuf_vptr = ui_v_reg.end (= highest PT_LOAD p_vaddr+p_memsz).
         * If that is not page-aligned the IPC buffer frame lands on the
         * ELF's last page and every seL4 call with extra caps fails.  See
         * tools/ld/root_task.ld for the full write-up.
         */
        . = ALIGN(0x1000);
        __stack_bottom = .;
        . += 0x10000;   /* 64 KB */
        __stack_top = .;
    }

    . = ALIGN(0x1000);

    /* PD bundle: embedded PD ELF data, and the signed boot manifest.
     *
     * Both are produced by `xtask gen-pd-bundle` and converted to linkable
     * RISC-V ELF objects by objcopy (see kernel/agentos-root-task/Makefile).
     * Those objects define __pd_bundle_start/__pd_bundle_end and
     * __pd_manifest_start/__pd_manifest_end directly, so this script must
     * NOT assign them: a linker-script assignment would override the
     * objcopy symbols and make bundle_size() read a bogus length.
     *
     * The trailing ALIGN(0x1000) in each section keeps the final PT_LOAD
     * memsz page-aligned, so seL4's ipcbuf_vptr = ui_v_reg.end lands on
     * its own page (same reasoning as the stack alignment above).
     */
    .pd_bundle : {
        KEEP(*(.pd_bundle))
        . = ALIGN(0x1000);
    }

    .pd_manifest : {
        KEEP(*(.pd_manifest))
        . = ALIGN(0x1000);
    }

    . = ALIGN(0x1000);
    _end = .;
}
