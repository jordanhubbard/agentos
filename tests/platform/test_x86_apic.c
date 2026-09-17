#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "platform/x86_apic.h"
#include "platform/x86_memory.h"

static uint8_t ram[0x10000], rom[4096];
static void pte(unsigned at, uint64_t value)
{ for (unsigned i=0; i<8; i++) ram[at+i]=(uint8_t)(value >> (8*i)); }
static uint32_t apic_io(aos_x86_apic_t *a, unsigned off, bool write, uint32_t v, uint64_t t)
{ assert(aos_x86_apic_io(a,off,write,&v,t)); return v; }
static void rejected(aos_x86_apic_t *a, unsigned off, bool write, uint32_t v, uint64_t t)
{
    aos_x86_apic_t before=*a;
    uint32_t value=v;
    assert(!aos_x86_apic_io(a,off,write,&value,t));
    assert(value==v && !memcmp(a,&before,sizeof(before)));
}
int main(void)
{
    aos_x86_apic_t a,b;
    aos_x86_apic_init(&a, 100);
    aos_x86_apic_init(&b, 100);
    uint64_t msr=0;
    assert(aos_x86_apic_msr(false,&msr) && msr==0xfee00900);
    assert(aos_x86_apic_msr(true,&msr));
    msr|=0x400; assert(!aos_x86_apic_msr(true,&msr));
    assert(apic_io(&a,0x20,false,0,100)==0);
    assert(apic_io(&a,0x30,false,0,100)==0x50014);
    assert(apic_io(&a,0x320,false,0,100)==0x10000);
    assert(apic_io(&a,0x350,false,0,100)==0x10000);
    assert(apic_io(&a,0x360,false,0,100)==0x10000);
    apic_io(&a,0x350,true,0x700,100); /* ExtInt, no connected source */
    apic_io(&a,0x360,true,0x400,100); /* NMI, no connected source */
    assert(apic_io(&a,0x350,false,0,100)==0x700);
    assert(apic_io(&a,0x360,false,0,100)==0x400);
    assert(apic_io(&b,0x350,false,0,100)==0x10000);
    rejected(&a,0x350,true,0x4000,100); /* remote IRR is read-only */
    apic_io(&a,0xf0,true,0x1ff,100);
    apic_io(&a,0x3e0,true,0xb,100); /* divide by 1 */
    apic_io(&a,0x380,true,1000,100);
    assert(apic_io(&a,0x390,false,0,350)==750);
    assert(apic_io(&b,0x390,false,0,350)==0);
    assert(!aos_x86_apic_interrupt_due(&a,1100)); /* masked */
    apic_io(&a,0x320,true,0x40,350);
    assert(!aos_x86_apic_interrupt_due(&a,1099));
    assert(aos_x86_apic_interrupt_due(&a,1100));
    assert(apic_io(&a,0x390,false,0,1100)==0);
    assert(a.irr[2] == 1u);
    aos_x86_apic_init(&a,1100);
    apic_io(&a,0x3e0,true,0xb,1100);
    apic_io(&a,0x320,true,0x30040,1100); /* periodic, masked */
    apic_io(&a,0x380,true,10,1100);
    assert(apic_io(&a,0x390,false,0,1123)==7);
    apic_io(&a,0x3e0,true,0,1123); /* preserve 7 ticks, now divide by 2 */
    assert(apic_io(&a,0x390,false,0,1124)==7);
    assert(apic_io(&a,0x390,false,0,1125)==6);
    rejected(&a,0x320,true,0x40040,1125); /* no deadline mode */
    rejected(&a,0x390,true,1,1125);
    rejected(&a,0x321,false,0,1125);
    rejected(&a,0x300,true,0,1125); /* invalid fixed IPI vector */
    rejected(&a,0x390,false,0,1122); /* clock reversal */
    apic_io(&a,0x380,true,0,1125);
    assert(!aos_x86_apic_interrupt_due(&a,UINT64_MAX));
    for (unsigned i=0; i<8; i++) {
        unsigned enc=(i&3)|((i&4)<<1), div=1u<<((i+1)&7);
        apic_io(&a,0x3e0,true,enc,1200+i*1000);
        apic_io(&a,0x380,true,100,1200+i*1000);
        assert(apic_io(&a,0x390,false,0,1200+i*1000+div*2)==98);
        apic_io(&a,0x380,true,0,1200+i*1000+div*2);
    }
    aos_x86_apic_init(&b,0);
    apic_io(&b,0xf0,true,0x1ff,0);
    apic_io(&b,0x3e0,true,0xb,0);
    apic_io(&b,0x320,true,0x30040,0);
    apic_io(&b,0x380,true,10,0);
    apic_io(&b,0x320,true,0x20040,25); /* masked expiries do not become pending */
    assert(!aos_x86_apic_interrupt_due(&b,29));
    assert(aos_x86_apic_interrupt_due(&b,30));

    /* Expiry captures its vector; priority gates delivery without losing it. */
    assert(aos_x86_apic_pending(&b,30)==0x40);
    apic_io(&b,0x320,true,0x20060,30);
    assert(aos_x86_apic_pending(&b,30)==0x40);
    apic_io(&b,0x80,true,0x4f,30);
    assert(!aos_x86_apic_pending(&b,30));
    assert(!aos_x86_apic_accept(&b,0x40));
    apic_io(&b,0x80,true,0,30);
    assert(aos_x86_apic_accept(&b,0x40));
    assert(apic_io(&b,0x120,false,0,30)==1u); /* ISR */
    assert(apic_io(&b,0x220,false,0,30)==0u); /* IRR */
    assert(apic_io(&b,0xa0,false,0,30)==0x40);
    assert(!aos_x86_apic_accept(&b,0x40));
    assert(aos_x86_apic_pending(&b,40)==0x60); /* higher class nests */
    assert(aos_x86_apic_accept(&b,0x60));
    assert(!aos_x86_apic_pending(&b,100)); /* same class waits; expiries coalesce */
    assert(apic_io(&b,0x230,false,0,100)==1u);
    rejected(&b,0xb0,true,1,100);
    rejected(&b,0xb0,false,0,100);
    rejected(&b,0x120,true,0,100);
    apic_io(&b,0xb0,true,0,100); /* pop only highest in-service vector */
    assert(apic_io(&b,0x130,false,0,100)==0u);
    assert(apic_io(&b,0x120,false,0,100)==1u);
    assert(aos_x86_apic_pending(&b,100)==0x60);
    apic_io(&b,0xf0,true,0xff,100);
    assert(!aos_x86_apic_pending(&b,100));
    apic_io(&b,0xf0,true,0x1ff,100);
    assert(aos_x86_apic_accept(&b,0x60));
    assert(!aos_x86_apic_pending(&b,100));
    apic_io(&b,0xb0,true,0,100);
    apic_io(&b,0xb0,true,0,100);
    apic_io(&b,0xb0,true,0,100); /* idle EOI is harmless */
    assert(apic_io(&b,0xa0,false,0,100)==0);
    aos_x86_apic_init(&b,0);
    apic_io(&b,0xf0,true,0x1ff,0);
    apic_io(&b,0x320,true,0,0); /* valid programming while timer stopped */
    assert(!aos_x86_apic_pending(&b,100));
    apic_io(&b,0x380,true,1,100);
    assert(aos_x86_apic_pending(&b,102)==AOS_X86_APIC_INVALID_VECTOR);
    assert(!aos_x86_apic_accept(&b,AOS_X86_APIC_INVALID_VECTOR));

    /* Fixed IPIs remain inside one independently provisioned LAPIC. */
    aos_x86_apic_init(&a,0); aos_x86_apic_init(&b,0);
    apic_io(&a,0x300,true,0x40040,0); /* software-disabled APIC drops fixed IPI */
    assert(apic_io(&a,0xe0,false,0,0)==UINT32_MAX);
    apic_io(&a,0xf0,true,0x1ff,0);
    assert(!aos_x86_apic_pending(&a,0));
    apic_io(&a,0xf0,true,0x3ff,0); /* Linux disables legacy focus checking */
    assert(apic_io(&a,0xf0,false,0,0)==0x3ff);
    rejected(&a,0xf0,true,0x7ff,0); /* reserved bit10 */
    const unsigned lvts[]={0x330,0x340,0x370};
    for (unsigned i=0; i<3; i++) {
        assert(apic_io(&a,lvts[i],false,0,0)==0x10000);
        apic_io(&a,lvts[i],true,0x100fe,0);
        assert(apic_io(&a,lvts[i],false,0,0)==0x100fe);
        assert(apic_io(&b,lvts[i],false,0,0)==0x10000);
    }
    apic_io(&a,0x280,true,0,0); assert(!apic_io(&a,0x280,false,0,0));
    rejected(&a,0x280,true,1,0);
    apic_io(&a,0x300,true,0x40040,0); /* self */
    assert(apic_io(&a,0x300,false,0,0)==0x40040);
    assert(aos_x86_apic_pending(&a,0)==0x40 && !aos_x86_apic_pending(&b,0));
    assert(aos_x86_apic_accept(&a,0x40)); apic_io(&a,0xb0,true,0,0);
    apic_io(&a,0x300,true,0xc0040,0); assert(!aos_x86_apic_pending(&a,0));
    apic_io(&a,0x310,true,1u<<24,0); /* absent physical destination */
    apic_io(&a,0x300,true,0x40,0); assert(!aos_x86_apic_pending(&a,0));
    apic_io(&a,0xd0,true,2u<<24,0);
    apic_io(&a,0x310,true,2u<<24,0);
    apic_io(&a,0x300,true,0x840,0); /* flat logical destination */
    assert(aos_x86_apic_accept(&a,0x40)); apic_io(&a,0xb0,true,0,0);
    apic_io(&a,0xe0,true,0,0); assert(apic_io(&a,0xe0,false,0,0)==0x0fffffff);
    apic_io(&a,0xd0,true,0x21u<<24,0);
    apic_io(&a,0x310,true,0x11u<<24,0);
    apic_io(&a,0x300,true,0x840,0); assert(!aos_x86_apic_pending(&a,0));
    apic_io(&a,0x310,true,0x21u<<24,0);
    apic_io(&a,0x300,true,0x840,0); assert(aos_x86_apic_pending(&a,0)==0x40);
    rejected(&a,0x300,true,0x40440,0); /* NMI */
    rejected(&a,0x300,true,0x40500,0); /* INIT */
    rejected(&a,0x300,true,0x48040,0); /* level trigger */
    rejected(&a,0x300,true,0x4000f,0); /* reserved vector */
    rejected(&a,0x310,true,1,0); rejected(&a,0xd0,true,1,0);
    rejected(&a,0xe0,true,0x1fffffff,0);

    aos_x86_memory_t m={.ram=ram,.ram_size=sizeof(ram),.rom=rom,.rom_base=0xffc00000,.rom_size=sizeof(rom)};
    pte(0x1000,0x2003); pte(0x2000,0x3003); pte(0x3000,0x4003); pte(0x4000,0x5003);
    uint64_t pa=0xfeed;
    assert(aos_x86_translate(&m,0x1000,0x123,true,true,&pa) && pa==0x5123);
    ram[0x5fff]=0x89; ram[0x6000]=0x01; pte(0x4008,0x6003);
    uint8_t code[15]={0};
    assert(aos_x86_fetch(&m,0x1000,0xfff,code,2) && code[0]==0x89 && code[1]==1);
    pte(0x4000,0x5001); pa=0xfeed;
    assert(!aos_x86_translate(&m,0x1000,0,true,false,&pa) && pa==0xfeed);
    pte(0x4000,UINT64_C(0x8000000000005003));
    assert(!aos_x86_fetch(&m,0x1000,0,code,2));
    assert(aos_x86_translate(&m,0x1000,0,false,false,&pa) && pa==0x5000);
    pte(0x4000,0xffc00003); rom[0]=0x8b;
    assert(aos_x86_fetch(&m,0x1000,0,code,1) && code[0]==0x8b);
    pte(0x3000,0xfee00083);
    assert(aos_x86_translate(&m,0x1000,0x320,true,false,&pa) && pa==0xfee00320);
    assert(!aos_x86_fetch(&m,0x1000,0x320,code,1)); /* never dereference MMIO */
    pte(0x3000,0xfee02083);
    assert(!aos_x86_translate(&m,0x1000,0,false,false,&pa)); /* reserved large-page bits */
    pte(0x3000,UINT64_C(0x1000000003));
    assert(!aos_x86_translate(&m,0x1000,0,false,false,&pa)); /* >36 physical bits */
    assert(!aos_x86_translate(&m,0x1000,UINT64_C(0x800000000000),false,false,&pa));
    assert(!aos_x86_translate(&m,0x1001,0,false,false,&pa));
    assert(!aos_x86_translate(&m,0x10000,0,false,false,&pa));

    uint64_t regs[16]={0}; regs[1]=0xfee00320; regs[0]=0x123456789;
    aos_x86_mov_t op;
    const uint8_t load[]={0x8b,0x01}, store[]={0x89,0x01}, imm[]={0xc7,0x41,0x10,0x78,0x56,0x34,0x12};
    assert(aos_x86_decode_mov32(load,2,0,regs,&op) && !op.write && op.reg==0 && op.address==regs[1] && op.length==2);
    assert(aos_x86_decode_mov32(store,2,0,regs,&op) && op.write && op.value==0x23456789);
    assert(aos_x86_decode_mov32(imm,7,0,regs,&op) && op.value==0x12345678 && op.address==regs[1]+16 && op.length==7);
    const uint8_t sib[]={0x47,0x8b,0x44,0x8c,0xf0};
    regs[12]=0x1000; regs[9]=8;
    assert(aos_x86_decode_mov32(sib,5,0,regs,&op) && op.address==0x1010 && op.reg==8);
    const uint8_t relative[]={0x8b,0x05,0xfa,0xff,0xff,0xff};
    assert(aos_x86_decode_mov32(relative,6,0x1000,regs,&op) && op.address==0x1000);
    const uint8_t wide[]={0x48,0x8b,1}, direct[]={0x8b,0xc1}, locked[]={0xf0,0x89,1};
    assert(!aos_x86_decode_mov32(wide,3,0,regs,&op));
    assert(!aos_x86_decode_mov32(direct,2,0,regs,&op));
    assert(!aos_x86_decode_mov32(locked,3,0,regs,&op));
    for (unsigned i=1; i<sizeof(imm); i++) assert(!aos_x86_decode_mov32(imm,i,0,regs,&op));
    const uint8_t al[]={0x8a,0x01}, ah[]={0x8a,0x21}, spl[]={0x40,0x8a,0x21};
    const uint8_t r8b[]={0x44,0x8a,0x01}, zx8[]={0x0f,0xb6,0x01}, zx16[]={0x48,0x0f,0xb7,0x01};
    assert(aos_x86_decode_mov(al,2,0,regs,&op) && op.width==1 && op.reg==0 && op.shift==0);
    assert(aos_x86_mov_result(&op,UINT64_C(0x123456789abcdef0),0xff)==UINT64_C(0x123456789abcdeff));
    assert(!aos_x86_decode_mov32(al,2,0,regs,&op));
    assert(aos_x86_decode_mov(ah,2,0,regs,&op) && op.reg==0 && op.shift==8);
    assert(aos_x86_mov_result(&op,UINT64_C(0x123456789abcdef0),0xff)==UINT64_C(0x123456789abcfff0));
    assert(aos_x86_decode_mov(spl,3,0,regs,&op) && op.reg==4 && op.shift==0);
    assert(aos_x86_decode_mov(r8b,3,0,regs,&op) && op.reg==8 && op.shift==0);
    assert(aos_x86_decode_mov(zx8,3,0,regs,&op) && op.destination_bits==32 && op.length==3);
    assert(aos_x86_mov_result(&op,UINT64_MAX,0x1234)==0x34);
    assert(aos_x86_decode_mov(zx16,4,0,regs,&op) && op.width==2 && op.destination_bits==64);
    assert(aos_x86_mov_result(&op,UINT64_MAX,0x12345678)==0x5678);
    for (unsigned i=1; i<sizeof(zx16); i++) assert(!aos_x86_decode_mov(zx16,i,0,regs,&op));
    const uint8_t imm8[]={0xc6,0x01,0xa5}, store_ah[]={0x88,0x21};
    assert(aos_x86_decode_mov(imm8,3,0,regs,&op) && op.write && op.value==0xa5 && op.width==1);
    regs[0]=0x1234;
    assert(aos_x86_decode_mov(store_ah,2,0,regs,&op) && op.write && op.value==0x12);
    uint32_t absent=0;
    assert(aos_x86_absent_mmio(0xfed40000,1,false,&absent) && absent==0xff);
    assert(aos_x86_absent_mmio(0xfed440fe,2,false,&absent) && absent==0xffff);
    assert(aos_x86_absent_mmio(0xfed44ffc,4,false,&absent) && absent==UINT32_MAX);
    absent=0x1234;
    assert(!aos_x86_absent_mmio(0xfed44fff,2,false,&absent) && absent==0x1234);
    assert(!aos_x86_absent_mmio(0xfed45000,1,false,&absent));
    assert(!aos_x86_absent_mmio(0xfed40000,1,true,&absent));
    assert(!aos_x86_absent_mmio(0xfee00000,4,false,&absent));
    assert(aos_x86_rom_store(&m,0xffc00010,1));
    assert(aos_x86_rom_store(&m,0xffc00ffc,4));
    assert(!aos_x86_rom_store(&m,0xffc01000,1));
    assert(!aos_x86_rom_store(&m,0xffc00fff,2));
    assert(!aos_x86_rom_store(&m,0x5000,1));
    assert(!aos_x86_rom_store(&m,UINT64_MAX,4));
    puts("PASS: private APIC timer, bounded page walks and 32-bit MMIO MOV decoding");
    return 0;
}
