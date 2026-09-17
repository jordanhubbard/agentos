#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "platform/x86_config.h"

static uint32_t io(aos_x86_config_t *s, unsigned port, unsigned width,
                   bool write, uint32_t value)
{
    assert(aos_x86_config_io(s, (uint16_t)port, width, write, &value, 0x12345678u));
    return value;
}
static void select_pci(aos_x86_config_t *s, uint32_t address)
{ (void)io(s, 0xcf8, 4, true, address); }
static uint64_t read_le(const uint8_t *p, unsigned size)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i) value |= (uint64_t)p[i] << (8*i);
    return value;
}
static void fw(aos_x86_config_t *s, unsigned selector, uint8_t *data, unsigned size)
{
    io(s, 0x510, 2, true, selector);
    for (unsigned i = 0; i < size; ++i) data[i] = io(s, 0x511, 1, false, 0);
}
static void reject(aos_x86_config_t *s, unsigned port, unsigned width, bool write)
{
    aos_x86_config_t before = *s;
    uint32_t value = 0xabcdef01;
    assert(!aos_x86_config_io(s, port, width, write, &value, 0x12345678u));
    assert(value == 0xabcdef01 && !memcmp(s, &before, sizeof(before)));
}
static void boot_tests(void)
{
    static uint8_t kernel[1537], initrd[257];
    for (unsigned i=0; i<sizeof(kernel); i++) kernel[i]=(uint8_t)(i^(i>>8)^0x5a);
    for (unsigned i=0; i<sizeof(initrd); i++) initrd[i]=(uint8_t)(i+7);
    static const uint8_t cmd[]="console=hvc0 rdinit=/init";
    aos_x86_boot_blobs_t blobs={kernel,initrd,cmd,sizeof(kernel),sizeof(initrd),sizeof(cmd)};
    aos_x86_config_t a,b;
    assert(aos_x86_config_init(&a,0x8000000));
    assert(aos_x86_config_init(&b,0x8000000));
    assert(!aos_x86_config_boot(NULL,&blobs));
    assert(!aos_x86_config_boot(&a,NULL));
    aos_x86_config_t original=a;
    for (unsigned kind=0; kind<9; kind++) {
        aos_x86_boot_blobs_t bad=blobs;
        switch (kind) {
        case 0: bad.kernel=NULL; break;
        case 1: bad.kernel_size=0; break;
        case 2: bad.kernel_size=AOS_X86_BOOT_BLOB_LIMIT+1; break;
        case 3: bad.initrd_size=AOS_X86_BOOT_BLOB_LIMIT+1; break;
        case 4: bad.initrd=NULL; break;
        case 5: bad.cmdline_size=AOS_X86_BOOT_CMDLINE_LIMIT+1; break;
        case 6: bad.cmdline=NULL; break;
        case 7: bad.cmdline_size--; break; /* missing final NUL */
        case 8: bad.cmdline=(const uint8_t *)"x\ny"; bad.cmdline_size=4; break;
        }
        assert(!aos_x86_config_boot(&a,&bad));
        assert(!memcmp(&a,&original,sizeof(a)));
    }
    assert(aos_x86_config_boot(&a,&blobs));
    original=a;
    assert(!aos_x86_config_boot(&a,&blobs));
    assert(!memcmp(&a,&original,sizeof(a)));
    uint8_t bytes[1541];
    fw(&a,8,bytes,8);
    assert(read_le(bytes,8)==sizeof(kernel));
    fw(&a,0xb,bytes,8); assert(read_le(bytes,8)==sizeof(initrd));
    fw(&a,0x14,bytes,8); assert(read_le(bytes,8)==sizeof(cmd));
    fw(&a,0x11,bytes,sizeof(bytes));
    assert(!memcmp(bytes,kernel,sizeof(kernel)));
    for (unsigned i=sizeof(kernel); i<sizeof(bytes); i++) assert(!bytes[i]);
    fw(&a,0x11,bytes,97); assert(!memcmp(bytes,kernel,97)); /* reselect resets */
    fw(&a,0x12,bytes,sizeof(initrd)); assert(!memcmp(bytes,initrd,sizeof(initrd)));
    fw(&a,0x15,bytes,sizeof(cmd)); assert(!memcmp(bytes,cmd,sizeof(cmd)));
    assert(a.boot_reads[0]==sizeof(kernel)+97 && a.boot_reads[1]==sizeof(initrd) &&
           a.boot_reads[2]==sizeof(cmd));
    fw(&a,0x17,bytes,8); assert(read_le(bytes,8)==0); /* EFI only, no setup */
    fw(&b,8,bytes,8); assert(read_le(bytes,8)==0); /* isolation */
    assert(!aos_x86_config_boot(&b,&blobs)); /* cannot bind after first read */
    a.fw_offset=UINT32_MAX; a.fw_reads=UINT32_MAX;
    assert(io(&a,0x511,1,false,0)==0 && a.fw_offset==UINT32_MAX && a.fw_reads==UINT32_MAX);
    assert(aos_x86_config_init(&b,0x8000000));
    blobs.initrd=NULL; blobs.initrd_size=0; blobs.cmdline=NULL; blobs.cmdline_size=0;
    assert(aos_x86_config_boot(&b,&blobs));
    fw(&b,0xb,bytes,8); assert(read_le(bytes,8)==0);
    fw(&b,0x14,bytes,8); assert(read_le(bytes,8)==0);
}

