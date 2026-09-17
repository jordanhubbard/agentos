#ifndef AOS_PLATFORM_X86_EVENT_H
#define AOS_PLATFORM_X86_EVENT_H
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    uint32_t interruption_info;
    uint32_t error_code;
    uint8_t advance;
    bool accept_irq, interrupt_window;
} aos_x86_entry_event_t;
/* A synchronous #GP(0) retains the faulting RIP and takes precedence over
 * an external interrupt. Pending LAPIC state is consumed only on accept_irq.
 * VMX omits the error-code field for real-address mode. */
bool aos_x86_entry_event(aos_x86_entry_event_t *out, bool gp, bool protected_mode,
                          unsigned length, unsigned irq, uint64_t flags,
                          uint32_t interruptibility);
#endif
