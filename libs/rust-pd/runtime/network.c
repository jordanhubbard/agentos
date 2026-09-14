/* Control ABI and queue initialization shared with the VMM clients. */
#include "agentos.h"
#include "sel4_ipc.h"
#include "contracts/net_virt_contract.h"
#include <platform/net_virt_pump.h>
#include <platform/native_net_isolation_probe.h>

void agentos_pd_net_isolation_probe(void)
{
#ifdef AGENTOS_NATIVE_NET_ISOLATION_PROBE
    volatile uint64_t *foreign = (volatile uint64_t *)AOS_NATIVE_NET_PROBE_ADDRESS;
#if AOS_NATIVE_NET_PROBE_WRITE
    *foreign = UINT64_C(0xdeadbeef);
#else
    uint64_t observed = *foreign;
    __asm__ volatile("" : : "r"(observed) : "memory");
#endif
#endif
}

int agentos_pd_net_attach(void *page, uintptr_t endpoint, uint32_t client_id,
                         uint32_t slot, uint8_t *mac, uint32_t *hardware)
{
    aos_net_virt_client_t client = {0};
    sel4_msg_t req = {0}, rep = {0};
    net_virt_attach_req_t attach = { NET_VIRT_CONTRACT_VERSION, client_id, slot };
    net_virt_attach_reply_t reply = {0};
    aos_net_client_bind(page, 0u, &client);
    aos_net_client_init_buffers(&client);
    req.opcode = NET_VIRT_OP_ATTACH;
    req.length = sizeof(attach);
    __builtin_memcpy(req.data, &attach, sizeof(attach));
    sel4_call(endpoint, &req, &rep);
    if (rep.opcode != SEL4_ERR_OK || rep.length != sizeof(reply)) return -1;
    __builtin_memcpy(&reply, rep.data, sizeof(reply));
    if (reply.version != NET_VIRT_CONTRACT_VERSION || reply.status != NET_VIRT_OK) return -1;
    __builtin_memcpy(mac, reply.mac, 6);
    *hardware = reply.hw_state;
    return 0;
}

void agentos_pd_net_signal(uintptr_t notification)
{
    seL4_Signal(notification);
}

uintptr_t agentos_pd_net_wait(uintptr_t notification)
{
    seL4_Word badge = 0;
    seL4_Wait(notification, &badge);
    return badge;
}
