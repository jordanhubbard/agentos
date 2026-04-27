/* tools/ld/agentos.ld — agentOS PD linker script
 *
 * Replaces microkit.ld from the Microkit SDK.
 * Used by all Protection Domain ELFs compiled by kernel/agentos-root-task/Makefile.
 *
 * Load address: 0x400000 (4 MB) — avoids conflict with the seL4 kernel image
 * which is typically loaded below 0x200000 on RISC-V and AArch64 QEMU targets.
 * Override via -Ttext=<addr> on the ld.lld command line for boards that require
 * a different PD load address.
 *
 * __pd_bundle_start / __pd_bundle_end are defined here as zero-size symbols
 * so that the root task's main.c compiles and links on non-AArch64 targets.
 * On AArch64, tools/ld/root_task.ld is used instead and these symbols enclose
 * the real embedded PD ELF bundle.
 *
 * ENTRY(_start): the _start function is provided by pd_entry.c (compiled into
 * every service PD).  The root task uses its own entry via start_aarch64.S /
 * root_task.ld and does NOT link pd_entry.c.
 */

ENTRY(_start)

SECTIONS {
    . = 0x400000;

    .text : {
        *(.text.start)      /* _start from pd_entry.c must be first */
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

    /* Zero-size PD bundle placeholder — no bundle on non-AArch64 targets */
    __pd_bundle_start = .;
    .pd_bundle : { KEEP(*(.pd_bundle)) }
    __pd_bundle_end = .;

    _end = .;
}
