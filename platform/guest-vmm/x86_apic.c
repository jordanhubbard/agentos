#include "platform/x86_apic.h"

void aos_x86_apic_init(aos_x86_apic_t *a, uint64_t ticks)
{
    *a = (aos_x86_apic_t){.svr=0xff, .lvt_timer=0x10000,
        .lint0=0x10000, .lint1=0x10000, .now=ticks};
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
static void advance(aos_x86_apic_t *a, uint64_t ticks)
{
    uint64_t delta=ticks-a->now;
    unsigned div=divisor(a);
    uint64_t fraction=delta%div+a->phase;
    uint64_t elapsed=delta/div+fraction/div;
    a->phase=fraction%div;
    a->now=ticks;
    if (!a->counter) return;
    if (elapsed < a->counter) { a->counter-=(uint32_t)elapsed; return; }
    elapsed-=a->counter;
    a->counter=(a->lvt_timer & 0x20000u) && a->initial ?
        a->initial-(uint32_t)(elapsed%a->initial) : 0;
    if ((a->svr & 0x100u) && !(a->lvt_timer & 0x10000u)) a->timer_pending=true;
}
bool aos_x86_apic_interrupt_due(const aos_x86_apic_t *a, uint64_t ticks)
{
    if (!a || ticks < a->now) return false;
    aos_x86_apic_t next=*a;
    advance(&next,ticks);
    return next.timer_pending;
}
bool aos_x86_apic_io(aos_x86_apic_t *a, unsigned off, bool write,
                     uint32_t *value, uint64_t ticks)
{
    if (!a || !value || ticks < a->now) return false;
    aos_x86_apic_t next=*a;
    advance(&next,ticks);
    uint32_t *reg = 0, mask = 0;
    switch (off) {
    case 0x20: if (write) return false; *value=0; break; /* APIC ID 0 */
    case 0x30: if (write) return false; *value=0x00050014; break;
    case 0x80: reg=&next.tpr; mask=0xff; break;
    case 0xf0: reg=&next.svr; mask=0x1ff; break;
    case 0x320: reg=&next.lvt_timer; mask=0x300ff; break; /* no deadline mode */
    /* No external LINT sources are connected during bootstrap. Retain their
     * vector/mode/polarity/trigger/mask state; status and remote IRR stay zero. */
    case 0x350: reg=&next.lint0; mask=0x1a7ff; break;
    case 0x360: reg=&next.lint1; mask=0x1a7ff; break;
    case 0x380: reg=&next.initial; mask=UINT32_MAX; break;
    case 0x390: if (write) return false; *value=next.counter; break;
    case 0x3e0: reg=&next.divide; mask=0xb; break;
    default: return false;
    }
    if (reg) {
        if (write) {
            if (*value & ~mask) return false;
            /* Preserve remaining count at a divider change; restart the
             * prescaler at this tick under the new divider. */
            if (off == 0x3e0 && *value != *reg) next.phase=0;
            *reg=*value;
            if (off == 0x380) { next.counter=*value; next.phase=0; }
        } else *value=*reg;
    }
    *a=next;
    return true;
}
