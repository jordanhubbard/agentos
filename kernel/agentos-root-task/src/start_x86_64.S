/*
 * start_x86_64.S — root-task entry shim for seL4 x86_64
 *
 * seL4 enters the rootserver with the BootInfo pointer in rdi, but rsp is not
 * a usable C stack for this freestanding root task.  Install a small static
 * bootstrap stack, preserve rdi, and call the C entry point.
 */

.section .bss.rt_stack, "aw", @nobits
.balign 16
.global __rt_stack_x86_64
__rt_stack_x86_64:
    .skip 65536
.global __rt_stack_x86_64_top
__rt_stack_x86_64_top:

.section .text.start, "ax", @progbits
.global _start
.type _start, @function
_start:
    leaq __rt_stack_x86_64_top(%rip), %rsp
    andq $-16, %rsp
    call _rt_start_x86_64
1:
    hlt
    jmp 1b
