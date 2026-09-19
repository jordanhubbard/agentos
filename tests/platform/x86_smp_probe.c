/* Freestanding Linux/x86-64 qualification payload. No guest packages needed.
 * Two forked workers are pinned to distinct online CPUs; x87 and SSE values
 * remain live across repeated VMM timer exits and overlapping execution. */
#include <stdint.h>
#include <stddef.h>

static long syscall6(long nr,long a,long b,long c,long d,long e,long f)
{
    register long r10 __asm__("r10")=d;
    register long r8 __asm__("r8")=e;
    register long r9 __asm__("r9")=f;
    long result;
    __asm__ volatile("syscall" : "=a"(result)
        : "a"(nr),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8),"r"(r9)
        : "rcx","r11","memory");
    return result;
}
static _Noreturn void finish(unsigned status)
{
    (void)syscall6(60,status,0,0,0,0,0);
    for (;;) __asm__ volatile("ud2");
}
static _Noreturn void fail(void)
{
    static const char message[]="FAIL: x86 SMP CPU affinity, overlap or register state\n";
    (void)syscall6(1,2,(long)message,sizeof(message)-1u,0,0,0);
    finish(1);
}
static uint64_t now(void)
{
    struct { long seconds,nanoseconds; } stamp;
    if (syscall6(228,1,(long)&stamp,0,0,0,0)!=0 || stamp.seconds<0 ||
        stamp.nanoseconds<0 || stamp.nanoseconds>=1000000000) fail();
    return (uint64_t)stamp.seconds*UINT64_C(1000000000)+(uint64_t)stamp.nanoseconds;
}
typedef struct {
    uint32_t ready;
    struct { uint64_t start,end; uint32_t cpu,passed; } worker[2];
} shared_t;

static void worker(shared_t *shared,unsigned id)
{
    (void)syscall6(37,60,0,0,0,0,0); /* bounded even if the peer cannot run */
    uint64_t mask=UINT64_C(1)<<id;
    if (syscall6(203,0,sizeof(mask),(long)&mask,0,0,0)!=0) fail();
    unsigned cpu=99;
    if (syscall6(309,(long)&cpu,0,0,0,0,0)!=0 || cpu!=id) fail();
    shared->worker[id].cpu=cpu;
    __atomic_fetch_or(&shared->ready,1u<<id,__ATOMIC_RELEASE);
    while (__atomic_load_n(&shared->ready,__ATOMIC_ACQUIRE)!=3u)
        __asm__ volatile("pause");
    const uint64_t expected[2]={UINT64_C(0x123456789abcdef0)^id,
                                UINT64_C(0xfedcba9876543210)^id};
    const int integer=id ? 39 : 17;
    uint64_t observed[2];
    int observed_integer;
    uint32_t remaining=20000000u;
    shared->worker[id].start=now();
    /* No ABI calls inside this block: the values must survive actual guest
     * preemption, not an ordinary function's caller-saved register clobbers. */
    __asm__ volatile(
        "fninit\n\tfildl %[integer]\n\tmovdqu %[expected], %%xmm0\n"
        "1:\tpause\n\tdecl %[remaining]\n\tjnz 1b\n"
        "movdqu %%xmm0, %[observed]\n\tfistpl %[observed_integer]"
        : [remaining]"+&r"(remaining),[observed]"=m"(observed),
          [observed_integer]"=m"(observed_integer)
        : [integer]"m"(integer),[expected]"m"(expected)
        : "xmm0","st","cc","memory");
    shared->worker[id].end=now();
    if (syscall6(309,(long)&cpu,0,0,0,0,0)!=0 || cpu!=id ||
        observed[0]!=expected[0] || observed[1]!=expected[1] || observed_integer!=integer)
        fail();
    shared->worker[id].passed=1;
}

_Noreturn void smp_probe_main(void)
{
    static const char online[]="/sys/devices/system/cpu/online";
    long fd=syscall6(257,-100,(long)online,0,0,0,0);
    char cpus[32];
    if (fd<0 || syscall6(0,fd,(long)cpus,sizeof(cpus),0,0,0)!=4 ||
        cpus[0]!='0' || cpus[1]!='-' || cpus[2]!='1' || cpus[3]!='\n') fail();
    (void)syscall6(3,fd,0,0,0,0,0);
    long area=syscall6(9,0,4096,3,0x21,-1,0); /* shared anonymous read/write */
    if ((uint64_t)area>=UINT64_MAX-4095u) fail();
    shared_t *shared=(shared_t *)area;
    long child=syscall6(57,0,0,0,0,0,0);
    if (child<0) fail();
    if (!child) { worker(shared,1); finish(0); }
    worker(shared,0);
    int status=-1;
    long waited;
    do { waited=syscall6(61,child,(long)&status,0,0,0,0); } while (waited==-4);
    if (waited!=child || status || shared->ready!=3 ||
        !shared->worker[0].passed || !shared->worker[1].passed ||
        shared->worker[0].cpu!=0 || shared->worker[1].cpu!=1 ||
        shared->worker[0].start>=shared->worker[1].end ||
        shared->worker[1].start>=shared->worker[0].end) fail();
    static const char message[]="PASS: online=0-1 affinity=0,1 overlapping x87/SSE workers\n";
    if (syscall6(1,1,(long)message,sizeof(message)-1u,0,0,0)!=(long)sizeof(message)-1)
        fail();
    finish(0);
}
