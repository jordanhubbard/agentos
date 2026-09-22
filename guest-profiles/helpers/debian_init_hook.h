/* Linux guest initramfs hooks, not agentOS PDs. Reserve standard descriptors
 * before stock udev starts, then preserve its shutdown and provide the
 * console nodes needed by run-init validation. */
#include <stdint.h>

#if defined(__aarch64__)
enum { NR_MKNODAT=33, NR_STAT=79, NR_EXEC=221, NR_FCNTL=25,
       NR_OPEN=56, NR_DUP=24, NR_CLOSE=57, NR_FORK=220, NR_WAIT=260,
       NR_WRITE=64, STAT_MODE=2, STAT_RDEV=4, FORK_FLAGS=17 };
static long call4(long number, long a, long b, long c, long d)
{
    register long x8 __asm__("x8") = number;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = 0;
    __asm__ volatile("svc #0" : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4)
                     : "cc", "memory");
    return x0;
}
#elif defined(__x86_64__)
enum { NR_MKNODAT=259, NR_STAT=262, NR_EXEC=59, NR_FCNTL=72,
       NR_OPEN=257, NR_DUP=33, NR_CLOSE=3, NR_FORK=57, NR_WAIT=61,
       NR_WRITE=1, STAT_MODE=3, STAT_RDEV=5, FORK_FLAGS=0 };
static long call4(long number, long a, long b, long c, long d)
{
    register long r10 __asm__("r10") = d;
    long result;
    __asm__ volatile("syscall" : "=a"(result)
                     : "a"(number), "D"(a), "S"(b), "d"(c), "r"(r10)
                     : "rcx", "r11", "cc", "memory");
    return result;
}
#else
#error unsupported Linux initramfs hook architecture
#endif

static int equal(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

#ifndef AGENTOS_INIT_TOP
static int node(const char *path, unsigned device)
{
    /* Linux mknodat and newfstatat. Refuse an existing symlink or a
     * different device rather than replacing something we did not create. */
    long result = call4(NR_MKNODAT, -100, (long)path, 020600, device);
    if (result != 0 && result != -17) return 0;
    uint64_t status[18];
    if (call4(NR_STAT, -100, (long)path, (long)status, 0x100) != 0) return 0;
    unsigned mode = (unsigned)status[STAT_MODE];
    return (mode & 0170000) == 0020000 && status[STAT_RDEV] == device;
}
#endif

static int stock(char **argv, char **environment)
{
    call4(NR_EXEC,
#ifdef AGENTOS_INIT_TOP
          (long)"/scripts/init-top/udev.stock",
#else
          (long)"/scripts/init-bottom/udev.stock",
#endif
          (long)argv, (long)environment, 0);
    return 111;
}

int hook_main(long argc, char **argv)
{
    char **environment = argv + argc + 1;
    if (argc > 1 && equal(argv[1], "prereqs")) return stock(argv, environment);
    int root_ok = 0;
    for (char **p = environment; *p; p++)
        if (equal(*p, "rootmnt=/root")) root_ok = 1;
    /* The pinned initramfs uses /root. Fail if that contract changes. */
    if (!root_ok) return 111;
#ifdef AGENTOS_INIT_TOP
    /* Linux can start /init without stdio when no initial console exists.
     * Reserve missing descriptors before udev allocates its control/event
     * sockets: its later make_null_stdio() must not overwrite those sockets.
     * Preserve the initramfs debug log if stdout/stderr already exist. */
    for (long fd = 0; fd < 3; fd++) {
        long result = call4(NR_FCNTL, fd, 1, 0, 0); /* fcntl F_GETFD */
        if (result >= 0) continue;
        if (result != -9) return 111; /* EBADF */
        long opened = call4(NR_OPEN, -100, (long)"/dev/null", 2, 0);
        if (opened < 0) return 111;
        if (opened != fd) {
            long copied = call4(NR_DUP, opened, fd, 0, 0); /* dup2 / dup3 */
            call4(NR_CLOSE, opened, 0, 0, 0);
            if (copied != fd) return 111;
        }
    }
    return stock(argv, environment);
#else
    long child = call4(NR_FORK, FORK_FLAGS, 0, 0, 0);
    if (child < 0) return 111;
    if (child == 0) return stock(argv, environment);
    int status = 0;
    long waited;
    do { waited = call4(NR_WAIT, child, (long)&status, 0, 0); } while (waited == -4);
    if (waited != child || status != 0) return 111;
    if (!node("/root/dev/console", 0x501) || !node("/root/dev/hvc0", 0xe500)) {
        static const char error[] = "agentos: console node preparation failed\n";
        call4(NR_WRITE, 2, (long)error, sizeof(error) - 1, 0);
        return 111;
    }
    static const char ready[] = "agentos: console nodes verified for run-init\n";
    call4(NR_WRITE, 2, (long)ready, sizeof(ready) - 1, 0);
    return 0;
#endif
}

#if defined(__aarch64__)
__asm__(".text\n.global _start\n_start:\n"
        "ldr x0, [sp]\nadd x1, sp, #8\nbl hook_main\n"
        "mov x8, #93\nsvc #0\nbrk #0\n");
#else
__asm__(".text\n.global _start\n_start:\n"
        "mov (%rsp), %rdi\nlea 8(%rsp), %rsi\nand $-16, %rsp\n"
        "call hook_main\nmov %eax, %edi\nmov $60, %eax\nsyscall\nud2\n");
#endif
