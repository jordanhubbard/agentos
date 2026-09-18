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
/* On success return the new session's backend and MAC for device adoption.
 * Output is unchanged on failure, including a failed local frame mapping. */
bool aos_net_virt_rebind_with_info(uint32_t client, uint32_t generation,
                                  net_virt_rebind_reply_t *attachment);
static inline bool aos_net_rebind_reply_valid(const net_virt_rebind_reply_t *reply,
                                             size_t length, uint32_t generation)
{
    return reply && length == sizeof(*reply) && generation != 0u &&
        reply->status == NET_VIRT_OK && reply->version == NET_VIRT_REBIND_VERSION &&
        reply->generation == generation &&
        (reply->hw_state == NET_VIRT_HW_NONE || reply->hw_state == NET_VIRT_HW_NET_PD) &&
        reply->_pad[0] == 0u && reply->_pad[1] == 0u;
}
#endif
