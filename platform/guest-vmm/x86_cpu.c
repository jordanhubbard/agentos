#include "platform/x86_cpu.h"

bool aos_x86_cpu_efer(uint64_t current, uint64_t requested, bool paging,
                      uint64_t *next)
{
    const uint64_t lme=UINT64_C(1)<<8, lma=UINT64_C(1)<<10;
    const uint64_t allowed=1u | lme | lma | (UINT64_C(1)<<11);
    if (!next || (requested & ~allowed) ||
        (paging && ((requested ^ current) & lme))) return false;
    *next=(requested & ~lma) | (current & lma);
    return true;
}

bool aos_x86_cpu_syscall_msr(uint32_t msr, bool write, uint64_t value)
{
    if (msr<0xc0000081u || msr>0xc0000084u) return false;
    if (!write || msr==0xc0000081u) return true; /* STAR selectors */
    if (msr==0xc0000084u) return value<=UINT32_MAX; /* FMASK */
    return (value >> 48)==((value & (UINT64_C(1)<<47)) ? 0xffffu : 0u);
}

uint64_t aos_x86_tsc_frequency(bool invariant, aos_x86_cpuid_t ratio,
                              aos_x86_cpuid_t hypervisor, aos_x86_cpuid_t timing)
{
    if (!invariant) return 0;
    uint64_t hz=ratio.eax ? (uint64_t)ratio.ecx*ratio.ebx/ratio.eax : 0;
    /* QEMU publishes its KVM TSC rate in CPUID 0x40000010.EAX, in kHz.
     * This clock discovery is for the host board, never guest CPUID passthrough. */
    if (!hz && hypervisor.eax >= 0x40000010u && hypervisor.eax < 0x40000100u &&
        hypervisor.ebx == 0x4b4d564bu && hypervisor.ecx == 0x564b4d56u &&
        hypervisor.edx == 0x4du)
        hz=(uint64_t)timing.eax*1000u;
    return hz && hz <= UINT64_C(10000000000) ? hz : 0;
}

bool aos_x86_cpu_identity_msr(uint32_t msr, bool write, uint64_t *value)
{
    if (!value || (msr != 0x17u && msr != 0x8bu)) return false;
    if (write) return msr == 0x8bu && *value == 0;
    *value=0;
    return true;
}

bool aos_x86_cpu_supported(uint32_t basic_edx, uint32_t ext_edx, uint32_t widths)
{
    return (basic_edx & AOS_X86_BASIC_EDX) == AOS_X86_BASIC_EDX &&
           (ext_edx & AOS_X86_EXT_EDX) == AOS_X86_EXT_EDX &&
           (widths & 0xffu) >= 36u && ((widths >> 8) & 0xffu) >= 48u;
}

bool aos_x86_cpu_clock_supported(uint64_t tsc_hz)
{
    return tsc_hz>=1000000u && tsc_hz<=UINT32_MAX;
}

aos_x86_cpuid_t aos_x86_cpu_id(uint32_t leaf, uint32_t subleaf, uint64_t tsc_hz)
{
    (void)subleaf; /* These legacy leaves do not use a subleaf index. */
    aos_x86_cpuid_t r = {0};
    bool clock=aos_x86_cpu_clock_supported(tsc_hz);
    switch (leaf) {
    case 0u:
        r.eax = clock ? 0x16u : 1u;
        r.ebx = 0x756e6547u; r.edx = 0x49656e69u; r.ecx = 0x6c65746eu;
        break; /* GenuineIntel, virtual model below; no host identity copied. */
    case 1u:
        r.eax = 0x00000600u; /* synthetic family 6 */
        r.ebx = 1u << 16; /* one logical processor, APIC ID zero */
        r.ecx = 1u << 31; /* hypervisor present */
        r.edx = AOS_X86_BASIC_EDX;
        break;
    case 0x40000000u:
        r.eax = 0x40000000u;
        r.ebx = 0x6e656761u; r.ecx = 0x20534f74u; r.edx = 0x204d4d56u;
        break; /* "agentOS VMM " */
    case 0x15u:
        if (clock) { r.eax=1; r.ebx=1; r.ecx=(uint32_t)tsc_hz; }
        break; /* virtual TSC:crystal ratio 1:1; also matches APIC bus ticks */
    case 0x16u:
        if (clock) r.eax=(uint32_t)(tsc_hz/1000000u);
        break; /* nominal virtual CPU MHz; no maximum/bus-frequency claim */
    case 0x80000000u: r.eax = 0x80000008u; break;
    case 0x80000001u: r.edx = AOS_X86_EXT_EDX; break;
    case 0x80000007u: if (clock) r.edx=1u << 8; break;
    case 0x80000008u: r.eax = 36u | (48u << 8); break;
    default: break;
    }
    return r;
}
