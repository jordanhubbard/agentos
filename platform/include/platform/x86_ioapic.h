#ifndef AOS_X86_IOAPIC_H
#define AOS_X86_IOAPIC_H
#include <stdbool.h>
#include <stdint.h>
#define AOS_X86_IOAPIC_BASE UINT64_C(0xfec00000)
#define AOS_X86_IOAPIC_INPUTS 24u
typedef struct {
    uint64_t redirection[AOS_X86_IOAPIC_INPUTS];
    uint32_t asserted, edge_pending, remote_irr;
    uint8_t accepted_vector[AOS_X86_IOAPIC_INPUTS];
    uint8_t selector, id;
} aos_x86_ioapic_t;
typedef struct {
    uint8_t vector, destination;
    bool logical, level;
} aos_x86_ioapic_route_t;
/* Fixed, firmware-described ID. Each guest owns a distinct controller. */
bool aos_x86_ioapic_init(aos_x86_ioapic_t *s, unsigned id);
/* Aligned DWORD access to IOREGSEL/IOWIN only. Unsupported registers,
 * delivery modes and reserved bits fail without changing state or value. */
bool aos_x86_ioapic_io(aos_x86_ioapic_t *s, unsigned offset, bool write,
                       uint32_t *value);
/* Callers supply logical assertion, not electrical voltage; polarity is
 * retained in the route register. There is no ISA IRQ0-to-GSI2 remapping. */
bool aos_x86_ioapic_set_irq(aos_x86_ioapic_t *s, unsigned input, bool asserted);
/* Inspect, then accept only after the destination LAPIC accepted the route.
 * Masked edges are lost; asserted levels retry after EOI. */
bool aos_x86_ioapic_route(const aos_x86_ioapic_t *s, unsigned input,
                          aos_x86_ioapic_route_t *route);
bool aos_x86_ioapic_accept(aos_x86_ioapic_t *s, unsigned input);
void aos_x86_ioapic_eoi(aos_x86_ioapic_t *s, unsigned vector);
#endif
