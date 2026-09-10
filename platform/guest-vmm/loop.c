#include <platform/guest_vmm_loop.h>

#include <contracts/guest_contract.h>
#include <contracts/net-service/interface.h>

static bool is_guest_rpc(seL4_Word label)
{
    return label == MSG_GUEST_CREATE ||
           label == MSG_GUEST_BOOT ||
           label == MSG_GUEST_SEND_INPUT ||
           label == MSG_GUEST_CONSOLE_DRAIN ||
           label == MSG_GUEST_SUSPEND ||
           label == MSG_GUEST_RESUME ||
           label == MSG_GUEST_DESTROY;
}

__attribute__((noreturn))
void aos_guest_vmm_loop(seL4_CPtr endpoint, seL4_CPtr reply_cap,
                        const aos_guest_vmm_loop_ops_t *ops)
{
    seL4_Word badge;
#ifdef CONFIG_KERNEL_MCS
    seL4_MessageInfo_t info = seL4_Recv(endpoint, &badge, reply_cap);
#else
    (void)reply_cap;
    seL4_MessageInfo_t info = seL4_Recv(endpoint, &badge);
#endif
    for (;;) {
        seL4_Word label = seL4_MessageInfo_get_label(info);
        if (is_guest_rpc(label)) {
            seL4_MessageInfo_t reply = ops->rpc(info);
#ifdef CONFIG_KERNEL_MCS
            seL4_Send(reply_cap, reply);
            info = seL4_Recv(endpoint, &badge, reply_cap);
#else
            seL4_Reply(reply);
            info = seL4_Recv(endpoint, &badge);
#endif
        } else if (label == NET_SVC_EVENT_RX_READY) {
            if (*ops->guest_state == GUEST_STATE_RUNNING) {
                ops->net_rx_ready();
            }
#ifdef CONFIG_KERNEL_MCS
            info = seL4_Recv(endpoint, &badge, reply_cap);
#else
            info = seL4_Recv(endpoint, &badge);
#endif
        } else if (label == seL4_Fault_NullFault) {
            ops->notified(badge);
#ifdef CONFIG_KERNEL_MCS
            info = seL4_Recv(endpoint, &badge, reply_cap);
#else
            info = seL4_Recv(endpoint, &badge);
#endif
        } else {
            seL4_MessageInfo_t reply = ops->fault(badge, info);
#ifdef CONFIG_KERNEL_MCS
            seL4_Send(reply_cap, reply);
            info = seL4_Recv(endpoint, &badge, reply_cap);
#else
            seL4_Reply(reply);
            info = seL4_Recv(endpoint, &badge);
#endif
        }
    }
}
