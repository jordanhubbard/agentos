/* loader.ld — linker script for the agentOS AArch64 pre-seL4 loader
 *
 * The loader is a standalone ELF that QEMU loads via its "loader" device.
 * It must reside in DRAM outside the regions used by:
 *   - the seL4 kernel      (phys 0x60000000)
 *   - the root_task        (phys 0x41000000)
 *   - the agentos.img blob (phys 0x48000000)
 *
 * 0x44000000 satisfies all constraints and is below the image blob.
 * The stack is placed at 0x44010000 (64 KB above the loader base).
 */

ENTRY(_start)

SECTIONS {
    . = 0x44000000;

    .text.start : {
        KEEP(*(.text.start))   /* _start must be first */
    }

    .text : {
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    /* GOT / PLT (position-independent references, if any) */
    .got : {
        *(.got)
        *(.got.plt)
    }

    . = ALIGN(0x1000);

    .data : {
        *(.data .data.*)
    }

    . = ALIGN(0x1000);

    .bss (NOLOAD) : {
        __bss_start = .;
        *(.bss .bss.*)
        COMMON
        . = ALIGN(8);
        __bss_end = .;
    }

    . = ALIGN(0x1000);
    _end = .;
}
