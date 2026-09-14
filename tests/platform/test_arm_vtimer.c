#include <stdio.h>
#include <platform/arm_vtimer.h>

int main(void)
{
    static const aos_vtimer_action_t expected[8] = {
        AOS_VTIMER_REARM_VPPI, /* disabled; status is not meaningful */
        AOS_VTIMER_REARM_VPPI, /* enabled, not expired */
        AOS_VTIMER_REARM_VPPI, /* disabled, masked */
        AOS_VTIMER_REARM_VPPI, /* enabled, masked, not expired */
        AOS_VTIMER_REARM_VPPI, /* disabled, unknown status reads as one */
        AOS_VTIMER_INJECT,     /* enabled, unmasked, expired */
        AOS_VTIMER_REARM_VPPI, /* disabled, masked, unknown status */
        AOS_VTIMER_REARM_VPPI, /* enabled and expired but guest-masked */
    };
    unsigned failures = 0;
    puts("1..8");
    for (uint64_t ctl = 0; ctl < 8; ctl++) {
        bool ok = aos_vtimer_wfi_action(ctl, false, false) == expected[ctl];
        ok &= aos_vtimer_wfi_action(ctl | UINT64_C(0x8000000000000000),
                                   false, false) == expected[ctl];
        ok &= aos_vtimer_wfi_action(ctl, true, false) == AOS_VTIMER_KEEP_PENDING;
        ok &= aos_vtimer_wfi_action(ctl, false, true) == AOS_VTIMER_KEEP_PENDING;
        ok &= aos_vtimer_wfi_action(ctl, true, true) == AOS_VTIMER_KEEP_PENDING;
        printf("%s %u - CNTV_CTL=%u output and pending/inflight guards\n",
               ok ? "ok" : "not ok", (unsigned)ctl + 1u, (unsigned)ctl);
        failures += !ok;
    }
    return failures ? 1 : 0;
}
