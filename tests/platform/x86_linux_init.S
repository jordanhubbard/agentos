/* Linux PID 1 qualification. All filesystem work uses guest Linux syscalls.
 * CPUID is solely a test completion trap; it is not a device transport. */
.section .text
.global _start
_start:
    mov $39, %eax                 /* getpid */
    syscall
    cmp $1, %rax
    jne failed
    mov $2, %eax                  /* open a file on the initramfs */
    lea path(%rip), %rdi
    mov $0x242, %esi              /* O_CREAT | O_TRUNC | O_RDWR */
    mov $0600, %edx
    syscall
    test %rax, %rax
    js failed
    mov %rax, %r12
    mov $1, %eax                  /* write */
    mov %r12, %rdi
    lea data(%rip), %rsi
    mov $data_end-data, %edx
    syscall
    cmp $data_end-data, %rax
    jne failed
    mov $8, %eax                  /* lseek(fd, 0, SEEK_SET) */
    mov %r12, %rdi
    xor %esi, %esi
    xor %edx, %edx
    syscall
    test %rax, %rax
    jne failed
    xor %eax, %eax                /* read */
    mov %r12, %rdi
    lea buffer(%rip), %rsi
    mov $data_end-data, %edx
    syscall
    cmp $data_end-data, %rax
    jne failed
    lea data(%rip), %rsi
    lea buffer(%rip), %rdi
    mov $data_end-data, %ecx
    cld
    repe cmpsb
    jne failed
    mov $3, %eax                  /* close */
    mov %r12, %rdi
    syscall
    test %rax, %rax
    jne failed
    mov $39, %eax
    syscall
    cmp $1, %rax
    jne failed
    mov %eax, %ebx
    mov $0x41554f53, %eax         /* dedicated qualification leaf */
    mov $0x494e4954, %ecx         /* INIT */
    mov $0x50415353, %edx         /* PASS */
    cpuid
failed:
    mov $0x41554f53, %eax
    xor %ebx, %ebx
    mov $0x494e4954, %ecx
    xor %edx, %edx
    cpuid
    jmp failed
.section .rodata
path: .asciz "/userspace-proof"
data: .ascii "agentOS x86 Linux syscall roundtrip\n"
data_end:
.section .bss
.balign 16
buffer: .skip data_end-data
.section .note.GNU-stack,"",@progbits
