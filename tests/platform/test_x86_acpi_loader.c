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
static aos_x86_acpi_topology_t topology(unsigned count)
{
    aos_x86_acpi_topology_t t={.lapic_gpa=0xfee00000u,
        .ioapic_gpa=0xfec00000u,.ioapic_id=1,.cpu_count=(uint8_t)count};
    for (unsigned i=0;i<count;i++) {
        t.cpus[i].uid=(uint8_t)(255u-i);
        t.cpus[i].apic_id=(uint8_t)(i ? i+1u : 0u); /* skip I/O APIC ID */
    }
    return t;
}
static void relocate(uint64_t base, unsigned count)
{
    aos_x86_acpi_bundle_t source, copy={0};
    aos_x86_config_t c;
    aos_x86_acpi_topology_t t=topology(count);
    assert(aos_x86_acpi_bundle_topology(&source,&t));
    assert(source.cpu_count==count && source.table_bytes==846u+37u*(count-1u));
    /* Capacity outside the actual bundle must never escape through fw_cfg. */
    memset(source.tables+source.table_bytes,0xa5,sizeof(source.tables)-source.table_bytes);
    assert(aos_x86_config_init(&c,256u*1024u*1024u));
    assert(!aos_x86_config_acpi(NULL,&source));
    assert(!aos_x86_config_acpi(&c,NULL));
    assert(aos_x86_config_acpi(&c,&source));
    aos_x86_config_t before=c;
    assert(!aos_x86_config_acpi(&c,&source) && !memcmp(&c,&before,sizeof(c)));
    uint8_t dir[261]; fw(&c,0x19,dir,sizeof(dir));
    assert(dir[0]==0 && dir[1]==0 && dir[2]==0 && dir[3]==4 && dir[260]==0);
    const char *names[]={"etc/e820","etc/acpi/tables","etc/acpi/rsdp","etc/table-loader"};
    const unsigned lengths[]={80,source.table_bytes,sizeof(source.rsdp),sizeof(source.loader)};
    for (unsigned i=0;i<4;i++) {
        const uint8_t *d=dir+4+64*i;
        unsigned length=(unsigned)d[0]<<24 | (unsigned)d[1]<<16 | (unsigned)d[2]<<8 | d[3];
        assert(length==lengths[i] && d[4]==0 && d[5]==0x20+i && !d[6] && !d[7]);
        assert(!strcmp((const char *)d+8,names[i]));
    }
    fw(&c,0x21,copy.tables,sizeof(copy.tables));
    fw(&c,0x22,copy.rsdp,sizeof(copy.rsdp));
    fw(&c,0x23,copy.loader,sizeof(copy.loader));
    assert(!memcmp(copy.tables,source.tables,source.table_bytes));
    assert(!memcmp(copy.rsdp,source.rsdp,sizeof(copy.rsdp)));
    assert(!memcmp(copy.loader,source.loader,sizeof(copy.loader)));
    for (unsigned i=source.table_bytes;i<sizeof(copy.tables);i++) assert(!copy.tables[i]);
    uint32_t past=123;
    /* The preceding RSDP/loader reads do not inspect the tables' EOF. */
    fw(&c,0x21,copy.tables,sizeof(copy.tables));
    assert(aos_x86_config_io(&c,0x511,1,false,&past,0) && !past);
    uint8_t cpus[4];
    fw(&c,5,cpus,sizeof(cpus));
    assert(read_le(cpus,4)==count);
    fw(&c,0xf,cpus,sizeof(cpus));
    assert(read_le(cpus,4)==count);
    uint8_t *files[]={copy.tables,copy.rsdp};
    const unsigned sizes[]={source.table_bytes,sizeof(copy.rsdp)};
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
    assert(rsdt==738u+37u*(count-1u) && xsdt==rsdt+48u);
    assert(!sum(copy.tables+rsdt,48) && !sum(copy.tables+xsdt,60));
    const char *signatures[]={"FACP","APIC","SSDT"};
    unsigned fadt=0;
    for (unsigned i=0;i<3;i++) {
        uint64_t addr=read_le(copy.tables+rsdt+36+4*i,4);
        assert(addr==read_le(copy.tables+xsdt+36+8*i,8));
        unsigned off=(unsigned)(addr-base), len=(unsigned)read_le(copy.tables+off+4,4);
        assert(off+len<=source.table_bytes && !sum(copy.tables+off,len));
        assert(!memcmp(copy.tables+off,signatures[i],4));
        if (!i) fadt=off;
        if (i==1) {
            const uint8_t *m=copy.tables+off;
            assert(len==56u+8u*count && read_le(m+36,4)==t.lapic_gpa);
            for (unsigned cpu=0;cpu<count;cpu++) {
                const uint8_t *entry=m+44u+8u*cpu;
                assert(entry[0]==0 && entry[1]==8 && entry[2]==t.cpus[cpu].uid);
                assert(entry[3]==t.cpus[cpu].apic_id && read_le(entry+4,4)==1);
            }
            const uint8_t *io=m+44u+8u*count;
            assert(io[0]==1 && io[1]==12 && io[2]==t.ioapic_id);
            assert(read_le(io+4,4)==t.ioapic_gpa && !read_le(io+8,4));
        }
        if (i==2) {
            const uint8_t *s=copy.tables+off;
            assert(len==44u+29u*count);
            assert(s[36]==0x10 && (s[37]&0xc0u)==0x40u);
            assert((s[37]&15u)+((unsigned)s[38]<<4)==7u+29u*count);
            for (unsigned cpu=0;cpu<count;cpu++) {
                const uint8_t *d=s+44u+29u*cpu;
                assert(d[0]==0x5b && d[1]==0x82 && d[2]==27);
                assert(!memcmp(d+13,"ACPI0007",9));
                assert(!memcmp(d+23,"_UID",4) && d[27]==0x0a);
                assert(d[28]==t.cpus[cpu].uid);
            }
        }
    }
    const uint8_t *f=copy.tables+fadt;
    assert(read_le(f+36,4)==base && read_le(f+132,8)==base);
    assert(read_le(f+40,4)==base+64 && read_le(f+140,8)==base+64);
    assert(!memcmp(copy.tables,"FACS",4) && read_le(copy.tables+4,4)==64);
    const uint8_t *dsdt=copy.tables+64;
    assert(!memcmp(dsdt,"DSDT",4) && read_le(dsdt+4,4)==261 && !sum(dsdt,261));
    const char *devices[] = {"VCON", "VBLK", "VNET"};
    for (unsigned i = 0; i < 3; i++) {
        const uint8_t *d = dsdt + 36 + i * 75;
        assert(!memcmp(d+11,devices[i],4) && d[35]==0x0a && d[36]==i);
        assert(!memcmp(d+21,"LNRO0005",9));
        assert(d[52]==0x86 && read_le(d+53,2)==9 && d[55]==1);
        assert(read_le(d+56,4)==0xf0000000u+i*4096u && read_le(d+60,4)==4096);
        assert(d[64]==0x89 && read_le(d+65,2)==6);
        assert(d[67]==1 && d[68]==1 && read_le(d+69,4)==16+i);
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
static void invalid_topology(void)
{
    aos_x86_acpi_bundle_t bundle, before;
    memset(&bundle,0xa5,sizeof(bundle)); before=bundle;
    aos_x86_acpi_topology_t valid=topology(2), bad=valid;
    assert(!aos_x86_acpi_bundle_topology(NULL,&valid));
    assert(!aos_x86_acpi_bundle_topology(&bundle,NULL));
    for (unsigned i=0;i<9;i++) {
        bad=valid;
        switch (i) {
        case 0: bad.cpu_count=0; break;
        case 1: bad.cpu_count=AOS_X86_ACPI_MAX_CPUS+1; break;
        case 2: bad.cpus[1].apic_id=bad.cpus[0].apic_id; break;
        case 3: bad.cpus[1].uid=bad.cpus[0].uid; break;
        case 4: bad.cpus[1].apic_id=bad.ioapic_id; break;
        case 5: bad.cpus[1].apic_id=255; break;
        case 6: bad.lapic_gpa+=4096; break;
        case 7: bad.ioapic_gpa+=4096; break;
        case 8: bad.gsi_base=1; break;
        }
        assert(!aos_x86_acpi_bundle_topology(&bundle,&bad));
        assert(!memcmp(&bundle,&before,sizeof(bundle)));
    }
    /* Topology may reside inside the output; it must be snapshotted first. */
    assert(aos_x86_acpi_bundle_topology(&before,&valid));
    memcpy(bundle.tables,&valid,sizeof(valid));
    assert(aos_x86_acpi_bundle_topology(&bundle,(const void *)bundle.tables));
    assert(!memcmp(&bundle,&before,sizeof(bundle)));
    aos_x86_config_t config, saved;
    assert(aos_x86_config_init(&config,256u*1024u*1024u)); saved=config;
    for (unsigned i=0;i<4;i++) {
        bundle=before;
        if (i==0) bundle.cpu_count=0;
        if (i==1) bundle.cpu_count=AOS_X86_ACPI_MAX_CPUS+1;
        if (i==2) bundle.table_bytes=UINT32_MAX;
        if (i==3) bundle.table_bytes--;
        assert(!aos_x86_config_acpi(&config,&bundle));
        assert(!memcmp(&config,&saved,sizeof(config)));
    }
    /* Default wrapper retains the historical one-CPU byte layout. */
    valid=topology(1); valid.cpus[0].uid=0;
    assert(aos_x86_acpi_bundle_topology(&before,&valid));
    assert(aos_x86_acpi_bundle_init(&bundle));
    assert(!memcmp(&bundle,&before,sizeof(bundle)));
}
int main(int argc, char **argv)
{
    if (argc>2) return 2;
    invalid_topology();
    for (unsigned count=1;count<=AOS_X86_ACPI_MAX_CPUS;count++) {
        relocate(0x100000,count); relocate(0x12340000,count); relocate(0xfffe0000,count);
    }
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
