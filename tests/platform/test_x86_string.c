#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "platform/x86_string.h"

static uint8_t ram[32768], before[32768];
static const aos_x86_memory_t memory={.ram=ram,.ram_size=sizeof(ram)};
static void pte(unsigned at, uint64_t value)
{ for (unsigned i=0; i<8; i++) ram[at+i]=(uint8_t)(value >> (8*i)); }
static void setup(aos_x86_config_t *c)
{
    memset(ram,0,sizeof(ram));
    pte(0x1000,0x2003); pte(0x2000,0x3003); pte(0x3000,0x4003);
    pte(0x4000,0x5003); pte(0x4008,0x6003);
    assert(aos_x86_config_init(c,0x2000000));
}
static void reject(aos_x86_config_t *c, uint64_t address, uint64_t count, bool backwards)
{
    aos_x86_config_t saved=*c;
    memcpy(before,ram,sizeof(ram));
    uint64_t a=address,n=count;
    assert(!aos_x86_fw_insb(&memory,ram,0x1000,c,&a,&n,backwards));
    assert(a==address && n==count && !memcmp(c,&saved,sizeof(saved)));
    assert(!memcmp(before,ram,sizeof(ram)));
}
int main(void)
{
    aos_x86_config_t c;
    setup(&c);
    uint64_t a=0xffe,n=4;
    assert(aos_x86_fw_insb(&memory,ram,0x1000,&c,&a,&n,false));
    assert(a==0x1002 && n==0 && !memcmp(ram+0x5ffe,"QEMU",4));
    assert((ram[0x1000]&0x60)==0x20 && (ram[0x2000]&0x60)==0x20);
    assert((ram[0x4000]&0x60)==0x60 && (ram[0x4008]&0x60)==0x60);
    setup(&c); a=0x1001; n=4;
    assert(aos_x86_fw_insb(&memory,ram,0x1000,&c,&a,&n,true));
    assert(a==0xffd && !n && !memcmp(ram+0x5ffe,"UMEQ",4));
    setup(&c); a=0; n=300;
    assert(aos_x86_fw_insb(&memory,ram,0x1000,&c,&a,&n,false));
    assert(a==256 && n==44 && c.fw_reads==256);
    assert(!memcmp(ram+0x5000,"QEMU",4));
    assert(aos_x86_fw_insb(&memory,ram,0x1000,&c,&a,&n,false));
    assert(a==300 && !n && c.fw_reads==300);
    for (unsigned i=4;i<300;i++) assert(ram[0x5000+i]==0);
    setup(&c); a=UINT64_MAX; n=0;
    assert(aos_x86_fw_insb(&memory,ram,0x1000,&c,&a,&n,false));
    assert(a==UINT64_MAX && !n && !c.fw_reads);
    reject(&c,UINT64_MAX,1,false);
    reject(&c,0,1,true);
    pte(0x4008,0x6001); /* second destination page lacks write permission */
    reject(&c,0xffe,4,false);
    pte(0x4008,0xffc00003); /* ROM / device is never a writable destination */
    reject(&c,0x1000,4,false);
    pte(0x4008,0);
    reject(&c,0xffe,4,false);
    reject(&c,UINT64_C(0x800000000000),1,false);
    setup(&c);
    uint32_t selector=0x20;
    assert(aos_x86_config_io(&c,0x510,2,true,&selector,0));
    a=0x100; n=80;
    assert(aos_x86_fw_insb(&memory,ram,0x1000,&c,&a,&n,false));
    assert(!n && ram[0x510a]==0x0a && ram[0x5110]==1); /* first E820 length/type */
    assert(ram[0x5138]==1 && ram[0x514c]==2); /* remaining RAM / ROM types */
    puts("PASS: bounded fw_cfg REP input, cross-page and reverse delivery, A/D bits and atomic rejection");
    return 0;
}
