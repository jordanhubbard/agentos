/* tools/ld/agentos.ld — agentOS PD linker script
 *
 * Replaces microkit.ld from the Microkit SDK.
 * Used by all Protection Domain ELFs compiled by kernel/agentos-root-task/Makefile.
 *
 * Load address: 0x400000 (4 MB) — avoids conflict with the seL4 kernel image
 * which is typically loaded below 0x200000 on RISC-V and AArch64 QEMU targets.
 * Override via -Ttext=<addr> on the ld.lld command line for boards that require
 * a different PD load address.
 */

SECTIONS {
    . = 0x400000;

    .text : {
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    .data : {
        *(.data .data.*)
    }

    .bss : {
        *(.bss .bss.*)
        COMMON
    }

    . = ALIGN(4096);
    _end = .;
}
