#include "platform/x86_event.h"
bool aos_x86_entry_event(aos_x86_entry_event_t *out, bool gp, bool protected_mode,
                          unsigned length, unsigned irq, uint64_t flags,
                          uint32_t interruptibility)
{
    if (!out || length>15u || (irq && (irq<16u || irq>255u))) return false;
    aos_x86_entry_event_t next={.advance=(uint8_t)length};
    if (gp) {
        next.advance=0;
        next.interruption_info=0x8000030du | (protected_mode ? 0x800u : 0u);
    } else if (irq) {
        if ((flags & 0x200u) && !(interruptibility & 3u)) {
            next.interruption_info=0x80000000u | irq;
            next.accept_irq=true;
        } else next.interrupt_window=true;
    }
    *out=next;
    return true;
}
