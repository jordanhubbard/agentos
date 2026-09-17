#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "platform/x86_ioapic.h"
#include "platform/x86_apic.h"

static uint32_t io(aos_x86_ioapic_t *s, unsigned reg, bool write, uint32_t v)
{
    uint32_t select=reg;
    assert(aos_x86_ioapic_io(s,0,true,&select));
    assert(aos_x86_ioapic_io(s,0x10,write,&v)); return v;
}
static void reject(aos_x86_ioapic_t *s, unsigned off, bool write, uint32_t v)
{
    aos_x86_ioapic_t before=*s; uint32_t old=v;
    assert(!aos_x86_ioapic_io(s,off,write,&v));
    assert(v==old && !memcmp(s,&before,sizeof(before)));
}
static void lapic_write(aos_x86_apic_t *a, unsigned off, uint32_t v)
{ assert(aos_x86_apic_io(a,off,true,&v,0)); }
int main(void)
{
    aos_x86_ioapic_t s, other;
    assert(aos_x86_ioapic_init(&s,1)); other=s;
    assert(!aos_x86_ioapic_init(&s,16) && !memcmp(&s,&other,sizeof(s)));
    assert(io(&s,0,false,0)==0x01000000);
    assert(io(&s,1,false,0)==0x00170011);
    assert(io(&s,2,false,0)==0x01000000);
    for (unsigned i=0; i<24; i++) {
        assert(io(&s,0x10+2*i,false,0)==0x10000);
        assert(io(&s,0x11+2*i,false,0)==0);
    }
    reject(&s,4,false,0); reject(&s,0x40,true,0x40); reject(&s,0,true,256);
    uint32_t select=1; assert(aos_x86_ioapic_io(&s,0,true,&select));
    reject(&s,0x10,true,0);
    select=0x40; assert(aos_x86_ioapic_io(&s,0,true,&select));
    reject(&s,0x10,false,0);
    select=0x10; assert(aos_x86_ioapic_io(&s,0,true,&select));
    reject(&s,0x10,true,0x440); /* NMI is not a fixed/lowest-priority route */
    reject(&s,0x10,true,0); reject(&s,0x10,true,0x20040);
    select=0x11; assert(aos_x86_ioapic_io(&s,0,true,&select));
    reject(&s,0x10,true,1); /* high dword contains only destination */
    aos_x86_ioapic_route_t route={.vector=99};
    assert(!aos_x86_ioapic_set_irq(&s,24,true));
    assert(!aos_x86_ioapic_route(&s,24,&route) && route.vector==99);
    assert(!aos_x86_ioapic_accept(&s,24));
    /* A masked rising edge is lost; unmasking a held edge does not invent one. */
    assert(aos_x86_ioapic_set_irq(&s,0,true));
    io(&s,0x10,true,0x40);
    assert(!aos_x86_ioapic_route(&s,0,&route));
    assert(aos_x86_ioapic_set_irq(&s,0,false));
    assert(aos_x86_ioapic_set_irq(&s,0,true));
    assert(aos_x86_ioapic_route(&s,0,&route));
    assert(route.vector==0x40 && !route.destination && !route.logical && !route.level);
    assert(aos_x86_ioapic_accept(&s,0));
    assert(!aos_x86_ioapic_route(&s,0,&route) && !s.remote_irr);
    /* Logical assertion is independent of the retained active-low flag. */
    io(&s,0x12,true,0x1a841); io(&s,0x13,true,2u<<24);
    assert(aos_x86_ioapic_set_irq(&s,1,true));
    assert(!aos_x86_ioapic_route(&s,1,&route));
    io(&s,0x12,true,0xa841);
    assert(aos_x86_ioapic_route(&s,1,&route));
    assert(route.logical && route.level && route.destination==2 && route.vector==0x41);
    aos_x86_apic_t lapic, isolated;
    aos_x86_apic_init(&lapic,0); aos_x86_apic_init(&isolated,0);
    assert(!aos_x86_apic_route(&lapic,route.vector,route.destination,true,true));
    lapic_write(&lapic,0xf0,0x1ff); lapic_write(&lapic,0xd0,2u<<24);
    assert(aos_x86_apic_route(&lapic,route.vector,route.destination,true,true));
    assert(aos_x86_ioapic_accept(&s,1));
    assert((io(&s,0x12,false,0)&0x4000) && !aos_x86_ioapic_route(&s,1,&route));
    assert(aos_x86_apic_pending(&lapic,0)==0x41 && !aos_x86_apic_pending(&isolated,0));
    assert(aos_x86_apic_accept(&lapic,0x41));
    assert(aos_x86_apic_eoi_vector(&lapic)==0x41);
    uint32_t tmr=0; assert(aos_x86_apic_io(&lapic,0x1a0,false,&tmr,0) && tmr==2);
    /* Changing the route cannot let a new vector acknowledge the old one. */
    io(&s,0x12,true,0xa842);
    aos_x86_ioapic_eoi(&s,0x42); assert(s.remote_irr==2);
    unsigned vector=aos_x86_apic_eoi_vector(&lapic);
    lapic_write(&lapic,0xb0,0); aos_x86_ioapic_eoi(&s,vector);
    assert(!aos_x86_apic_eoi_vector(&lapic));
    assert(aos_x86_ioapic_route(&s,1,&route) && route.vector==0x42);
    assert(aos_x86_ioapic_accept(&s,1));
    assert(aos_x86_ioapic_set_irq(&s,1,false));
    aos_x86_ioapic_eoi(&s,0x42);
    assert(!aos_x86_ioapic_route(&s,1,&route));
    assert(io(&other,0x12,false,0)==0x10000 && !other.asserted && !other.remote_irr);
    puts("PASS: private IOAPIC register policy, masked edges, level EOI/reassertion and LAPIC isolation");
}
