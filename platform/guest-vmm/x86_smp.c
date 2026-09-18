#include <platform/x86_smp.h>

bool aos_x86_smp_icr(aos_x86_smp_cpu_t *cpus, size_t count, unsigned sender,
                     uint32_t command, uint64_t ticks)
{
    if (!cpus || !count || count>AOS_X86_SMP_MAX_CPUS || sender>=count ||
        (command & ~UINT32_C(0x000ccfff))) return false;
    unsigned mode=(command>>8)&7u, vector=command&255u;
    bool level=(command & 0x8000u)!=0, assert_level=(command & 0x4000u)!=0;
    if (mode!=0u && mode!=5u && mode!=6u) return false;
    if ((mode==0u && (vector<16u || level)) || (mode==6u && level)) return false;
    for (unsigned i=0; i<count; i++) {
        if (cpus[i].apic.id==255u || cpus[i].apic.now>ticks ||
            (unsigned)cpus[i].state>AOS_X86_CPU_START_PENDING) return false;
        for (unsigned j=0; j<i; j++)
            if (cpus[i].apic.id==cpus[j].apic.id) return false;
    }
    unsigned shortcut=(command>>18)&3u, destination=cpus[sender].apic.icr_high>>24;
    uint32_t targets=0;
    for (unsigned i=0; i<count; i++) {
        bool match=shortcut==1u ? i==sender : shortcut==2u ? true :
            shortcut==3u ? i!=sender :
            aos_x86_apic_destination(&cpus[i].apic,destination,(command & 0x800u)!=0);
        if (match) targets |= UINT32_C(1)<<i;
    }
    /* Advance only the sender's local clock, then publish the command before
     * delivery. A self-directed INIT subsequently resets its ICR too. */
    uint32_t old;
    (void)aos_x86_apic_io(&cpus[sender].apic,0x300,false,&old,ticks);
    cpus[sender].apic.icr_low=command;
    for (unsigned i=0; i<count; i++) {
        if (!(targets & (UINT32_C(1)<<i))) continue;
        aos_x86_smp_cpu_t *cpu=&cpus[i];
        if (mode==0u) {
            (void)aos_x86_apic_route(&cpu->apic,vector,cpu->apic.id,false,false);
        } else if (mode==5u) {
            if (level && !assert_level) continue; /* INIT deassert has no reset effect */
            (void)aos_x86_apic_init_cpu(&cpu->apic,ticks,cpu->apic.id,cpu->apic.bootstrap);
            cpu->state=AOS_X86_CPU_WAIT_SIPI;
            cpu->reset_pending=true;
            cpu->startup_vector=0;
        } else if (cpu->state==AOS_X86_CPU_WAIT_SIPI) {
            cpu->startup_vector=(uint8_t)vector;
            cpu->state=AOS_X86_CPU_START_PENDING;
        }
    }
    return true;
}
