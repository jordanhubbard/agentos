/* One private guest page, linked at GPA 0x1000. The VMM never writes the
 * completion counter: both #GP handlers must run and return through IRETQ. */
.section .text
.code64
.global _start
_start:
    hlt
.org 0x100
entry:
    mov $0x12345678, %eax
    mov $0x87654321, %edx
    mov $0xffffffff, %ecx
read_fault:
    rdmsr
    cmpl $1, count(%rip)
    jne failed
    cmp $0x12345678, %eax
    jne failed
    cmp $0x87654321, %edx
    jne failed
write_fault:
    wrmsr
    cmpl $2, count(%rip)
    jne failed
    cmp $0x12345678, %eax
    jne failed
    cmp $0x87654321, %edx
    jne failed
    jmp _start
failed:
    hlt
    jmp failed
.org 0x200
handler:
    cmpq $0, (%rsp)
    jne failed
    cmpl $0, count(%rip)
    jne check_write
    cmpq $read_fault, 8(%rsp)
    jne failed
    jmp recover
check_write:
    cmpl $1, count(%rip)
    jne failed
    cmpq $write_fault, 8(%rsp)
    jne failed
recover:
    incl count(%rip)
    addq $2, 8(%rsp)
    add $8, %rsp
    iretq
.org 0x280
gdt:
    .quad 0, 0x00af9b000000ffff, 0x00cf93000000ffff
count:
    .long 0
.org 0x400
idt:
    .zero 13*16
    .word 0x1200, 8
    .byte 0, 0x8e
    .word 0
    .long 0, 0
.section .note.GNU-stack,"",@progbits
