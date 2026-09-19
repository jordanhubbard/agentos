/* SPDX-License-Identifier: BSD-2-Clause */
#include <platform/input_rebind.h>
#include "contracts/virtualizer_authority.h"

uint32_t aos_input_rebind_validate(const aos_input_service_t *s, uint64_t badge,
    const input_virt_rebind_req_t *q, size_t length)
{
    if (!s || !s->frontend || !q || length!=sizeof(*q) ||
        q->version!=INPUT_VIRT_REBIND_VERSION) return AOS_INPUT_BAD_REQUEST;
    if (q->client>=AOS_INPUT_CLIENTS ||
        !virt_client_authorized(badge,q->client,q->client)) return AOS_INPUT_DENIED;
    unsigned client=q->client;
    if ((s->allowed_mask & (1u<<client)) || !(s->retired_mask & (1u<<client)) ||
        s->clients[client] || s->generation[client]==UINT32_MAX ||
        !q->generation || q->generation!=s->generation[client]+1u)
        return AOS_INPUT_WOULD_BLOCK;
    if (__atomic_load_n(&s->frontend->req_head,__ATOMIC_ACQUIRE)!=
        __atomic_load_n(&s->frontend->req_tail,__ATOMIC_ACQUIRE))
        return AOS_INPUT_WOULD_BLOCK;
    return AOS_INPUT_OK;
}

uint32_t aos_input_rebind_commit(aos_input_service_t *s, uint64_t badge,
    const input_virt_rebind_req_t *q, size_t length, aos_input_client_region_t *fresh)
{
    uint32_t status=aos_input_rebind_validate(s,badge,q,length);
    if (status!=AOS_INPUT_OK) return status;
    if (!fresh || fresh->detach.version || fresh->detach.request || fresh->detach.ack)
        return AOS_INPUT_BAD_REQUEST;
    for (unsigned i=0;i<AOS_INPUT_CLIENTS;++i)
        if (s->clients[i]==fresh) return AOS_INPUT_DENIED;
    for (unsigned d=0;d<AOS_INPUT_DEVICES;++d)
        if (fresh->devices[d].head || fresh->devices[d].tail) return AOS_INPUT_BAD_REQUEST;
    unsigned client=q->client;
    s->clients[client]=fresh;
    s->generation[client]=q->generation;
    s->retired_mask &= ~(1u<<client);
    s->allowed_mask |= 1u<<client;
    return AOS_INPUT_OK;
}
