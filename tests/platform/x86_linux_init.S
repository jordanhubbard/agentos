/* Linux PID 1 qualification. All filesystem work uses guest Linux syscalls.
 * CPUID is solely a test completion trap; it is not a device transport. */
.ifndef STORAGE_MODE
.set STORAGE_MODE, 0
.endif
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
    mov $10, %r13d
    mov $2, %eax                  /* open the in-tree virtio-blk device */
    lea disk_path(%rip), %rdi
.if STORAGE_MODE==1
    mov $2, %esi                 /* O_RDWR */
.else
    xor %esi, %esi               /* O_RDONLY */
.endif
    xor %edx, %edx
    syscall
    test %rax, %rax
    js failed
    mov %rax, %r12
    mov $11, %r13d
    mov $16, %eax                 /* ioctl(BLKGETSIZE64) */
    mov %r12, %rdi
    mov $0x80081272, %esi
    lea disk_capacity(%rip), %rdx
    syscall
    test %rax, %rax
    jne failed
    cmpq $33554432, disk_capacity(%rip)
    jne failed
    mov $12, %r13d
    mov $8, %eax                  /* read a different block from preboot */
    mov %r12, %rdi
    mov $4096, %esi
    xor %edx, %edx
    syscall
    cmp $4096, %rax
    jne failed
    mov $13, %r13d
    lea disk_buffer(%rip), %r15
    mov $4096, %ebp
disk_read:
    xor %eax, %eax
    mov %r12, %rdi
    mov %r15, %rsi
    mov %rbp, %rdx
    syscall
    test %rax, %rax
    jle failed
    add %rax, %r15
    sub %rax, %rbp
    jne disk_read
    mov $14, %r13d
    lea disk_data(%rip), %rsi
    lea disk_buffer(%rip), %rdi
    mov $disk_data_end-disk_data, %ecx
    cld
    repe cmpsb
    jne failed
    xor %eax, %eax
    mov $4096-(disk_data_end-disk_data), %ecx
    repe scasb
    jne failed
.if STORAGE_MODE
    mov $16, %r13d
    mov $8, %eax
    mov %r12, %rdi
    mov $8192, %esi
    xor %edx, %edx
    syscall
    cmp $8192, %rax
    jne failed
    lea disk_buffer(%rip), %r15
    mov $4096, %ebp
storage_read:
    xor %eax, %eax
    mov %r12, %rdi
    mov %r15, %rsi
    mov %rbp, %rdx
    syscall
    test %rax, %rax
    jle failed
    add %rax, %r15
    sub %rax, %rbp
    jne storage_read
    mov $17, %r13d
    lea disk_buffer(%rip), %rdi
    mov $4096, %ecx
.if STORAGE_MODE==1
    xor %eax, %eax                /* writer requires pristine media */
.else
    mov $0x5a, %eax               /* next cold boot must see the write */
.endif
    cld
    repe scasb
    jne failed
.if STORAGE_MODE==1
    lea disk_buffer(%rip), %rdi
    mov $4096, %ecx
    mov $0x5a, %eax
    rep stosb
    mov $18, %r13d
    mov $8, %eax
    mov %r12, %rdi
    mov $8192, %esi
    xor %edx, %edx
    syscall
    cmp $8192, %rax
    jne failed
    lea disk_buffer(%rip), %r15
    mov $4096, %ebp
storage_write:
    mov $1, %eax
    mov %r12, %rdi
    mov %r15, %rsi
    mov %rbp, %rdx
    syscall
    test %rax, %rax
    jle failed
    add %rax, %r15
    sub %rax, %rbp
    jne storage_write
    mov $19, %r13d
    mov $74, %eax                 /* fsync: wait for data and device flush */
    mov %r12, %rdi
    syscall
    test %rax, %rax
    jne failed
.endif
.endif
    mov $15, %r13d
    mov $3, %eax
    mov %r12, %rdi
    syscall
    test %rax, %rax
    jne failed
    mov $6, %r13d
    call network_probe
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
/* Native Linux packet socket over the in-tree virtio-net driver. The fixture
 * sends one ARP request to the local QEMU gateway and verifies its reply.
 * No host-forwarded port, Internet access or guest custom driver is used. */
