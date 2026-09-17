#include <platform/x86_acpi.h>
#include <string.h>
#include <platform/x86_apic.h>
#include <platform/x86_ioapic.h>

static void le32(uint8_t *p, uint32_t n)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(n >> (8u * i));
}

static int topology_valid(const aos_x86_acpi_topology_t *topology)
{
    const aos_x86_acpi_topology_t t = *topology;
    if (!t.cpu_count || t.cpu_count > AOS_X86_ACPI_MAX_CPUS ||
        !t.lapic_gpa || !t.ioapic_gpa ||
        (t.lapic_gpa & 4095u) || (t.ioapic_gpa & 4095u) ||
        t.lapic_gpa == t.ioapic_gpa || t.ioapic_id > 15u ||
        t.gsi_base > UINT32_MAX - 23u) return 0;
    for (unsigned i = 0; i < t.cpu_count; ++i) {
        if (t.cpus[i].apic_id == 255u || t.cpus[i].apic_id == t.ioapic_id)
            return 0;
        for (unsigned j = 0; j < i; ++j)
            if (t.cpus[i].uid == t.cpus[j].uid ||
                t.cpus[i].apic_id == t.cpus[j].apic_id) return 0;
    }
    return 1;
}
static void header(uint8_t *p, size_t length, const char *signature,
                   uint8_t revision, const char *table_id)
{
    memset(p, 0, length);
    memcpy(p, signature, 4);
    le32(p + 4, (uint32_t)length);
    p[8] = revision;
    memcpy(p + 10, "AGNTOS", 6);
    memcpy(p + 16, table_id, 8);
    le32(p + 24, 1);
    memcpy(p + 28, "AOSC", 4);
    le32(p + 32, 1);
}
static void checksum(uint8_t *p, size_t length)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < length; ++i) sum = (uint8_t)(sum + p[i]);
    p[9] = (uint8_t)(0u - sum);
}
size_t aos_x86_madt_write(void *output, size_t capacity,
                         const aos_x86_acpi_topology_t *topology)
{
    if (!output || !topology) return 0;
    /* Snapshot configuration before writing, including when output aliases it. */
    const aos_x86_acpi_topology_t t = *topology;
    if (!topology_valid(&t)) return 0;
    const size_t length = 44u + 8u * t.cpu_count + 12u;
    if (capacity < length) return 0;
    uint8_t *p = output;
    header(p, length, "APIC", 7, "AOSX86  ");
    le32(p + 36, t.lapic_gpa);
    /* PCAT_COMPAT stays zero: no claim of a VMM-emulated legacy 8259. */
    size_t offset = 44;
    for (unsigned i = 0; i < t.cpu_count; ++i, offset += 8) {
        p[offset + 1] = 8;
        p[offset + 2] = t.cpus[i].uid;
        p[offset + 3] = t.cpus[i].apic_id;
        le32(p + offset + 4, 1); /* enabled, not online-capable/hotplug */
    }
    p[offset] = 1;
    p[offset + 1] = 12;
    p[offset + 2] = t.ioapic_id;
    le32(p + offset + 4, t.ioapic_gpa);
    le32(p + offset + 8, t.gsi_base);
    checksum(p, length);
    return length;
}

size_t aos_x86_cpu_ssdt_write(void *output, size_t capacity,
                             const aos_x86_acpi_topology_t *topology)
{
    if (!output || !topology) return 0;
    const aos_x86_acpi_topology_t t = *topology;
    if (!topology_valid(&t)) return 0;
    const size_t length = 44u + 29u * t.cpu_count;
    if (capacity < length) return 0;
    uint8_t *p = output;
    header(p, length, "SSDT", 2, "AOSCPU  ");
    /* Scope(\\_SB_) uses the two-byte AML package length form. Its length
     * includes the length encoding itself, but excludes the Scope opcode. */
    unsigned package = 7u + 29u * t.cpu_count;
    p[36] = 0x10;
    p[37] = (uint8_t)(0x40u | (package & 15u));
    p[38] = (uint8_t)(package >> 4);
    memcpy(p + 39, "\\_SB_", 5);
    static const uint8_t device[] = {
        0x5b,0x82,27,'C','0','0','0',
        0x08,'_','H','I','D',0x0d,'A','C','P','I','0','0','0','7',0,
        0x08,'_','U','I','D',0x0a,0
    };
    _Static_assert(sizeof(device) == 29, "CPU AML device length");
    static const char hex[] = "0123456789ABCDEF";
    for (unsigned i = 0; i < t.cpu_count; ++i) {
        uint8_t *d = p + 44u + i * sizeof(device);
        memcpy(d, device, sizeof(device));
        d[5] = (uint8_t)hex[i >> 4];
        d[6] = (uint8_t)hex[i & 15u];
        d[28] = t.cpus[i].uid;
    }
    checksum(p, length);
    return length;
}

/* Standard QEMU firmware table-loader commands, each 128 bytes. See
 * edk2-stable202402/OvmfPkg/Include/IndustryStandard/QemuLoader.h. */
