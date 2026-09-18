#include "platform/x86_apic.h"

bool aos_x86_apic_init_cpu(aos_x86_apic_t *a, uint64_t ticks,
                           unsigned id, bool bootstrap)
{
    if (!a || id >= 255u) return false;
    *a = (aos_x86_apic_t){.svr=0xff, .lvt_timer=0x10000,
        .lint0=0x10000, .lint1=0x10000, .dfr=UINT32_MAX,
        .lvt_thermal=0x10000, .lvt_perf=0x10000, .lvt_error=0x10000, .now=ticks,
        .id=(uint8_t)id, .bootstrap=bootstrap};
    return true;
}
void aos_x86_apic_init(aos_x86_apic_t *a, uint64_t ticks)
{
    (void)aos_x86_apic_init_cpu(a,ticks,0u,true);
}
bool aos_x86_apic_msr(const aos_x86_apic_t *a, bool write, uint64_t *value)
{
    if (!a || !value) return false;
    const uint64_t fixed = AOS_X86_APIC_BASE | 0x800u |
        (a->bootstrap ? 0x100u : 0u); /* enabled, explicit BSP, xAPIC */
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
    if (a->irr_level[vector/32] & (1u << (vector%32)))
        a->tmr[vector/32] |= 1u << (vector%32);
    else a->tmr[vector/32] &= ~(1u << (vector%32));
    a->irr_level[vector/32] &= ~(1u << (vector%32));
    return true;
}
bool aos_x86_apic_route(aos_x86_apic_t *a, unsigned vector,
                       unsigned destination, bool logical, bool level)
{
    if (!a || vector<16u || vector>255u || destination>255u || !(a->svr & 0x100u))
        return false;
    unsigned local=a->ldr >> 24;
    bool target=destination==255u || (logical ?
        (a->dfr==UINT32_MAX ? (destination & local)!=0 :
         (destination >> 4)==(local >> 4) && (destination & local & 15u)!=0) :
        destination==a->id);
    if (!target) return false;
    a->irr[vector/32] |= 1u << (vector%32);
    if (level) a->irr_level[vector/32] |= 1u << (vector%32);
    return true;
}
unsigned aos_x86_apic_eoi_vector(const aos_x86_apic_t *a)
{
    if (!a) return 0;
    unsigned vector=highest(a->isr);
    return a->tmr[vector/32] & (1u << (vector%32)) ? vector : 0;
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
    case 0x20: if (write) return false; *value=(uint32_t)a->id << 24; break;
    case 0x30: if (write) return false; *value=0x00050014; break;
    case 0x80: reg=&next.tpr; mask=0xff; break;
    case 0xd0: reg=&next.ldr; mask=0xff000000u; break;
    case 0xe0:
        if (write) {
            if ((*value >> 28)!=0u && (*value >> 28)!=15u) return false;
            next.dfr=*value | 0x0fffffffu;
        } else *value=next.dfr;
        break;
    case 0xa0: if (write) return false; *value=priority(&next); break;
    case 0xb0:
        if (!write || *value) return false;
        { unsigned v=highest(next.isr); if (v) {
            next.isr[v/32] &= ~(1u << (v%32));
            next.tmr[v/32] &= ~(1u << (v%32));
        } }
        break;
    /* Focus checking only affects lowest-priority arbitration, which this
     * single-CPU fixed-delivery profile does not implement. Retain bit9. */
    case 0xf0: reg=&next.svr; mask=0x3ff; break;
    case 0x280:
        if (write && *value) return false;
        if (!write) *value=0; /* unsupported/error operations fail atomically */
        break;
    case 0x300:
        if (!write) { *value=next.icr_low; break; }
        /* One provisioned CPU: fixed, edge-triggered IPIs only. No host
         * APIC access, NMI injection, AP startup or hidden vCPU creation. */
        if ((*value & ~0x000c48ffu) || (*value & 0xffu)<16u) return false;
        {
            unsigned shortcut=(*value >> 18)&3u, dest=next.icr_high >> 24;
            if (shortcut!=3u)
                (void)aos_x86_apic_route(&next,*value & 255u,
                    shortcut ? next.id : dest,!shortcut && (*value & 0x800u),false);
            next.icr_low=*value; /* delivery completes synchronously */
        }
        break;
    case 0x310: reg=&next.icr_high; mask=0xff000000u; break;
    case 0x320: reg=&next.lvt_timer; mask=0x300ff; break; /* no deadline mode */
    case 0x330: reg=&next.lvt_thermal; mask=0x107ff; break;
    case 0x340: reg=&next.lvt_perf; mask=0x107ff; break;
    case 0x370: reg=&next.lvt_error; mask=0x100ff; break;
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
        else if (off >= 0x180 && off <= 0x1f0) *value=next.tmr[(off-0x180)/16];
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
