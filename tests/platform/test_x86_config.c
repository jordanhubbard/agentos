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
    assert(!aos_x86_config_io(s, port, width, write, &value, 0));
    assert(value == 0xabcdef01 && !memcmp(s, &before, sizeof(before)));
}
int main(void)
{
    aos_x86_config_t a, b;
    assert(!aos_x86_config_init(NULL, 0x2000000));
    assert(!aos_x86_config_init(&a, 0x1000000));
    assert(!aos_x86_config_init(&a, 0x2000001));
    assert(!aos_x86_config_init(&a, 0x80010000));
    assert(aos_x86_config_init(&a, 0x2000000));
    assert(aos_x86_config_init(&b, 0x80000000));
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
    reject(&a, 0x21, 1, true);
    reject(&a, 0x20, 1, true);
    reject(&a, 0xa1, 2, false);
    assert(io(&a, 0xcfc, 4, false, 0) == 0xffffffff);
    select_pci(&a, 0x80000000);
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
    reject(&b, 0x4008, 4, false);
    reject(&a, 0x4008, 4, true);
    reject(&a, 0x4008, 2, false);
    reject(&a, 0xcfd, 2, false);
    reject(&a, 0xcf8, 2, true);
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
    io(&a, 0x70, 1, true, 0); reject(&a, 0x71, 1, false);
    puts("PASS: private PCI config, PM timer decoding, firmware directory/E820 and rejected I/O");
    return 0;
}
