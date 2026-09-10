/* Guest-neutral seL4 receive loop for profile-backed VMM PDs. */
#ifndef AOS_PLATFORM_GUEST_VMM_LOOP_H
#define AOS_PLATFORM_GUEST_VMM_LOOP_H

#include <stdbool.h>
#include <stdint.h>
#include <sel4/sel4.h>

typedef struct aos_guest_vmm_loop_ops {
    uint32_t *guest_state;
    seL4_MessageInfo_t (*rpc)(seL4_MessageInfo_t info);
    seL4_MessageInfo_t (*fault)(seL4_Word badge,
                                seL4_MessageInfo_t info);
    void (*notified)(seL4_Word badge);
    void (*net_rx_ready)(void);
} aos_guest_vmm_loop_ops_t;

__attribute__((noreturn))
void aos_guest_vmm_loop(seL4_CPtr endpoint, seL4_CPtr reply_cap,
                        const aos_guest_vmm_loop_ops_t *ops);

#endif /* AOS_PLATFORM_GUEST_VMM_LOOP_H */
