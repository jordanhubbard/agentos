#ifndef AOS_X86_APIC_H
#define AOS_X86_APIC_H
#include <stdbool.h>
#include <stdint.h>
#define AOS_X86_APIC_BASE UINT64_C(0xfee00000)
typedef struct {
    uint32_t svr, tpr, lvt_timer, initial, divide;
    uint64_t start, now;
} aos_x86_apic_t;
/* One virtual APIC bus tick per invariant host TSC tick. Clock never advances
 * from exit counts. No external interrupt sources or host APIC access. */
void aos_x86_apic_init(aos_x86_apic_t *a, uint64_t ticks);
bool aos_x86_apic_msr(bool write, uint64_t *value);
bool aos_x86_apic_io(aos_x86_apic_t *a, unsigned offset, bool write,
                     uint32_t *value, uint64_t ticks);
/* Until injection is wired, the VMM must stop when this reports due. */
bool aos_x86_apic_interrupt_due(const aos_x86_apic_t *a, uint64_t ticks);
#endif
