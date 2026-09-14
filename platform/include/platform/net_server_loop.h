#pragma once
#include <sel4/sel4.h>

/* Root grants this notification badge only to the NIC IRQ handler. The
 * driver has one hardware IRQ; RPC endpoint badges are service identities. */
#define AOS_NET_HOST_IRQ_BADGE ((seL4_Word)0x80000000u)

static inline __attribute__((noreturn)) void aos_net_server_loop(
    seL4_CPtr endpoint, seL4_CPtr reply_cap,
    void (*host_irq)(void), seL4_MessageInfo_t (*request)(seL4_Word))
{
    for (;;) {
        seL4_Word badge = 0;
#ifdef CONFIG_KERNEL_MCS
        (void)seL4_Recv(endpoint, &badge, reply_cap);
#else
        (void)reply_cap;
        (void)seL4_Recv(endpoint, &badge);
#endif
        /* Notification delivery does not supply an IPC message label. */
        if (badge == AOS_NET_HOST_IRQ_BADGE) {
            host_irq();
            continue;
        }
        seL4_MessageInfo_t reply = request(badge);
#ifdef CONFIG_KERNEL_MCS
        seL4_Send(reply_cap, reply);
#else
        seL4_Reply(reply);
#endif
    }
}
