#ifndef AOS_X86_APIC_H
#define AOS_X86_APIC_H
#include <stdbool.h>
#include <stdint.h>
#define AOS_X86_APIC_BASE UINT64_C(0xfee00000)
typedef struct {
    uint32_t svr, tpr, lvt_timer, initial, divide, lint0, lint1;
    uint64_t now, phase;
    uint32_t counter;
    uint32_t irr[8], isr[8];
} aos_x86_apic_t;
/* One virtual APIC bus tick per invariant host TSC tick. Clock never advances
 * from exit counts. No external interrupt sources or host APIC access. */
void aos_x86_apic_init(aos_x86_apic_t *a, uint64_t ticks);
bool aos_x86_apic_msr(bool write, uint64_t *value);
bool aos_x86_apic_io(aos_x86_apic_t *a, unsigned offset, bool write,
                     uint32_t *value, uint64_t ticks);
/* Observational expiry check; does not advance state. */
bool aos_x86_apic_interrupt_due(const aos_x86_apic_t *a, uint64_t ticks);
/* Advance time and return the highest eligible vector, or zero. The caller
 * must separately check CPU interruptibility before accepting that vector. */
unsigned aos_x86_apic_pending(aos_x86_apic_t *a, uint64_t ticks);
bool aos_x86_apic_accept(aos_x86_apic_t *a, unsigned vector);
#endif
