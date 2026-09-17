#include <assert.h>
#include <stdio.h>
#include "platform/x86_cpu.h"

int main(void)
{
    assert(aos_x86_cpu_supported(AOS_X86_BASIC_EDX, AOS_X86_EXT_EDX, 0x3024u));
    for (unsigned i = 0; i < 32; i++) {
        if (AOS_X86_BASIC_EDX & (1u << i))
            assert(!aos_x86_cpu_supported(AOS_X86_BASIC_EDX & ~(1u << i), AOS_X86_EXT_EDX, 0x3024u));
        if (AOS_X86_EXT_EDX & (1u << i))
            assert(!aos_x86_cpu_supported(AOS_X86_BASIC_EDX, AOS_X86_EXT_EDX & ~(1u << i), 0x3024u));
    }
    assert(!aos_x86_cpu_supported(~0u, ~0u, 0x3023u));
    assert(!aos_x86_cpu_supported(~0u, ~0u, 0x2f24u));
    aos_x86_cpuid_t r = aos_x86_cpu_id(0, 0);
    assert(r.eax == 1u && r.ebx == 0x756e6547u && r.edx == 0x49656e69u && r.ecx == 0x6c65746eu);
    r = aos_x86_cpu_id(1, 0);
    assert(r.ecx == 0x80000000u && r.ebx == 0x10000u);
    assert((r.edx & ((1u<<9)|(1u<<12)|(1u<<16)|(1u<<28))) == 0u);
    assert((r.edx & ((1u<<6)|(1u<<25)|(1u<<26))) == ((1u<<6)|(1u<<25)|(1u<<26)));
    r = aos_x86_cpu_id(0x80000001u, 0);
    assert(r.edx == ((1u<<20)|(1u<<29)) && r.ecx == 0u);
    r = aos_x86_cpu_id(0x80000008u, 0);
    assert(r.eax == 0x3024u && r.ebx == 0u && r.ecx == 0u && r.edx == 0u);
    const uint32_t absent[] = {2u, 7u, 0xdu, 0x21u, 0x40000001u, 0x8000001fu, 0xffffffffu};
    for (unsigned i = 0; i < sizeof(absent)/sizeof(absent[0]); i++) {
        r = aos_x86_cpu_id(absent[i], 0xffffffffu);
        assert(!(r.eax | r.ebx | r.ecx | r.edx));
    }
    puts("PASS: CPU baseline admission, bounded CPUID features and unsupported leaves");
    return 0;
}