int main(void)
{
    boot_tests();
    aos_x86_config_t a, b;
    assert(!aos_x86_config_init(NULL, 0x2000000));
    assert(!aos_x86_config_init(&a, 0x1000000));
    assert(!aos_x86_config_init(&a, 0x2000001));
    assert(!aos_x86_config_init(&a, 0x80010000));
    assert(aos_x86_config_init(&a, 0x2000000));
    assert(aos_x86_config_init(&b, 0x80000000));
    aos_x86_config_t unchanged=a;
    assert(io(&a,0x80,1,true,0xa5)==0xa5);
    assert(io(&a,0xed,1,true,0x5a)==0x5a);
    assert(!memcmp(&a,&unchanged,sizeof(a)));
    uint32_t delay=0;
    const unsigned dma_pages[]={0x81,0x82,0x83,0x87,0x89,0x8a,0x8b,0x8f};
    for (unsigned i=0;i<sizeof(dma_pages)/sizeof(dma_pages[0]);i++) {
        assert(io(&a,dma_pages[i],1,false,0)==0xff);
        reject(&a,dma_pages[i],2,false);
        reject(&a,dma_pages[i],1,true);
    }
    reject(&a,0x84,1,false);
    const unsigned com_bases[]={0x3f8,0x2f8,0x3e8,0x2e8};
    for (unsigned i=0;i<4;i++) {
        for (unsigned reg=0;reg<8;reg++) {
            io(&a,com_bases[i]+reg,1,true,0);
            assert(io(&a,com_bases[i]+reg,1,false,0)==0xff);
        }
        reject(&a,com_bases[i],2,false);
        reject(&a,com_bases[i],4,true);
    }
    assert(!memcmp(&a,&unchanged,sizeof(a)));
    assert(!aos_x86_config_io(&a,0xed,1,false,&delay,0));
    assert(!aos_x86_config_io(&a,0xed,2,true,&delay,0));
    assert(!aos_x86_config_io(&a,0xec,1,true,&delay,0));
    assert(!memcmp(&a,&unchanged,sizeof(a)));
    io(&a, 0xaf00, 4, true, 0);
    io(&a, 0xaf05, 1, true, 0);
    assert(io(&a, 0xaf00, 4, false, 0) == 0);
    assert(io(&a, 0xaf04, 1, false, 0) == 1);
    assert(io(&a, 0xaf08, 4, false, 0) == 0);
    io(&a, 0xaf00, 4, true, 1);
    assert(io(&a, 0xaf04, 1, false, 0) == 0);
    assert(io(&a, 0xaf08, 4, false, 0) == 0);
    assert(io(&b, 0xaf04, 1, false, 0) == 1);
    io(&a, 0xaf05, 1, true, 3);
    assert(io(&a, 0xaf00, 4, false, 0) == 0);
    reject(&a, 0xaf04, 1, true);
    reject(&a, 0xaf00, 2, true);
    reject(&a, 0xaf05, 1, true);
    assert(io(&a, 0x92, 1, false, 0) == 2);
    io(&a, 0x92, 1, true, 2);
    reject(&a, 0x92, 1, true);
    io(&a, 0x21, 1, true, 0xff);
    io(&a, 0xa1, 1, true, 0xff);
    assert(io(&a, 0x21, 1, false, 0) == 0xff);
    assert(io(&a, 0xa1, 1, false, 0) == 0xff);
    /* Linux's presence probe must observe no PIC, not a writable mask. */
    aos_x86_config_t before_pic=a;
    io(&a, 0x21, 1, true, 0xfb);
    assert(io(&a, 0x21, 1, false, 0) == 0xff);
    const unsigned pic_ports[]={0x20,0x21,0xa0,0xa1};
    for (unsigned p=0; p<4; p++) {
        for (unsigned v=0; v<256; v++) {
            io(&a,pic_ports[p],1,true,v);
            assert(io(&a,pic_ports[p],1,false,0)==0xff);
        }
        reject(&a,pic_ports[p],2,true);
        reject(&a,pic_ports[p],4,false);
    }
    assert(!memcmp(&a,&before_pic,sizeof(a))); /* no hidden IRQ/controller state */
    reject(&a, 0x22, 1, true);
    reject(&a, 0xa1, 2, false);
    assert(io(&a, 0xcfc, 4, false, 0) == 0xffffffff);
    select_pci(&a, 0x80000000);
    aos_x86_config_t before_partial=a;
    for (unsigned port=0xcf8;port<0xcfc;port++) {
        io(&a,port,1,true,0xff);
        if (!(port&1)) io(&a,port,2,true,0xffff);
    }
    io(&a,0xcfb,1,true,1); /* Linux pci_check_type1 */
    assert(!memcmp(&a,&before_partial,sizeof(a)));
    assert(io(&a,0xcf8,4,false,0)==0x80000000);
    reject(&a,0xcfb,2,true); /* crossing the address/data boundary */
    reject(&a,0xcf9,2,true); /* unaligned */
    reject(&a,0xcfb,1,false); /* no invented partial read behavior */
    assert(io(&a, 0xcfc, 4, false, 0) == 0x12378086);
    assert(io(&a, 0xcfe, 2, false, 0) == 0x1237);
    io(&a, 0xcfc, 4, true, 0);
    assert(io(&a, 0xcfc, 4, false, 0) == 0x12378086);
    assert(io(&b, 0xcf8, 4, false, 0) == 0);
    select_pci(&a, 0x80010000);
    assert(io(&a, 0xcfd, 1, false, 0) == 0xff);
    select_pci(&a, 0x80000b00);
    assert(io(&a, 0xcfc, 4, false, 0) == 0x71138086);
    select_pci(&a, 0x80000b40);
    io(&a, 0xcfc, 4, true, 0x12344001);
    assert(io(&a, 0xcfc, 4, false, 0) == 0x4001);
    reject(&a, 0x4008, 4, false);
    select_pci(&a, 0x80000b04);
    io(&a, 0xcfc, 2, true, 0xffff);
    assert(io(&a, 0xcfc, 2, false, 0) == 7);
    reject(&a, 0x4008, 4, false);
    select_pci(&a, 0x80000b80);
    io(&a, 0xcfc, 1, true, 0xff);
    assert(io(&a, 0x4008, 4, false, 0) == 0x345678);
    select_pci(&a,0x80000b04); io(&a,0xcfc,2,true,0);
    assert(io(&a,0x4008,4,false,0)==0x345678); /* legacy decode ignores PCI CMD */
    select_pci(&a,0x80000b80); io(&a,0xcfc,1,true,0);
    reject(&a,0x4004,2,false);
    io(&a,0xcfc,1,true,1);
    assert(io(&a,0x4000,2,false,0)==1); /* elapsed bit-23 transitions */
    io(&a,0x4000,1,true,1);
    assert(io(&a,0x4000,2,false,0)==0);
    assert(io(&a,0x4002,2,false,0)==0);
    io(&a,0x4002,2,true,0);
    io(&a,0x4002,2,true,0x20); /* absent global-lock enable must not stick */
    assert(io(&a,0x4002,2,false,0)==0);
    io(&a,0x4002,1,true,0x20);
    assert(io(&a,0x4002,1,false,0)==0);
    io(&a,0x4002,2,true,0x420); /* no global-lock or RTC-wake SCI */
    assert(io(&a,0x4002,2,false,0)==0);
    io(&a,0x4003,1,true,4);
    assert(io(&a,0x4003,1,false,0)==0);
    assert(io(&a,0x4004,2,false,0)==0);
    io(&a,0x4004,2,true,0x1c03);
    assert(io(&a,0x4004,1,false,0)==3 && io(&a,0x4005,1,false,0)==0x1c);
    io(&a,0x4004,1,true,0);
    assert(io(&a,0x4004,2,false,0)==0x1c00);
    io(&a,0x4005,1,true,0);
    assert(io(&a,0x4004,2,false,0)==0);
    reject(&a,0x4001,2,false);
    reject(&a,0x4004,4,false);
    reject(&a,0x4006,2,false);
    uint32_t pm_value=0x2000; aos_x86_config_t before=a;
    assert(!aos_x86_config_io(&a,0x4004,2,true,&pm_value,0x12345678)); /* sleep */
    assert(pm_value==0x2000 && !memcmp(&a,&before,sizeof(a)));
    pm_value=4;
    assert(!aos_x86_config_io(&a,0x4004,2,true,&pm_value,0x12345678)); /* SMI */
    assert(!memcmp(&a,&before,sizeof(a)));
    pm_value=1;
    assert(!aos_x86_config_io(&a,0x4002,2,true,&pm_value,0x12345678)); /* SCI */
    assert(!memcmp(&a,&before,sizeof(a)));
    pm_value=0;
    assert(!aos_x86_config_io(&a,0x4000,2,false,&pm_value,0)); /* reversal */
    assert(!memcmp(&a,&before,sizeof(a)));
    reject(&b, 0x4008, 4, false);
    aos_x86_config_t wrapped=a;
    pm_value=0;
    assert(aos_x86_config_io(&wrapped,0x4000,2,false,&pm_value,0x13345678));
    assert(pm_value==1); /* two bit-23 transitions still latch status */
    pm_value=1;
    assert(aos_x86_config_io(&wrapped,0x4000,2,true,&pm_value,0x13345678));
    pm_value=0;
    assert(aos_x86_config_io(&wrapped,0x4000,2,false,&pm_value,0x13345678));
    assert(pm_value==0 && a.pm_status==0);
    reject(&a, 0x4008, 4, true);
    reject(&a, 0x4008, 2, false);
    reject(&a, 0xcfd, 2, false);
    reject(&a, 0xcf8, 2, false);
    reject(&a, 0x511, 4, false);
    reject(&a, 0x1234, 1, false);
    reject(&a, 0xcf8, 3, true);
    uint8_t data[100];
    fw(&a, 0, data, 6);
    assert(!memcmp(data, "QEMU\0\0", 6));
    fw(&a, 1, data, 4); assert(read_le(data, 4) == 1);
    fw(&a, 3, data, 8); assert(read_le(data, 8) == 0x2000000);
    fw(&b, 3, data, 8); assert(read_le(data, 8) == 0x80000000);
    fw(&a, 5, data, 2); assert(read_le(data, 2) == 1);
    fw(&a, 0xf, data, 2); assert(read_le(data, 2) == 1);
    fw(&a, 0x19, data, 68);
    assert(data[0] == 0 && data[3] == 1 && data[7] == 80 && data[9] == 0x20);
    assert(!strcmp((char *)data+12, "etc/e820"));
    fw(&a, 0x20, data, 100);
    assert(read_le(data, 8) == 0 && read_le(data+8, 8) == 0xa0000);
    assert(read_le(data+16, 4) == 1);
    assert(read_le(data+20, 8) == 0xa0000 && read_le(data+28, 8) == 0x60000);
    assert(read_le(data+36, 4) == 2);
    assert(read_le(data+40, 8) == 0x100000 && read_le(data+48, 8) == 0x1f00000);
    assert(read_le(data+56, 4) == 1);
    assert(read_le(data+60, 8) == 0xffc00000 && read_le(data+68, 8) == 0x400000);
    assert(read_le(data+76, 4) == 2);
    for (unsigned i = 80; i < 100; ++i) assert(data[i] == 0);
    fw(&a, 0xffff, data, 100);
    for (unsigned i = 0; i < 100; ++i) assert(data[i] == 0);
    io(&a, 0x70, 1, true, 0xb4); assert(io(&a, 0x71, 1, false, 0) == 0);
    io(&a, 0x70, 1, true, 0x35); assert(io(&a, 0x71, 1, false, 0) == 1);
    assert(io(&a,0x70,1,false,0)==0xff);
    assert(io(&a,0x71,1,false,0)==1); /* index read does not change selection */
    io(&a, 0x70, 1, true, 0xf); assert(io(&a, 0x71, 1, false, 0) == 0);
    io(&a, 0x71, 1, true, 0);
    reject(&a, 0x71, 1, true);
    io(&a,0x70,1,true,0xa); io(&a,0x71,1,true,0x26);
    assert((io(&a,0x71,1,false,0)&0x7f)==0x26);
    io(&a,0x70,1,true,1); assert(io(&a,0x71,1,false,0)==0);
    io(&a, 0x70, 1, true, 0x33); reject(&a, 0x71, 1, false);
    puts("PASS: private PCI config, PM timer decoding, firmware directory/E820 and rejected I/O");
    return 0;
}
