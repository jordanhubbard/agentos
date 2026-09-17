#include "platform/x86_apic.h"

void aos_x86_apic_init(aos_x86_apic_t *a, uint64_t ticks)
{
    *a = (aos_x86_apic_t){.svr=0xff, .lvt_timer=0x10000,
        .lint0=0x10000, .lint1=0x10000, .start=ticks, .now=ticks};
}
bool aos_x86_apic_msr(bool write, uint64_t *value)
{
    if (!value) return false;
    const uint64_t fixed = AOS_X86_APIC_BASE | 0x900u; /* enabled, BSP, xAPIC */
    if (write) return *value == fixed; /* no relocation or x2APIC transition */
    *value = fixed; return true;
}
static unsigned divisor(const aos_x86_apic_t *a)
{
    unsigned encoding = (a->divide & 3) | ((a->divide >> 1) & 4);
    return 1u << ((encoding + 1) & 7);
}
static uint32_t current(const aos_x86_apic_t *a, uint64_t ticks)
{
    if (!a->initial) return 0;
    uint64_t elapsed = (ticks-a->start)/divisor(a);
    if (a->lvt_timer & 0x20000u) return a->initial - elapsed % a->initial;
    return elapsed >= a->initial ? 0 : a->initial - (uint32_t)elapsed;
}
bool aos_x86_apic_interrupt_due(const aos_x86_apic_t *a, uint64_t ticks)
{
    return ticks >= a->start && a->initial && (a->svr & 0x100u) &&
           !(a->lvt_timer & 0x10000u) && (ticks-a->start)/divisor(a) >= a->initial;
}
bool aos_x86_apic_io(aos_x86_apic_t *a, unsigned off, bool write,
                     uint32_t *value, uint64_t ticks)
{
    if (!a || !value || ticks < a->now) return false;
    uint32_t *reg = 0, mask = 0;
    switch (off) {
    case 0x20: if (write) return false; *value=0; break; /* APIC ID 0 */
    case 0x30: if (write) return false; *value=0x00050014; break;
    case 0x80: reg=&a->tpr; mask=0xff; break;
    case 0xf0: reg=&a->svr; mask=0x1ff; break;
    case 0x320: reg=&a->lvt_timer; mask=0x300ff; break; /* no deadline mode */
    /* No external LINT sources are connected during bootstrap. Retain their
     * vector/mode/polarity/trigger/mask state; status and remote IRR stay zero. */
    case 0x350: reg=&a->lint0; mask=0x1a7ff; break;
    case 0x360: reg=&a->lint1; mask=0x1a7ff; break;
    case 0x380: reg=&a->initial; mask=UINT32_MAX; break;
    case 0x390: if (write) return false; *value=current(a,ticks); break;
    case 0x3e0: reg=&a->divide; mask=0xb; break;
    default: return false;
    }
    if (reg) {
        if (write) {
            if (*value & ~mask) return false;
            /* Reprogramming a running divider needs additional phase state. */
            if (off == 0x3e0 && *value != *reg && a->initial) return false;
            *reg=*value;
            if (off == 0x380) a->start=ticks;
        } else *value=*reg;
    }
    a->now=ticks;
    return true;
}
