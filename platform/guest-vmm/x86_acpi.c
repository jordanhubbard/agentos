#include <platform/x86_acpi.h>
#include <string.h>

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
