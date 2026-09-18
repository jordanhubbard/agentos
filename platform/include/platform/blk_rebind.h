#ifndef AGENTOS_BLK_REBIND_H
#define AGENTOS_BLK_REBIND_H
#include <stdbool.h>
#include <stddef.h>
#include <platform/blk_layout.h>
#include "contracts/blk_virt_contract.h"
#include "contracts/virtualizer_authority.h"

static inline uintptr_t aos_blk_rebind_queue_va(uint32_t client)
{
    return client < AOS_BLK_MAX_CLIENTS ?
        AOS_BLK_SHMEM_VA + AOS_BLK_CLIENT_BASE + client * AOS_BLK_CLIENT_STRIDE : 0u;
}

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
/* Output is published only after a validated reply and successful mapping. */
bool aos_blk_virt_rebind_with_info(uint32_t client, uint32_t generation,
                                  blk_virt_rebind_reply_t *attachment);
static inline bool aos_blk_rebind_reply_valid(const blk_virt_rebind_reply_t *reply,
                                             size_t length, uint32_t generation)
{
    return reply && length == sizeof(*reply) && generation &&
        reply->status == BLK_VIRT_OK && reply->version == BLK_VIRT_REBIND_VERSION &&
        reply->generation == generation &&
        (reply->hw_state == BLK_VIRT_HW_NONE || reply->hw_state == BLK_VIRT_HW_VIRTIO_BLK);
}
/* Stopped, detached device and retired bus required. Adopt already initialized
 * queues without ATTACH or clearing service metadata. Failed registration
 * retains backend ownership: detach before queue revocation. */
bool aos_vmm_virtio_blk_adopt(uint32_t media_id, void *shared_region,
                            const blk_virt_rebind_reply_t *attachment);
#endif
