#ifndef AGENTOS_BLK_REBIND_H
#define AGENTOS_BLK_REBIND_H
#include <stdbool.h>
#include <stddef.h>
#include "contracts/blk_virt_contract.h"
#include "contracts/virtualizer_authority.h"

/* Validation must precede all capability and queue access. Detach is the
 * only transition that can set retired after draining both old queues. */
static inline uint32_t aos_blk_rebind_validate(uint64_t badge,
    const blk_virt_rebind_req_t *req, size_t length,
    bool attached, bool retired, uint32_t generation)
{
    if (!req || length != sizeof(*req) || req->version != BLK_VIRT_REBIND_VERSION)
        return BLK_VIRT_ERR_VERSION;
    if (!virt_media_authorized(badge, req->client, req->client, req->client))
        return BLK_VIRT_ERR_BAD_CLIENT;
    if (attached || !retired || generation == UINT32_MAX ||
        req->generation == 0u || req->generation != generation + 1u)
        return BLK_VIRT_ERR_BUSY;
    return BLK_VIRT_OK;
}

bool aos_blk_virt_rebind(uint32_t client, uint32_t generation);
#endif
