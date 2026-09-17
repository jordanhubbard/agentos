#ifndef AOS_PLATFORM_X86_ACPI_H
#define AOS_PLATFORM_X86_ACPI_H
#include <stddef.h>
#include <stdint.h>

#define AOS_X86_ACPI_MAX_CPUS 32u
#define AOS_X86_MADT_MAX_BYTES (44u + 8u * AOS_X86_ACPI_MAX_CPUS + 12u)
#define AOS_X86_CPU_SSDT_MAX_BYTES (44u + 29u * AOS_X86_ACPI_MAX_CPUS)

typedef struct aos_x86_acpi_cpu {
    uint8_t uid;
    uint8_t apic_id;
} aos_x86_acpi_cpu_t;

/* These are guest physical addresses of VMM-emulated controllers, never
 * host device addresses. Every listed vCPU must already be provisioned.
 * Firmware must supply matching ACPI0007 Device objects with integer _UIDs.
 * The I/O APIC has 24 inputs starting at gsi_base. This topology has no
 * legacy PIC, ISA overrides, NMI sources or hotplug. */
typedef struct aos_x86_acpi_topology {
    uint32_t lapic_gpa;
    uint32_t ioapic_gpa;
    uint32_t gsi_base;
    uint8_t ioapic_id;
    uint8_t cpu_count;
    aos_x86_acpi_cpu_t cpus[AOS_X86_ACPI_MAX_CPUS];
} aos_x86_acpi_topology_t;

/* Writes an ACPI 6.6 MADT (revision 7), returning its length. Returns zero
 * without modifying output if configuration/capacity is invalid. This only
 * serializes topology: it neither maps devices nor installs firmware tables. */
size_t aos_x86_madt_write(void *output, size_t capacity,
                         const aos_x86_acpi_topology_t *topology);
/* Matching ACPI0007 Device objects under \\_SB_, with integer _UIDs from
 * the same topology. Install alongside MADT; do not duplicate these objects
 * in the DSDT. Returns zero without modifying output on invalid input. */
size_t aos_x86_cpu_ssdt_write(void *output, size_t capacity,
                             const aos_x86_acpi_topology_t *topology);
#endif
