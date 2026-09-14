#ifndef AGENTOS_SERIAL_VIRT_CONTRACT_H
#define AGENTOS_SERIAL_VIRT_CONTRACT_H

#include <stdint.h>
#include "virtualizer_authority.h"

/* Draft implementation: root/VMM/CC integration and target proof pending.
 * Only ATTACH uses IPC. Console bytes use the shared sDDF byte queues.
 * Notifications are persistent seL4 Signals, not endpoint NBSends. */
#define SERIAL_VIRT_CONTRACT_VERSION 1u
#define SERIAL_VIRT_OP_ATTACH 0x2d01u
#define SERIAL_VIRT_ROLE_VMM 0u
#define SERIAL_VIRT_ROLE_FRONTEND 1u
#define SERIAL_VIRT_OK 0u
#define SERIAL_VIRT_ERR_VERSION 1u
#define SERIAL_VIRT_ERR_AUTHORITY 2u
#define SERIAL_VIRT_ERR_BUSY 3u
#define SERIAL_VIRT_ERR_PROTOCOL 4u
#define SERIAL_VIRT_FRONTEND_BADGE UINT64_C(0xa0510100)

/* Signals to a VMM use a bit distinct from legacy control notifications.
 * Signals to serial_virt use bits 0/1 for VMM clients and bit 2 for CC-PD.
 * Consumers rescan queues; combined notification bits lose no work. */
#define SERIAL_VIRT_VMM_WAKE_BADGE (UINT64_C(1) << 61)
#define SERIAL_VIRT_FRONTEND_WAKE_BADGE (UINT64_C(1) << 2)

typedef struct __attribute__((packed)) {
    uint32_t version;
    uint32_t client;
    uint32_t role;
} serial_virt_attach_req_t;

typedef struct __attribute__((packed)) {
    uint32_t status;
    uint32_t version;
} serial_virt_attach_reply_t;

static inline int serial_virt_authorized(uint64_t badge, uint32_t client,
                                         uint32_t role)
{
    if (client >= 2u) return 0;
    if (role == SERIAL_VIRT_ROLE_VMM)
        return virt_client_authorized(badge, client, client);
    return role == SERIAL_VIRT_ROLE_FRONTEND &&
           badge == SERIAL_VIRT_FRONTEND_BADGE;
}

_Static_assert(sizeof(serial_virt_attach_req_t) == 12u, "serial attach request ABI");
_Static_assert(sizeof(serial_virt_attach_reply_t) == 8u, "serial attach reply ABI");
#endif
