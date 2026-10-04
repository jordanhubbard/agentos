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
 * RISC-V embeds the PD bundle and signed boot manifest in the same SECTIONS
 * as AArch64, but produces the objects differently.  AArch64 uses
 * `objcopy --input-target=binary`; that does NOT work here, because the
 * object objcopy emits carries no RISC-V float-ABI tag and ld.lld refuses to
 * link it against the soft-float root task.  RISC-V instead assembles
 * src/pd_bundle_riscv64.S, which .incbin's both blobs through the same
 * compiler driver and flags as every other root-task object, so the ABI tags
 * match by construction.
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
     * Both are produced by `xtask gen-pd-bundle` and embedded by
     * src/pd_bundle_riscv64.S (NOT by objcopy -- see the header comment and
     * kernel/agentos-root-task/Makefile).  That object defines
     * __pd_bundle_start/__pd_bundle_end and __pd_manifest_start/
     * __pd_manifest_end directly, so this script must NOT assign them: a
     * linker-script assignment would override the symbols the object defines
     * and make bundle_size() read a bogus length.
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
