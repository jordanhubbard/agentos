#include <assert.h>
#include <stdio.h>
#include "platform/x86_cpu.h"

int main(void)
{
    uint64_t next=0xdead;
    assert(aos_x86_cpu_efer(0xd00,0xd01,true,&next) && next==0xd01);
    assert(aos_x86_cpu_efer(0xd01,0x900,true,&next) && next==0xd00);
    assert(aos_x86_cpu_efer(0,0x501,false,&next) && next==0x101);
    next=0xdead;
    assert(!aos_x86_cpu_efer(0xd01,0x801,true,&next) && next==0xdead);
    assert(!aos_x86_cpu_efer(0,UINT64_C(1)<<32,false,&next) && next==0xdead);
    assert(!aos_x86_cpu_efer(0,2,false,&next) && next==0xdead);
    assert(!aos_x86_cpu_efer(0,0,false,NULL));
    assert(aos_x86_cpu_syscall_msr(0xc0000081,true,UINT64_MAX));
    assert(aos_x86_cpu_syscall_msr(0xc0000084,true,0x47700));
    assert(!aos_x86_cpu_syscall_msr(0xc0000084,true,UINT64_C(1)<<32));
    for (uint32_t msr=0xc0000082; msr<=0xc0000083; msr++) {
        assert(aos_x86_cpu_syscall_msr(msr,false,0));
        assert(aos_x86_cpu_syscall_msr(msr,true,0x00007fffffffffff));
        assert(aos_x86_cpu_syscall_msr(msr,true,UINT64_C(0xffff800000000000)));
        assert(!aos_x86_cpu_syscall_msr(msr,true,0x0000800000000000));
        assert(!aos_x86_cpu_syscall_msr(msr,true,UINT64_C(0xffff7fffffffffff)));
    }
    assert(!aos_x86_cpu_syscall_msr(0xc0000080,false,0));
    assert(!aos_x86_cpu_syscall_msr(0xc0000085,true,0));
    assert(!aos_x86_cpu_syscall_msr(0x79,true,0));
    aos_x86_cpuid_t ratio={.eax=2,.ebx=192,.ecx=24000000}, zero={0};
    aos_x86_cpuid_t kvm={.eax=0x40000010,.ebx=0x4b4d564b,.ecx=0x564b4d56,.edx=0x4d};
    aos_x86_cpuid_t timing={.eax=2400000};
    assert(aos_x86_tsc_frequency(true,ratio,zero,zero)==UINT64_C(2304000000));
    assert(aos_x86_tsc_frequency(true,zero,kvm,timing)==UINT64_C(2400000000));
    assert(aos_x86_tsc_frequency(true,ratio,kvm,timing)==UINT64_C(2304000000));
    assert(!aos_x86_tsc_frequency(false,ratio,kvm,timing));
    assert(!aos_x86_tsc_frequency(true,zero,zero,timing));
    assert(!aos_x86_tsc_frequency(true,zero,kvm,zero));
    kvm.edx++; assert(!aos_x86_tsc_frequency(true,zero,kvm,timing)); kvm.edx--;
    kvm.eax--; assert(!aos_x86_tsc_frequency(true,zero,kvm,timing)); kvm.eax++;
    timing.eax=UINT32_MAX; assert(!aos_x86_tsc_frequency(true,zero,kvm,timing));
    ratio.ecx=UINT32_MAX; ratio.ebx=UINT32_MAX; ratio.eax=1;
    assert(!aos_x86_tsc_frequency(true,ratio,zero,zero));
    uint64_t value=UINT64_MAX;
    assert(aos_x86_cpu_identity_msr(0x17,false,&value) && value==0);
    assert(!aos_x86_cpu_identity_msr(0x17,true,&value));
    assert(aos_x86_cpu_identity_msr(0x8b,true,&value));
    value=1;
    assert(!aos_x86_cpu_identity_msr(0x8b,true,&value) && value==1);
    assert(aos_x86_cpu_identity_msr(0x8b,false,&value) && value==0);
    assert(!aos_x86_cpu_identity_msr(0x79,true,&value));
    assert(!aos_x86_cpu_identity_msr(0x1a0,false,&value));
    assert(aos_x86_cpu_supported(AOS_X86_BASIC_EDX, AOS_X86_EXT_EDX, 0x3024u));
    for (unsigned i = 0; i < 32; i++) {
        if (AOS_X86_BASIC_EDX & (1u << i))
            assert(!aos_x86_cpu_supported(AOS_X86_BASIC_EDX & ~(1u << i), AOS_X86_EXT_EDX, 0x3024u));
        if (AOS_X86_EXT_EDX & (1u << i))
            assert(!aos_x86_cpu_supported(AOS_X86_BASIC_EDX, AOS_X86_EXT_EDX & ~(1u << i), 0x3024u));
    }
    assert(!aos_x86_cpu_supported(~0u, ~0u, 0x3023u));
    assert(!aos_x86_cpu_supported(~0u, ~0u, 0x2f24u));
    aos_x86_cpuid_t r = aos_x86_cpu_id(0, 0, 0);
    assert(r.eax == 1u && r.ebx == 0x756e6547u && r.edx == 0x49656e69u && r.ecx == 0x6c65746eu);
    r = aos_x86_cpu_id(1, 0, 0);
    assert(r.ecx == 0x80000000u && r.ebx == 0x10000u);
    assert((r.edx & ((1u<<12)|(1u<<16)|(1u<<28))) == 0u);
    assert(r.edx & (1u<<9)); /* VMM-owned local xAPIC */
    assert((r.edx & ((1u<<6)|(1u<<25)|(1u<<26))) == ((1u<<6)|(1u<<25)|(1u<<26)));
    r = aos_x86_cpu_id(0x80000001u, 0, 0);
    assert(r.edx == ((1u<<11)|(1u<<20)|(1u<<29)) && r.ecx == 0u);
    r = aos_x86_cpu_id(0x80000008u, 0, 0);
    assert(r.eax == 0x3024u && r.ebx == 0u && r.ecx == 0u && r.edx == 0u);
    const uint32_t absent[] = {2u, 7u, 0xdu, 0x21u, 0x40000001u, 0x8000001fu, 0xffffffffu};
    for (unsigned i = 0; i < sizeof(absent)/sizeof(absent[0]); i++) {
        r = aos_x86_cpu_id(absent[i], 0xffffffffu, 3187200000u);
        assert(!(r.eax | r.ebx | r.ecx | r.edx));
    }
    const uint64_t clocks[]={1000000u,3187200000u,UINT32_MAX};
    for (unsigned i=0; i<sizeof(clocks)/sizeof(clocks[0]); i++) {
        assert(aos_x86_cpu_clock_supported(clocks[i]));
        r=aos_x86_cpu_id(0,0,clocks[i]); assert(r.eax==0x16u);
        r=aos_x86_cpu_id(0x15,0,clocks[i]);
        assert(r.eax && r.ebx && (uint64_t)r.ecx*r.ebx/r.eax==clocks[i]);
        assert(r.ecx==clocks[i] && !r.edx); /* same undivided APIC clock */
        r=aos_x86_cpu_id(0x16,0,clocks[i]);
        assert(r.eax==clocks[i]/1000000u && !(r.ebx|r.ecx|r.edx));
        r=aos_x86_cpu_id(0x80000007,0,clocks[i]); assert(r.edx==0x100u);
    }
    const uint64_t bad_clocks[]={0,999999u,UINT64_C(1)<<32,UINT64_MAX};
    const uint32_t clock_leaves[]={0x15,0x16,0x80000007};
    for (unsigned i=0; i<sizeof(bad_clocks)/sizeof(bad_clocks[0]); i++) {
        assert(!aos_x86_cpu_clock_supported(bad_clocks[i]));
        r=aos_x86_cpu_id(0,0,bad_clocks[i]); assert(r.eax==1u);
        for (unsigned j=0; j<sizeof(clock_leaves)/sizeof(clock_leaves[0]); j++) {
            r=aos_x86_cpu_id(clock_leaves[j],0,bad_clocks[i]);
            assert(!(r.eax|r.ebx|r.ecx|r.edx));
        }
    }
    puts("PASS: CPU baseline admission, bounded CPUID features and unsupported leaves");
    return 0;
}
