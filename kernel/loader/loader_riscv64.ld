/* loader_riscv64.ld — linker script for the agentOS RISC-V pre-seL4 loader
 *
 * QEMU loads this ELF via -kernel; OpenSBI's fw_dynamic next_addr is taken
 * from its ELF entry point, so it may live anywhere in DRAM above the
 * firmware reservation (0x80000000..0x80200000).
 *
 * It must not collide with:
 *   - the seL4 kernel      (phys 0x80200000..~0x80214000)
 *   - the agentos.img blob (phys 0x88000000)
 *   - the root_task        (phys 0x90000000)
 *
 * 0x81000000 satisfies all of those.  The stack top is 0x81040000 (256 KB
 * above the loader base); see start_riscv64.S.
 */

ENTRY(_start)

SECTIONS {
    . = 0x81000000;

    .text.start : {
        KEEP(*(.text.start))   /* _start must be first */
    }

    .text : {
        *(.text .text.*)
    }

    .rodata : {
        *(.srodata .srodata.*)
        *(.rodata .rodata.*)
    }

    .got : {
        *(.got)
        *(.got.plt)
    }

    . = ALIGN(0x1000);

    .data : {
        *(.sdata .sdata.*)
        *(.data .data.*)
    }

    . = ALIGN(0x1000);

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.sbss .sbss.*)
        *(.bss .bss.*)
        COMMON
        . = ALIGN(8);
        __bss_end = .;
    }

    . = ALIGN(0x1000);
    _end = .;
    ASSERT(_end <= 0x8103c000, "loader must leave at least 16 KiB for stack")
}