static const char tables_file[] = "etc/acpi/tables";
static const char rsdp_file[] = "etc/acpi/rsdp";
static uint8_t *allocate(uint8_t *p, const char *file, unsigned align)
{
    le32(p,1); memcpy(p+4,file,strlen(file)+1); le32(p+60,align);
    p[64]=1; return p+128;
}
static uint8_t *pointer(uint8_t *p, const char *file, unsigned offset, unsigned size)
{
    le32(p,2); memcpy(p+4,file,strlen(file)+1);
    memcpy(p+60,tables_file,sizeof(tables_file));
    le32(p+116,offset); p[120]=(uint8_t)size; return p+128;
}
static uint8_t *sum_command(uint8_t *p, const char *file, unsigned result,
                            unsigned start, unsigned length)
{
    le32(p,3); memcpy(p+4,file,strlen(file)+1);
    le32(p+60,result); le32(p+64,start); le32(p+68,length); return p+128;
}
static void io_gas(uint8_t *p, unsigned width, unsigned access, unsigned port)
{
    p[0]=1; p[1]=(uint8_t)width; p[3]=(uint8_t)access; le32(p+4,port);
}
bool aos_x86_acpi_bundle_init(aos_x86_acpi_bundle_t *bundle)
{
    if (!bundle) return false;
    enum { FACS=0, DSDT=64, FADT=100, MADT=376, SSDT=440, RSDT=513, XSDT=561 };
    const aos_x86_acpi_topology_t t={.lapic_gpa=AOS_X86_APIC_BASE,
        .ioapic_gpa=AOS_X86_IOAPIC_BASE,.ioapic_id=1,.cpu_count=1,
        .cpus={{.uid=0,.apic_id=0}}};
    memset(bundle,0,sizeof(*bundle));
    uint8_t *b=bundle->tables;
    memcpy(b+FACS,"FACS",4); le32(b+FACS+4,64); b[FACS+32]=2;
    header(b+DSDT,36,"DSDT",2,"AOSDSDT "); checksum(b+DSDT,36);
    header(b+FADT,276,"FACP",6,"AOSFADT ");
    le32(b+FADT+36,FACS); le32(b+FADT+40,DSDT);
    b[FADT+46]=9; /* SCI GSI, currently no enabled event sources */
    le32(b+FADT+56,0xb000); le32(b+FADT+64,0xb004); le32(b+FADT+76,0xb008);
    b[FADT+88]=4; b[FADT+89]=2; b[FADT+91]=4;
    memset(b+FADT+96,0xff,4); /* no C2/C3 */
    b[FADT+108]=0x32; /* RTC century register */
    b[FADT+109]=0x0c; /* no VGA or MSI, no 8042/legacy-device claim */
    le32(b+FADT+112,0x70); /* no fixed power/sleep buttons or RTC wake */
    b[FADT+131]=5;
    le32(b+FADT+132,FACS); le32(b+FADT+140,DSDT);
    io_gas(b+FADT+148,32,2,0xb000); io_gas(b+FADT+172,16,2,0xb004);
    io_gas(b+FADT+208,32,3,0xb008);
    if (aos_x86_madt_write(b+MADT,64,&t)!=64 ||
        aos_x86_cpu_ssdt_write(b+SSDT,73,&t)!=73) return false;
    header(b+RSDT,48,"RSDT",1,"AOSROOT ");
    header(b+XSDT,60,"XSDT",1,"AOSROOT ");
    const unsigned entries[]={FADT,MADT,SSDT};
    for (unsigned i=0;i<3;i++) {
        le32(b+RSDT+36+4*i,entries[i]); le32(b+XSDT+36+8*i,entries[i]);
    }
    memcpy(bundle->rsdp,"RSD PTR ",8); memcpy(bundle->rsdp+9,"AGNTOS",6);
    bundle->rsdp[15]=2; le32(bundle->rsdp+16,RSDT);
    le32(bundle->rsdp+20,36); le32(bundle->rsdp+24,XSDT);
    uint8_t *p=allocate(bundle->loader,tables_file,64);
    p=allocate(p,rsdp_file,16);
    p=pointer(p,tables_file,FADT+36,4); p=pointer(p,tables_file,FADT+40,4);
    p=pointer(p,tables_file,FADT+132,8); p=pointer(p,tables_file,FADT+140,8);
    for (unsigned i=0;i<3;i++) {
        p=pointer(p,tables_file,RSDT+36+4*i,4);
        p=pointer(p,tables_file,XSDT+36+8*i,8);
    }
    p=pointer(p,rsdp_file,16,4); p=pointer(p,rsdp_file,24,8);
    p=sum_command(p,tables_file,FADT+9,FADT,276);
    p=sum_command(p,tables_file,RSDT+9,RSDT,48);
    p=sum_command(p,tables_file,XSDT+9,XSDT,60);
    p=sum_command(p,rsdp_file,8,0,20);
    p=sum_command(p,rsdp_file,32,0,36);
    return p==bundle->loader+sizeof(bundle->loader);
}
