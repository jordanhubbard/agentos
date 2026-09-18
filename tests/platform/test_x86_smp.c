#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <platform/x86_smp.h>

static aos_x86_smp_cpu_t cpus[AOS_X86_SMP_MAX_CPUS];
static void init(unsigned count)
{
    memset(cpus,0,sizeof(cpus));
    for (unsigned i=0; i<count; i++) {
        assert(aos_x86_apic_init_cpu(&cpus[i].apic,100,i,i==0));
        cpus[i].state=i ? AOS_X86_CPU_WAIT_SIPI : AOS_X86_CPU_RUNNING;
        cpus[i].apic.svr=0x1ff;
        cpus[i].apic.ldr=(1u<<(i%8))<<24;
    }
}
static void reject(size_t count,unsigned sender,uint32_t command,uint64_t ticks)
{
    aos_x86_smp_cpu_t before[AOS_X86_SMP_MAX_CPUS];
    memcpy(before,cpus,sizeof(before));
    assert(!aos_x86_smp_icr(cpus,count,sender,command,ticks));
    assert(!memcmp(before,cpus,sizeof(before)));
}
int main(void)
{
    init(4);
    reject(0,0,0x40,100); reject(33,0,0x40,100); reject(4,4,0x40,100);
    reject(4,0,0x40,99);
    for (unsigned mode=1; mode<8; mode++)
        if (mode!=5 && mode!=6) reject(4,0,(mode<<8)|0x40,100);
    reject(4,0,0x0f,100); reject(4,0,0x8040,100); reject(4,0,0x8600,100);
    for (unsigned bit=0; bit<32; bit++)
        if (!((1u<<bit)&0xccfffu)) reject(4,0,(1u<<bit)|0x40,100);
    cpus[3].apic.id=2; reject(4,0,0xc4500,100); cpus[3].apic.id=3;
    cpus[3].apic.id=255; reject(4,0,0xc4500,100); cpus[3].apic.id=3;
    cpus[3].state=(aos_x86_cpu_start_state_t)-1; reject(4,0,0xc4500,100);
    init(4);
    cpus[0].apic.icr_high=2u<<24;
    assert(aos_x86_smp_icr(cpus,4,0,0x40,100));
    for (unsigned i=0; i<4; i++) assert(aos_x86_apic_pending(&cpus[i].apic,100)==(i==2 ? 0x40u : 0u));
    init(4);
    assert(aos_x86_smp_icr(cpus,4,2,0x40041,100)); /* self */
    for (unsigned i=0; i<4; i++) assert(aos_x86_apic_pending(&cpus[i].apic,100)==(i==2 ? 0x41u : 0u));
    init(4);
    assert(aos_x86_smp_icr(cpus,4,2,0xc0042,100)); /* all except self */
    for (unsigned i=0; i<4; i++) assert(aos_x86_apic_pending(&cpus[i].apic,100)==(i!=2 ? 0x42u : 0u));
    init(4);
    cpus[0].apic.icr_high=0x0au<<24;
    assert(aos_x86_smp_icr(cpus,4,0,0x843,100)); /* flat logical CPUs 1,3 */
    for (unsigned i=0; i<4; i++) assert(aos_x86_apic_pending(&cpus[i].apic,100)==(i%2 ? 0x43u : 0u));
    init(4);
    cpus[2].apic.id=17; cpus[3].apic.id=254;
    cpus[0].apic.icr_high=254u<<24;
    assert(aos_x86_smp_icr(cpus,4,0,0x44,100));
    assert(aos_x86_apic_pending(&cpus[3].apic,100)==0x44);
    assert(!aos_x86_apic_pending(&cpus[2].apic,100));
    init(4);
    for (unsigned i=0; i<4; i++) {
        cpus[i].apic.dfr=0x0fffffff;
        cpus[i].apic.ldr=((i<2 ? 0x10u : 0x20u)|(1u<<(i%2)))<<24;
    }
    cpus[0].apic.icr_high=0x21u<<24;
    assert(aos_x86_smp_icr(cpus,4,0,0x845,100));
    for (unsigned i=0; i<4; i++) assert(aos_x86_apic_pending(&cpus[i].apic,100)==(i==2 ? 0x45u : 0u));
    init(4);
    cpus[0].apic.icr_high=254u<<24; /* absent destination, no hidden CPU */
    assert(aos_x86_smp_icr(cpus,4,0,0x4500,100));
    for (unsigned i=0; i<4; i++) assert(!cpus[i].reset_pending);
    /* INIT resets virtual APIC state but leaves native reset work explicit. */
    assert(aos_x86_smp_icr(cpus,4,0,0xc4500,110));
    assert(cpus[0].state==AOS_X86_CPU_RUNNING && !cpus[0].reset_pending);
    for (unsigned i=1; i<4; i++) {
        assert(cpus[i].state==AOS_X86_CPU_WAIT_SIPI && cpus[i].reset_pending);
        assert(cpus[i].apic.id==i && !cpus[i].apic.bootstrap && cpus[i].apic.svr==0xff);
        assert(cpus[i].apic.now==110 && cpus[i].apic.ldr==0 && cpus[i].apic.dfr==UINT32_MAX);
    }
    aos_x86_smp_cpu_t ap=cpus[1];
    assert(aos_x86_smp_icr(cpus,4,0,0xc8500,110)); /* deassert */
    assert(!memcmp(&ap,&cpus[1],sizeof(ap)));
    assert(aos_x86_smp_icr(cpus,4,0,0xc4608,110)); /* SIPI, software-disabled APIC */
    for (unsigned i=1; i<4; i++)
        assert(cpus[i].state==AOS_X86_CPU_START_PENDING && cpus[i].startup_vector==8 && cpus[i].reset_pending);
    assert(aos_x86_smp_icr(cpus,4,0,0xc4609,110)); /* duplicate SIPI cannot redirect */
    for (unsigned i=1; i<4; i++) assert(cpus[i].startup_vector==8);
    cpus[1].state=AOS_X86_CPU_RUNNING; cpus[1].reset_pending=false;
    assert(aos_x86_smp_icr(cpus,4,0,0xc460a,110));
    assert(cpus[1].state==AOS_X86_CPU_RUNNING && !cpus[1].reset_pending && cpus[1].startup_vector==8);
    cpus[0].apic.icr_high=1u<<24;
    assert(aos_x86_smp_icr(cpus,4,0,0x4500,120));
    assert(aos_x86_smp_icr(cpus,4,0,0x46ff,120));
    assert(cpus[1].state==AOS_X86_CPU_START_PENDING && cpus[1].startup_vector==255 && cpus[1].reset_pending);
    init(32);
    assert(aos_x86_smp_icr(cpus,32,0,0x80044,100));
    for (unsigned i=0; i<32; i++) assert(aos_x86_apic_pending(&cpus[i].apic,100)==0x44);
    assert(aos_x86_smp_icr(cpus,32,0,0x44500,120)); /* self INIT retains BSP identity */
    assert(cpus[0].apic.bootstrap && cpus[0].apic.id==0 && cpus[0].apic.icr_low==0);
    assert(cpus[0].state==AOS_X86_CPU_WAIT_SIPI && cpus[0].reset_pending);
    puts("PASS: bounded IPI destinations, INIT reset, deassert, one-shot SIPI and explicit native startup work");
}
