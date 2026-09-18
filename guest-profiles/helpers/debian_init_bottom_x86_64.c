/* Linux guest initramfs hook, not an agentOS PD. Preserve the stock udev
 * handoff, then provide the console nodes needed by run-init validation. */
#include <stdint.h>

static long call4(long number, long a, long b, long c, long d)
{
    register long r10 __asm__("r10") = d;
    long result;
    __asm__ volatile("syscall" : "=a"(result)
                     : "a"(number), "D"(a), "S"(b), "d"(c), "r"(r10)
                     : "rcx", "r11", "cc", "memory");
    return result;
}

static int equal(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int node(const char *path, unsigned device)
{
    /* x86_64 Linux mknodat and newfstatat. Refuse an existing symlink or a
     * different device rather than replacing something we did not create. */
    long result = call4(259, -100, (long)path, 020600, device);
    if (result != 0 && result != -17) return 0;
    uint64_t status[18];
    if (call4(262, -100, (long)path, (long)status, 0x100) != 0) return 0;
    unsigned mode = (unsigned)status[3];
    return (mode & 0170000) == 0020000 && status[5] == device;
}

static int stock(char **argv, char **environment)
{
    call4(59, (long)"/scripts/init-bottom/udev.stock",
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
    long child = call4(57, 0, 0, 0, 0);
    if (child < 0) return 111;
    if (child == 0) return stock(argv, environment);
    int status = 0;
    long waited;
    do { waited = call4(61, child, (long)&status, 0, 0); } while (waited == -4);
    if (waited != child || status != 0) return 111;
    if (!node("/root/dev/console", 0x501) || !node("/root/dev/hvc0", 0xe500)) {
        static const char error[] = "agentos: console node preparation failed\n";
        call4(1, 2, (long)error, sizeof(error) - 1, 0);
        return 111;
    }
    static const char ready[] = "agentos: console nodes verified for run-init\n";
    call4(1, 2, (long)ready, sizeof(ready) - 1, 0);
    return 0;
}

__asm__(".text\n.global _start\n_start:\n"
        "mov (%rsp), %rdi\nlea 8(%rsp), %rsi\nand $-16, %rsp\n"
        "call hook_main\nmov %eax, %edi\nmov $60, %eax\nsyscall\nud2\n");
