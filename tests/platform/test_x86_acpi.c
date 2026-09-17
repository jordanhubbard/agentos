#include <platform/x86_acpi.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint8_t expected[] = {
    'A','P','I','C',72,0,0,0,7,199,'A','G','N','T','O','S',
    'A','O','S','X','8','6',' ',' ',1,0,0,0,'A','O','S','C',1,0,0,0,
    0,0,0xe0,0xfe,0,0,0,0,
    0,8,3,0,1,0,0,0, 0,8,9,2,1,0,0,0,
    1,12,7,0,0,0,0xc0,0xfe,32,0,0,0
};
static void rejects(aos_x86_acpi_topology_t t, size_t capacity)
{
    uint8_t bytes[AOS_X86_MADT_MAX_BYTES + 2];
    memset(bytes, 0xa5, sizeof(bytes));
    assert(aos_x86_madt_write(bytes + 1, capacity, &t) == 0);
    for (size_t i = 0; i < sizeof(bytes); ++i) assert(bytes[i] == 0xa5);
}
int main(void)
{
    aos_x86_acpi_topology_t t = {
        .lapic_gpa = 0xfee00000u, .ioapic_gpa = 0xfec00000u,
        .gsi_base = 32, .ioapic_id = 7, .cpu_count = 2,
        .cpus = {{3,0},{9,2}}
    };
    uint8_t bytes[AOS_X86_MADT_MAX_BYTES + 2];
    memset(bytes, 0xa5, sizeof(bytes));
    assert(aos_x86_madt_write(bytes + 1, sizeof(expected), &t) == sizeof(expected));
    assert(memcmp(bytes + 1, expected, sizeof(expected)) == 0);
    assert(bytes[0] == 0xa5 && bytes[sizeof(expected) + 1] == 0xa5);
    rejects(t, sizeof(expected) - 1);
    aos_x86_acpi_topology_t bad = t;
    bad.cpu_count = 0; rejects(bad, sizeof(bytes) - 2);
    bad.cpu_count = 33; rejects(bad, sizeof(bytes) - 2);
    bad = t; bad.cpus[1].uid = 3; rejects(bad, sizeof(bytes) - 2);
    bad = t; bad.cpus[1].apic_id = 0; rejects(bad, sizeof(bytes) - 2);
    bad = t; bad.cpus[1].apic_id = 255; rejects(bad, sizeof(bytes) - 2);
    bad = t; bad.ioapic_id = 2; rejects(bad, sizeof(bytes) - 2);
    bad = t; bad.ioapic_id = 16; rejects(bad, sizeof(bytes) - 2);
    bad = t; bad.lapic_gpa = 0; rejects(bad, sizeof(bytes) - 2);
    bad = t; bad.ioapic_gpa++; rejects(bad, sizeof(bytes) - 2);
    bad = t; bad.ioapic_gpa = bad.lapic_gpa; rejects(bad, sizeof(bytes) - 2);
    bad = t; bad.gsi_base = UINT32_MAX - 22; rejects(bad, sizeof(bytes) - 2);
    assert(aos_x86_madt_write(NULL, sizeof(bytes), &t) == 0);
    assert(aos_x86_madt_write(bytes, sizeof(bytes), NULL) == 0);
    t.cpu_count = AOS_X86_ACPI_MAX_CPUS;
    t.gsi_base = UINT32_MAX - 23;
    for (unsigned i = 0; i < t.cpu_count; ++i)
        t.cpus[i] = (aos_x86_acpi_cpu_t){(uint8_t)i,(uint8_t)(i + 16)};
    size_t length = aos_x86_madt_write(bytes + 1, sizeof(bytes) - 2, &t);
    assert(length == AOS_X86_MADT_MAX_BYTES);
    uint8_t sum = 0;
    for (size_t i = 0; i < length; ++i) sum = (uint8_t)(sum + bytes[i + 1]);
    assert(sum == 0 && bytes[0] == 0xa5 && bytes[length + 1] == 0xa5);
    for (unsigned i = 0; i < t.cpu_count; ++i) {
        const uint8_t *entry = bytes + 1 + 44 + i * 8;
        assert(entry[0] == 0 && entry[1] == 8 && entry[2] == i && entry[3] == i + 16);
        assert(entry[4] == 1 && entry[5] == 0 && entry[6] == 0 && entry[7] == 0);
    }
    puts("PASS: x86 MADT exact bytes, capacity, topology and checksum");
    return 0;
}
