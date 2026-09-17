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
    if ((a->svr & 0x100u) && !(a->lvt_timer & 0x10000u)) {
        unsigned vector=a->lvt_timer & 0xffu;
        if (vector >= 16u) a->irr[vector/32] |= 1u << (vector%32);
        else a->invalid_vector=true;
    }
}
static unsigned highest(const uint32_t bits[8])
{
    for (unsigned v=256; v-- > 16;) if (bits[v/32] & (1u << (v%32))) return v;
    return 0;
}
static unsigned priority(const aos_x86_apic_t *a)
{
    unsigned in_service=highest(a->isr);
    return (a->tpr >> 4) >= (in_service >> 4) ? a->tpr : in_service & 0xf0u;
}
unsigned aos_x86_apic_pending(aos_x86_apic_t *a, uint64_t ticks)
{
    if (!a || ticks < a->now) return 0;
    advance(a,ticks);
    if (a->invalid_vector) return AOS_X86_APIC_INVALID_VECTOR;
    unsigned v=highest(a->irr);
    return (a->svr & 0x100u) && (v >> 4) > (priority(a) >> 4) ? v : 0;
}
bool aos_x86_apic_accept(aos_x86_apic_t *a, unsigned vector)
{
    if (!a || !vector || vector >= 256u || vector != aos_x86_apic_pending(a,a->now)) return false;
    a->irr[vector/32] &= ~(1u << (vector%32));
    a->isr[vector/32] |= 1u << (vector%32);
    return true;
}
bool aos_x86_apic_interrupt_due(const aos_x86_apic_t *a, uint64_t ticks)
{
    if (!a || ticks < a->now) return false;
    aos_x86_apic_t next=*a;
    advance(&next,ticks);
    return highest(next.irr) != 0;
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
    case 0xa0: if (write) return false; *value=priority(&next); break;
    case 0xb0:
        if (!write || *value) return false;
        { unsigned v=highest(next.isr); if (v) next.isr[v/32] &= ~(1u << (v%32)); }
        break;
    case 0xf0: reg=&next.svr; mask=0x1ff; break;
    case 0x320: reg=&next.lvt_timer; mask=0x300ff; break; /* no deadline mode */
    /* No external LINT sources are connected during bootstrap. Retain their
     * vector/mode/polarity/trigger/mask state; status and remote IRR stay zero. */
    case 0x350: reg=&next.lint0; mask=0x1a7ff; break;
    case 0x360: reg=&next.lint1; mask=0x1a7ff; break;
    case 0x380: reg=&next.initial; mask=UINT32_MAX; break;
    case 0x390: if (write) return false; *value=next.counter; break;
    case 0x3e0: reg=&next.divide; mask=0xb; break;
    default:
        if (write || (off & 15u)) return false;
        if (off >= 0x100 && off <= 0x170) *value=next.isr[(off-0x100)/16];
        else if (off >= 0x180 && off <= 0x1f0) *value=0; /* all sources edge-triggered */
        else if (off >= 0x200 && off <= 0x270) *value=next.irr[(off-0x200)/16];
        else return false;
        break;
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
