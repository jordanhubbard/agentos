/* AArch64 virtual timer output and WFI recovery, independent of seL4 calls. */
#ifndef AOS_PLATFORM_ARM_VTIMER_H
#define AOS_PLATFORM_ARM_VTIMER_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    AOS_VTIMER_KEEP_PENDING,
    AOS_VTIMER_REARM_VPPI,
    AOS_VTIMER_INJECT,
} aos_vtimer_action_t;

static inline aos_vtimer_action_t aos_vtimer_wfi_action(uint64_t ctl,
                                                       bool pending,
                                                       bool inflight)
{
    if (pending || inflight) {
        return AOS_VTIMER_KEEP_PENDING;
    }
    /* CNTV_CTL_EL0: ENABLE bit 0, IMASK bit 1, ISTATUS bit 2.
     * ISTATUS ignores IMASK and is UNKNOWN when ENABLE is clear. Only
     * enabled + expired + unmasked asserts the architectural timer output.
     * seL4's VPPI mask is independent of this guest-owned register. */
    if ((ctl & UINT64_C(7)) == UINT64_C(5)) {
        return AOS_VTIMER_INJECT;
    }
    return AOS_VTIMER_REARM_VPPI;
}

#endif
