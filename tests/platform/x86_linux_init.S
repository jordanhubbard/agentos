/* Linux PID 1 qualification. All filesystem work uses guest Linux syscalls.
 * CPUID is solely a test completion trap; it is not a device transport. */
.section .text
.global _start
_start:
    mov $1, %r13d
    mov $39, %eax                 /* getpid */
    syscall
    cmp $1, %rax
    jne failed
    mov $2, %r13d
    mov $83, %eax                 /* mkdir /dev */
    lea dev_path(%rip), %rdi
    mov $0755, %esi
    syscall
    cmp $-17, %rax                /* Linux's built-in initramfs may supply /dev */
    je dev_directory_ready
    test %rax, %rax
    jne failed
dev_directory_ready:
    mov $3, %r13d
    mov $165, %eax                /* mount devtmpfs: native driver nodes */
    lea devfs(%rip), %rdi
    lea dev_path(%rip), %rsi
    lea devfs(%rip), %rdx
    xor %r10d, %r10d
    xor %r8d, %r8d
    syscall
    test %rax, %rax
    jne failed
    mov $4, %r13d
    mov $2, %eax                  /* hvc0 exists only after console binding */
    lea console_path(%rip), %rdi
    mov $2, %esi                  /* O_RDWR; host gate bounds the exchange */
    xor %edx, %edx
    syscall
    test %rax, %rax
    js failed
    mov $5, %r13d
    mov %rax, %r14
    mov %rax, %rdi
    mov $16, %eax                 /* ioctl(TCGETS), kernel termios ABI */
    mov $0x5401, %esi
    lea termios(%rip), %rdx
    syscall
    test %rax, %rax
    jne failed
    movl $0, termios(%rip)        /* raw input/output, no echo or canonical mode */
    movl $0, termios+4(%rip)
    movl $0, termios+12(%rip)
    movb $0, termios+22(%rip)     /* VTIME */
    movb $1, termios+23(%rip)     /* VMIN */
    mov $16, %eax
    mov %r14, %rdi
    mov $0x5402, %esi
    lea termios(%rip), %rdx
    syscall
    test %rax, %rax
    jne failed
    mov $7, %r13d
    lea ready(%rip), %rsi
    mov $ready_end-ready, %edx
    call console_write
    mov $8, %r13d
    lea console_buffer(%rip), %r15
    mov $request_end-request, %ebp
console_read:
    xor %eax, %eax
    mov %r14, %rdi
    mov %r15, %rsi
    mov %rbp, %rdx
    syscall
    test %rax, %rax
    jle failed
    add %rax, %r15
    sub %rax, %rbp
    jne console_read
    lea request(%rip), %rsi
    lea console_buffer(%rip), %rdi
    mov $request_end-request, %ecx
    cld
    repe cmpsb
    jne failed
    mov $9, %r13d
    lea reply(%rip), %rsi
    mov $reply_end-reply, %edx
    call console_write
    mov %r14, %rdi
    mov $3, %eax
    syscall
    test %rax, %rax
    jne failed
    mov $6, %r13d
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
    mov %r13d, %edx
    cpuid
    jmp failed
console_write:
    mov %rsi, %r15
    mov %rdx, %rbp
1:
    mov $1, %eax
    mov %r14, %rdi
    mov %r15, %rsi
    mov %rbp, %rdx
    syscall
    test %rax, %rax
    jle failed
    add %rax, %r15
    sub %rax, %rbp
    jne 1b
    ret
.section .rodata
path: .asciz "/userspace-proof"
dev_path: .asciz "/dev"
devfs: .asciz "devtmpfs"
console_path: .asciz "/dev/hvc0"
ready: .ascii "agentos-uart-ready\n"
ready_end:
request: .ascii "agentos-uart-request\n"
request_end:
reply: .ascii "agentos-uart-reply\n"
reply_end:
data: .ascii "agentOS x86 Linux syscall roundtrip\n"
data_end:
.section .bss
.balign 16
buffer: .skip data_end-data
termios: .skip 36
console_buffer: .skip request_end-request
.section .note.GNU-stack,"",@progbits
