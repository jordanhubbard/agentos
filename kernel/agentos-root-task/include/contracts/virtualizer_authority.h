/* Root-minted virtualizer client authority. Current board: client/media N
 * belongs to VMM slot N. A request payload cannot grant another assignment.
 * Other clients need an explicit root assignment before receiving authority. */
#ifndef AGENTOS_VIRTUALIZER_AUTHORITY_H
#define AGENTOS_VIRTUALIZER_AUTHORITY_H
#include <stdbool.h>
#include <stdint.h>

#define VIRT_CLIENT_AUTHORITY_VERSION 1u
#define VIRT_CLIENT_BADGE_PRIMARY UINT64_C(0xa0510001)
#define VIRT_CLIENT_BADGE_SECONDARY UINT64_C(0xa0510002)
#define VIRT_NET_BADGE_NATIVE UINT64_C(0xa0510003)

static inline uint64_t virt_client_badge(uint32_t slot)
{
    return slot == 0u ? VIRT_CLIENT_BADGE_PRIMARY :
           slot == 1u ? VIRT_CLIENT_BADGE_SECONDARY : 0u;
}

static inline bool virt_client_authorized(uint64_t badge, uint32_t client,
                                         uint32_t slot)
{
    return slot < 2u && client == slot && badge == virt_client_badge(slot);
}

static inline bool virt_media_authorized(uint64_t badge, uint32_t client,
                                        uint32_t slot, uint32_t media)
{
    return virt_client_authorized(badge, client, slot) && media == slot;
}

/* The native network assignment grants no block/media or serial authority. */
static inline bool virt_net_authorized(uint64_t badge, uint32_t client,
                                       uint32_t slot)
{
    return virt_client_authorized(badge, client, slot) ||
           (badge == VIRT_NET_BADGE_NATIVE && client == 2u && slot == 2u);
}
#endif
