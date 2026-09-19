.text
.globl _start
.type _start,@function
_start:
    xor %ebp,%ebp
    and $-16,%rsp
    call smp_probe_main
    ud2
.size _start,.-_start
.section .note.GNU-stack,"",@progbits
