#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "platform/x86_event.h"
#include "platform/x86_apic.h"
int main(void)
{
    aos_x86_entry_event_t e;
    aos_x86_apic_t a;
    aos_x86_apic_init(&a,0);
    uint32_t v=0x1ff;
    assert(aos_x86_apic_io(&a,0xf0,true,&v,0));
    assert(aos_x86_apic_route(&a,0x51,0,false,true));
    for (unsigned shadow=0;shadow<4;shadow++) {
        for (unsigned enabled=0;enabled<2;enabled++) {
            assert(aos_x86_entry_event(&e,true,true,2,0x51,enabled*0x200u,shadow));
            assert(e.interruption_info==0x80000b0du && !e.error_code && !e.advance);
            assert(!e.accept_irq && !e.interrupt_window);
            assert(aos_x86_apic_pending(&a,0)==0x51); /* GP did not consume IRQ */
        }
    }
    assert(aos_x86_entry_event(&e,false,true,2,0x51,0,0));
    assert(!e.interruption_info && e.advance==2 && e.interrupt_window && !e.accept_irq);
    assert(aos_x86_entry_event(&e,false,true,0,0x51,0x200,0));
    assert(e.interruption_info==0x80000051 && e.accept_irq && !e.interrupt_window && !e.advance);
    assert(aos_x86_apic_accept(&a,0x51));
    assert(aos_x86_apic_eoi_vector(&a)==0x51 && !aos_x86_apic_pending(&a,0));
    assert(aos_x86_entry_event(&e,true,false,2,0,0,0));
    assert(e.interruption_info==0x8000030du && !e.error_code && !e.advance);
    assert(aos_x86_entry_event(&e,false,true,15,0,0,0));
    assert(!e.interruption_info && !e.error_code && e.advance==15 && !e.accept_irq);
    aos_x86_entry_event_t old=e;
    assert(!aos_x86_entry_event(&e,false,true,16,0,0,0));
    assert(!aos_x86_entry_event(&e,true,true,2,256,0,0));
    assert(!aos_x86_entry_event(&e,true,true,2,15,0,0));
    assert(!memcmp(&e,&old,sizeof(e)));
    assert(!aos_x86_entry_event(NULL,true,true,2,0,0,0));
    puts("PASS: GP error-code/RIP semantics, pending IRQ preservation and event priority");
}
