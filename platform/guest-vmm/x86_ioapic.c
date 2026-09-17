#include "platform/x86_ioapic.h"
#define MASK (1u << 16)
#define LEVEL (1u << 15)
#define REMOTE (1u << 14)

bool aos_x86_ioapic_init(aos_x86_ioapic_t *s, unsigned id)
{
    if (!s || id>15u) return false;
    *s=(aos_x86_ioapic_t){.id=(uint8_t)id};
    for (unsigned i=0; i<AOS_X86_IOAPIC_INPUTS; i++) s->redirection[i]=MASK;
    return true;
}

bool aos_x86_ioapic_io(aos_x86_ioapic_t *s, unsigned offset, bool write,
                       uint32_t *value)
{
    if (!s || !value) return false;
    if (!offset) {
        if (write) {
            if (*value>255u) return false;
            s->selector=(uint8_t)*value;
        } else *value=s->selector;
        return true;
    }
    if (offset!=0x10u) return false;
    unsigned reg=s->selector;
    if (reg==0u || reg==2u) {
        uint32_t id=(uint32_t)s->id << 24;
        if (write) return reg==0u && *value==id;
        *value=id; return true;
    }
    if (reg==1u) {
        if (write) return false;
        *value=((AOS_X86_IOAPIC_INPUTS-1u)<<16) | 0x11u;
        return true;
    }
    if (reg<0x10u || reg>=0x10u+2u*AOS_X86_IOAPIC_INPUTS) return false;
    unsigned input=(reg-0x10u)/2u, shift=(reg&1u)*32u;
    uint64_t old=s->redirection[input];
    if (!write) {
        uint64_t readback=old | ((s->remote_irr & (1u<<input)) ? REMOTE : 0u);
        *value=(uint32_t)(readback >> shift); return true;
    }
    uint32_t data=*value;
    if (shift) {
        if (data & 0x00ffffffu) return false;
    } else {
        /* Delivery status and Remote IRR are read-only; RMW preserves them
         * independently of the guest-supplied copies of those bits. */
        if (data & ~0x0001ffffu) return false;
        data &= ~0x5000u;
        if (((data >> 8)&7u)>1u || (!(data & MASK) && (data & 255u)<16u))
            return false; /* fixed or lowest-priority, not NMI/SMI/INIT/ExtINT */
    }
    uint64_t part=UINT64_C(0xffffffff)<<shift;
    s->redirection[input]=(old & ~part) | ((uint64_t)data << shift);
    return true;
}

bool aos_x86_ioapic_set_irq(aos_x86_ioapic_t *s, unsigned input, bool asserted)
{
    if (!s || input>=AOS_X86_IOAPIC_INPUTS) return false;
    uint32_t bit=1u<<input;
    uint64_t rte=s->redirection[input];
    if (asserted) {
        if (!(s->asserted & bit) && !(rte & (MASK|LEVEL))) s->edge_pending|=bit;
        s->asserted|=bit;
    } else s->asserted&=~bit;
    return true;
}

bool aos_x86_ioapic_route(const aos_x86_ioapic_t *s, unsigned input,
                          aos_x86_ioapic_route_t *route)
{
    if (!s || !route || input>=AOS_X86_IOAPIC_INPUTS) return false;
    uint32_t bit=1u<<input;
    uint64_t rte=s->redirection[input];
    if ((rte & MASK) || (s->remote_irr & bit) || (rte & 255u)<16u ||
        !((rte & LEVEL ? s->asserted : s->edge_pending) & bit)) return false;
    *route=(aos_x86_ioapic_route_t){.vector=(uint8_t)rte,
        .destination=(uint8_t)(rte >> 56),.logical=(rte & 0x800u)!=0,
        .level=(rte & LEVEL)!=0};
    return true;
}

bool aos_x86_ioapic_accept(aos_x86_ioapic_t *s, unsigned input)
{
    aos_x86_ioapic_route_t route;
    if (!aos_x86_ioapic_route(s,input,&route)) return false;
    uint32_t bit=1u<<input;
    s->edge_pending&=~bit;
    if (route.level) {
        s->remote_irr|=bit;
        s->accepted_vector[input]=route.vector;
    }
    return true;
}

void aos_x86_ioapic_eoi(aos_x86_ioapic_t *s, unsigned vector)
{
    if (!s || vector<16u || vector>255u) return;
    for (unsigned i=0; i<AOS_X86_IOAPIC_INPUTS; i++)
        if ((s->remote_irr & (1u<<i)) && s->accepted_vector[i]==vector)
            s->remote_irr&=~(1u<<i);
}