network_probe:
    mov $20, %r13d
    mov $41, %eax                 /* socket(AF_PACKET, SOCK_RAW, ETH_P_ARP) */
    mov $17, %edi
    mov $3, %esi
    mov $0x0608, %edx             /* htons(0x0806) */
    syscall
    test %rax, %rax
    js failed
    mov %rax, %r12
    mov $21, %r13d
    mov $16, %eax                 /* SIOCGIFHWADDR: verify guest MAC */
    mov %r12, %rdi
    mov $0x8927, %esi
    lea net_ifreq(%rip), %rdx
    syscall
    test %rax, %rax
    jne failed
    cmpw $1, net_ifreq+16(%rip)   /* ARPHRD_ETHER */
    jne failed
    lea net_ifreq+18(%rip), %rsi
    lea arp_request+6(%rip), %rdi
    mov $6, %ecx
    cld
    repe cmpsb
    jne failed
    mov $22, %r13d
    mov $16, %eax                 /* SIOCGIFINDEX */
    mov %r12, %rdi
    mov $0x8933, %esi
    lea net_ifreq(%rip), %rdx
    syscall
    test %rax, %rax
    jne failed
    mov net_ifreq+16(%rip), %eax
    test %eax, %eax
    jle failed
    mov %eax, net_address+4(%rip)
    mov $23, %r13d
    mov $16, %eax                 /* SIOCGIFFLAGS */
    mov %r12, %rdi
    mov $0x8913, %esi
    lea net_ifreq(%rip), %rdx
    syscall
    test %rax, %rax
    jne failed
    orw $1, net_ifreq+16(%rip)    /* IFF_UP */
    mov $16, %eax
    mov %r12, %rdi
    mov $0x8914, %esi             /* SIOCSIFFLAGS */
    lea net_ifreq(%rip), %rdx
    syscall
    test %rax, %rax
    jne failed
    mov $24, %r13d
    mov $49, %eax                 /* bind packet socket to eth0 */
    mov %r12, %rdi
    lea net_address(%rip), %rsi
    mov $20, %edx
    syscall
    test %rax, %rax
    jne failed
    mov $25, %r13d
    mov $44, %eax                 /* sendto: exact Ethernet + ARP request */
    mov %r12, %rdi
    lea arp_request(%rip), %rsi
    mov $42, %edx
    xor %r10d, %r10d
    lea net_address(%rip), %r8
    mov $20, %r9d
    syscall
    cmp $42, %rax
    jne failed
    mov $26, %r13d
    mov %r12d, net_poll(%rip)
    mov $7, %eax                  /* poll: bounded guest receive wait */
    lea net_poll(%rip), %rdi
    mov $1, %esi
    mov $20000, %edx
    syscall
    cmp $1, %rax
    jne failed
    cmpw $1, net_poll+6(%rip)
    jne failed
    mov $27, %r13d
    mov $45, %eax                 /* recvfrom(MSG_DONTWAIT) */
    mov %r12, %rdi
    lea net_buffer(%rip), %rsi
    mov $2048, %edx
    mov $0x40, %r10d
    xor %r8d, %r8d
    xor %r9d, %r9d
    syscall
    cmp $42, %rax
    jl failed
    /* Ethernet padding is permitted; all 42 protocol bytes must match. */
    lea net_buffer(%rip), %rsi
    lea arp_reply(%rip), %rdi
    mov $42, %ecx
    cld
    repe cmpsb
    jne failed
    mov $28, %r13d
    mov $3, %eax
    mov %r12, %rdi
    syscall
    test %rax, %rax
    jne failed
    ret
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
disk_path: .asciz "/dev/vda"
disk_data: .ascii "agentos-guest-block-qualification-v1\n"
disk_data_end:
ready: .ascii "agentos-uart-ready\n"
ready_end:
request: .ascii "agentos-uart-request\n"
request_end:
reply: .ascii "agentos-uart-reply\n"
reply_end:
data: .ascii "agentOS x86 Linux syscall roundtrip\n"
data_end:
arp_request:
    .byte 0xff,0xff,0xff,0xff,0xff,0xff, 0x52,0x54,0,0x12,0x34,0x56, 8,6
    .byte 0,1,8,0,6,4,0,1, 0x52,0x54,0,0x12,0x34,0x56, 10,0,2,15
    .byte 0,0,0,0,0,0, 10,0,2,2
arp_reply:
    .byte 0x52,0x54,0,0x12,0x34,0x56, 0x52,0x55,10,0,2,2, 8,6
    .byte 0,1,8,0,6,4,0,2, 0x52,0x55,10,0,2,2, 10,0,2,2
    .byte 0x52,0x54,0,0x12,0x34,0x56, 10,0,2,15
.section .data
.balign 8
net_ifreq: .asciz "eth0"
    .zero 35                     /* IFNAMSIZ 16 + 24-byte ifreq union */
net_address:
    .short 17, 0x0608
    .long 0
    .short 0
    .byte 0,6
    .byte 0xff,0xff,0xff,0xff,0xff,0xff,0,0
net_poll: .long 0
    .short 1,0
.section .bss
.balign 16
buffer: .skip data_end-data
termios: .skip 36
console_buffer: .skip request_end-request
.balign 8
disk_capacity: .skip 8
disk_buffer: .skip 4096
net_buffer: .skip 2048
.section .note.GNU-stack,"",@progbits
