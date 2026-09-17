#include "platform/x86_cpu.h"

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

aos_x86_cpuid_t aos_x86_cpu_id(uint32_t leaf, uint32_t subleaf)
{
    (void)subleaf; /* These legacy leaves do not use a subleaf index. */
    aos_x86_cpuid_t r = {0};
    switch (leaf) {
    case 0u:
        r.eax = 1u;
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
    case 0x80000000u: r.eax = 0x80000008u; break;
    case 0x80000001u: r.edx = AOS_X86_EXT_EDX; break;
    case 0x80000008u: r.eax = 36u | (48u << 8); break;
    default: break;
    }
    return r;
}
