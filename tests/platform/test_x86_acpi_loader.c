#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "platform/x86_config.h"

static uint64_t read_le(const uint8_t *p, unsigned n)
{
    uint64_t v=0; for (unsigned i=0;i<n;i++) v|=(uint64_t)p[i]<<(8*i);
    return v;
}
static void write_le(uint8_t *p, uint64_t v, unsigned n)
{
    for (unsigned i=0;i<n;i++) p[i]=(uint8_t)(v>>(8*i));
}
static uint8_t sum(const uint8_t *p, unsigned n)
{
    uint8_t v=0; for (unsigned i=0;i<n;i++) v=(uint8_t)(v+p[i]); return v;
}
static void fw(aos_x86_config_t *s, unsigned selector, uint8_t *p, unsigned n)
{
    uint32_t v=selector;
    assert(aos_x86_config_io(s,0x510,2,true,&v,0));
    for (unsigned i=0;i<n;i++) {
        assert(aos_x86_config_io(s,0x511,1,false,&v,0)); p[i]=(uint8_t)v;
    }
}
static unsigned file_index(const uint8_t *name)
{
    assert(memchr(name,0,56));
    if (!strcmp((const char *)name,"etc/acpi/tables")) return 0;
    assert(!strcmp((const char *)name,"etc/acpi/rsdp")); return 1;
}
static void relocate(uint64_t base)
{
    aos_x86_acpi_bundle_t source, copy;
    aos_x86_config_t c;
    assert(aos_x86_acpi_bundle_init(&source));
    assert(aos_x86_config_init(&c,256u*1024u*1024u));
    assert(!aos_x86_config_acpi(NULL,&source));
    assert(!aos_x86_config_acpi(&c,NULL));
    assert(aos_x86_config_acpi(&c,&source));
    aos_x86_config_t before=c;
    assert(!aos_x86_config_acpi(&c,&source) && !memcmp(&c,&before,sizeof(c)));
    uint8_t dir[261]; fw(&c,0x19,dir,sizeof(dir));
    assert(dir[0]==0 && dir[1]==0 && dir[2]==0 && dir[3]==4 && dir[260]==0);
    const char *names[]={"etc/e820","etc/acpi/tables","etc/acpi/rsdp","etc/table-loader"};
    const unsigned lengths[]={80,sizeof(source.tables),sizeof(source.rsdp),sizeof(source.loader)};
    for (unsigned i=0;i<4;i++) {
        const uint8_t *d=dir+4+64*i;
        unsigned length=(unsigned)d[0]<<24 | (unsigned)d[1]<<16 | (unsigned)d[2]<<8 | d[3];
        assert(length==lengths[i] && d[4]==0 && d[5]==0x20+i && !d[6] && !d[7]);
        assert(!strcmp((const char *)d+8,names[i]));
    }
    fw(&c,0x21,copy.tables,sizeof(copy.tables));
    fw(&c,0x22,copy.rsdp,sizeof(copy.rsdp));
    fw(&c,0x23,copy.loader,sizeof(copy.loader));
    assert(!memcmp(&copy,&source,sizeof(copy)));
    uint8_t *files[]={copy.tables,copy.rsdp};
    const unsigned sizes[]={sizeof(copy.tables),sizeof(copy.rsdp)};
    const uint64_t bases[]={base,base+0x10000};
    bool allocated[2]={false,false};
    unsigned allocations=0, pointers=0, checksums=0;
    for (unsigned offset=0;offset<sizeof(copy.loader);offset+=128) {
        const uint8_t *cmd=copy.loader+offset;
        unsigned type=(unsigned)read_le(cmd,4), file=file_index(cmd+4);
        if (type==1) {
            unsigned align=(unsigned)read_le(cmd+60,4);
            assert(!allocated[file] && align && !(align&(align-1)));
            assert(!(bases[file]&(align-1)) && cmd[64]==1);
            allocated[file]=true; allocations++;
        } else if (type==2) {
            unsigned src=file_index(cmd+60), off=(unsigned)read_le(cmd+116,4), width=cmd[120];
            assert(allocated[file] && allocated[src] && (width==4 || width==8));
            assert(off<=sizes[file] && width<=sizes[file]-off);
            uint64_t relative=read_le(files[file]+off,width);
            assert(relative<sizes[src]);
            assert(width==8 || relative+bases[src]<=UINT32_MAX);
            write_le(files[file]+off,relative+bases[src],width); pointers++;
        } else {
            assert(type==3 && allocated[file]);
            unsigned result=(unsigned)read_le(cmd+60,4), start=(unsigned)read_le(cmd+64,4);
            unsigned length=(unsigned)read_le(cmd+68,4);
            assert(start<=sizes[file] && length<=sizes[file]-start);
            assert(result>=start && result-start<length && files[file][result]==0);
            files[file][result]=(uint8_t)(0u-sum(files[file]+start,length)); checksums++;
        }
    }
    assert(allocations==2 && pointers==12 && checksums==5);
    assert(!sum(copy.rsdp,20) && !sum(copy.rsdp,36));
    assert(!memcmp(copy.rsdp,"RSD PTR ",8) && copy.rsdp[15]==2);
    unsigned rsdt=(unsigned)(read_le(copy.rsdp+16,4)-base);
    unsigned xsdt=(unsigned)(read_le(copy.rsdp+24,8)-base);
    assert(rsdt==661 && xsdt==709);
    assert(!sum(copy.tables+rsdt,48) && !sum(copy.tables+xsdt,60));
    const char *signatures[]={"FACP","APIC","SSDT"};
    unsigned fadt=0;
    for (unsigned i=0;i<3;i++) {
        uint64_t addr=read_le(copy.tables+rsdt+36+4*i,4);
        assert(addr==read_le(copy.tables+xsdt+36+8*i,8));
        unsigned off=(unsigned)(addr-base), len=(unsigned)read_le(copy.tables+off+4,4);
        assert(off+len<=sizeof(copy.tables) && !sum(copy.tables+off,len));
        assert(!memcmp(copy.tables+off,signatures[i],4));
        if (!i) fadt=off;
    }
    const uint8_t *f=copy.tables+fadt;
    assert(read_le(f+36,4)==base && read_le(f+132,8)==base);
    assert(read_le(f+40,4)==base+64 && read_le(f+140,8)==base+64);
    assert(!memcmp(copy.tables,"FACS",4) && read_le(copy.tables+4,4)==64);
    const uint8_t *dsdt=copy.tables+64;
    assert(!memcmp(dsdt,"DSDT",4) && read_le(dsdt+4,4)==184 && !sum(dsdt,184));
    const char *devices[] = {"VCON", "VBLK"};
    for (unsigned i = 0; i < 2; i++) {
        const uint8_t *d = dsdt + 36 + i * 74;
        assert(!memcmp(d+11,devices[i],4) && d[35]==i);
        assert(!memcmp(d+21,"LNRO0005",9));
        assert(d[51]==0x86 && read_le(d+52,2)==9 && d[54]==1);
        assert(read_le(d+55,4)==0xf0000000u+i*4096u && read_le(d+59,4)==4096);
        assert(d[63]==0x89 && read_le(d+64,2)==6);
        assert(d[66]==1 && d[67]==1 && read_le(d+68,4)==16+i);
    }
    assert(read_le(f+56,4)==0xb000 && read_le(f+64,4)==0xb004 && read_le(f+76,4)==0xb008);
    assert(f[88]==4 && f[89]==2 && f[91]==4 && read_le(f+112,4)==0x70);
    uint32_t v=0;
    assert(aos_x86_config_io(&c,0xb004,2,false,&v,0) && v==1);
    assert(aos_x86_config_io(&c,0xb008,4,false,&v,12345) && v==12345);
    v=0x30; assert(aos_x86_config_io(&c,0x43,1,true,&v,12345));
    v=0; assert(aos_x86_config_io(&c,0x40,1,true,&v,12345));
    assert(aos_x86_config_io(&c,0x40,1,true,&v,12345));
    before=c;
    assert(!aos_x86_config_io(&c,0x40,1,true,&v,12345));
    v=0x34; assert(!aos_x86_config_io(&c,0x43,1,true,&v,12345));
    v=0xa9; assert(!aos_x86_config_io(&c,0x40,1,true,&v,12345));
    assert(!aos_x86_config_io(&c,0x40,1,false,&v,12345));
    assert(!memcmp(&c,&before,sizeof(c)));
    assert(!aos_x86_acpi_bundle_init(NULL));
    assert(aos_x86_config_init(&c,256u*1024u*1024u));
    fw(&c,0,dir,1); before=c;
    assert(!aos_x86_config_acpi(&c,&source) && !memcmp(&c,&before,sizeof(c)));
}
int main(int argc, char **argv)
{
    if (argc>2) return 2;
    relocate(0x100000); relocate(0x12340000); relocate(0xfffe0000);
    if (argc==2) {
        aos_x86_acpi_bundle_t bundle;
        assert(aos_x86_acpi_bundle_init(&bundle));
        FILE *file=fopen(argv[1],"wb");
        unsigned length = (unsigned)read_le(bundle.tables+68,4);
        assert(file && fwrite(bundle.tables+64,1,length,file)==length);
        assert(!fclose(file));
    }
    puts("PASS: fw_cfg ACPI directory, bounded relocations, root/table checksums and PM contract");
    return 0;
}
