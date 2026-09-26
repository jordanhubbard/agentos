#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "platform/x86_string.h"

static uint8_t ram[131072], before[131072];
static const aos_x86_memory_t memory={.ram=ram,.ram_size=sizeof(ram)};
static void pte(unsigned at, uint64_t value)
{ for (unsigned i=0; i<8; i++) ram[at+i]=(uint8_t)(value >> (8*i)); }
static uint32_t swap32(uint32_t n)
{ return ((n&0xffu)<<24)|((n&0xff00u)<<8)|((n>>8)&0xff00u)|(n>>24); }
static void put_be32(unsigned at,uint32_t n)
{ for (unsigned i=0;i<4;i++) ram[at+i]=(uint8_t)(n>>(24u-8u*i)); }
static void put_be64(unsigned at,uint64_t n)
{ put_be32(at,(uint32_t)(n>>32)); put_be32(at+4,(uint32_t)n); }
static void setup(aos_x86_config_t *c)
{
    memset(ram,0,sizeof(ram));
    pte(0x1000,0x2003); pte(0x2000,0x3003); pte(0x3000,0x4003);
    pte(0x4000,0x5003); pte(0x4008,0x6003); pte(0x4010,0x7003);
    pte(0x4018,0x8003); pte(0x4020,0x9003);
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
    setup(&c); pte(0x4008,0xa003); a=0xffe; n=4;
    assert(aos_x86_fw_insb(&memory,ram,0x1000,&c,&a,&n,false));
    assert(a==0x1002 && !n && !memcmp(ram+0x5ffe,"QE",2) &&
           !memcmp(ram+0xa000,"MU",2));
    setup(&c); a=0x1001; n=4;
    assert(aos_x86_fw_insb(&memory,ram,0x1000,&c,&a,&n,true));
    assert(a==0xffd && !n && !memcmp(ram+0x5ffe,"UMEQ",4));
    setup(&c); a=0; n=AOS_X86_STRING_BATCH+76u;
    assert(aos_x86_fw_insb(&memory,ram,0x1000,&c,&a,&n,false));
    assert(a==AOS_X86_STRING_BATCH && n==76 && c.fw_reads==AOS_X86_STRING_BATCH);
    assert(!memcmp(ram+0x5000,"QEMU",4));
    assert(aos_x86_fw_insb(&memory,ram,0x1000,&c,&a,&n,false));
    assert(a==AOS_X86_STRING_BATCH+76u && !n &&
           c.fw_reads==AOS_X86_STRING_BATCH+76u);
    for (unsigned i=4;i<AOS_X86_STRING_BATCH+76u;i++) assert(ram[0x5000+i]==0);
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
    static uint8_t kernel[1537];
    for (unsigned i=0;i<sizeof(kernel);i++) kernel[i]=(uint8_t)(i^(i>>8)^0x5a);
    setup(&c);
    aos_x86_boot_blobs_t blobs={.kernel=kernel,.kernel_size=sizeof(kernel)};
    assert(aos_x86_config_boot(&c,&blobs));
    put_be32(0x7000,(0x11u<<16)|8u|2u);
    put_be32(0x7004,sizeof(kernel));
    put_be64(0x7008,0x6800);
    uint32_t dma=swap32(0);
    assert(aos_x86_fw_dma_io(&memory,ram,&c,0x514,4,true,&dma));
    dma=swap32(0x7000);
    assert(aos_x86_fw_dma_io(&memory,ram,&c,0x518,4,true,&dma));
    assert(!memcmp(ram+0x6800,kernel,sizeof(kernel)) && !memcmp(ram+0x7000,"\0\0\0\0",4));
    assert(c.boot_reads[0]==sizeof(kernel) && c.fw_dma_address==0);
    selector=0x11;
    assert(aos_x86_config_io(&c,0x510,2,true,&selector,0));
    a=0xe00; n=sizeof(kernel);
    assert(aos_x86_fw_insb(&memory,ram,0x1000,&c,&a,&n,false));
    assert(a==0xe00+sizeof(kernel) && !n && c.fw_offset==sizeof(kernel));
    assert(!memcmp(ram+0x5e00,kernel,sizeof(kernel)));
    assert(aos_x86_fw_insb(&memory,ram,0x1000,&c,&a,&n,false));
    assert(!n && !memcmp(ram+0x5e00,kernel,sizeof(kernel)));
    selector=0x11;
    assert(aos_x86_config_io(&c,0x510,2,true,&selector,0));
    pte(0x4008,0x6001);
    reject(&c,0xe00,1024,false); /* no blob bytes consumed on partial translation */
    puts("PASS: bounded fw_cfg REP input, cross-page and reverse delivery, A/D bits and atomic rejection");
    return 0;
}
