#ifndef AGENTOS_NET_REBIND_H
#define AGENTOS_NET_REBIND_H
#include <stdbool.h>
#include <stddef.h>
#include <platform/net_layout.h>
#include "contracts/net_virt_contract.h"
#include "contracts/virtualizer_authority.h"

static inline uintptr_t aos_net_rebind_queue_va(uint32_t client)
{
    return client < AOS_NET_GUEST_CLIENTS ?
        AOS_NET_SHMEM_VA + client * AOS_NET_CLIENT_STRIDE : 0u;
}

static inline uint32_t aos_net_rebind_validate(uint64_t badge,
    const net_virt_rebind_req_t *req, size_t length,
    bool attached, bool retired, uint32_t generation)
{
    if (!req || length != sizeof(*req) || req->version != NET_VIRT_REBIND_VERSION)
        return NET_VIRT_ERR_VERSION;
    if (!virt_client_authorized(badge, req->client, req->client))
        return NET_VIRT_ERR_BAD_CLIENT;
    if (attached || !retired || generation == UINT32_MAX ||
        !req->generation || req->generation != generation + 1u)
        return NET_VIRT_ERR_BUSY;
    return NET_VIRT_OK;
}
bool aos_net_virt_rebind(uint32_t client, uint32_t generation);
#endif
