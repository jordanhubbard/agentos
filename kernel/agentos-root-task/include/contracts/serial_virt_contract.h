#ifndef AGENTOS_SERIAL_VIRT_CONTRACT_H
#define AGENTOS_SERIAL_VIRT_CONTRACT_H

#include <stdint.h>
#include "virtualizer_authority.h"

/* Only attachment control uses IPC. Console bytes use shared sDDF byte queues.
 * Notifications are persistent seL4 Signals, not endpoint NBSends. */
#define SERIAL_VIRT_CONTRACT_VERSION 4u
#define SERIAL_VIRT_OP_ATTACH 0x2d01u
/* Version 4 terminal guest detach: same layouts as attach, VMM role only.
 * Frontend queue access uses the shared admission gate. Detach closes it
 * permanently and returns BUSY while an admitted operation is in progress.
 * The producer must stop before calling. OK retires the service's pointers
 * to this guest page and marks its frontend detached. Pending terminal bytes
 * may be abandoned; no viewer drain is required. Repeated detach is OK;
 * reattachment requires a future generation/reset contract. Other guest and
 * operator channels are unaffected. This does not revoke page capabilities. */
#define SERIAL_VIRT_OP_DETACH 0x2d02u
#define SERIAL_VIRT_ROLE_VMM 0u
#define SERIAL_VIRT_ROLE_FRONTEND 1u
#define SERIAL_VIRT_ROLE_OPERATOR 2u
#define SERIAL_VIRT_OPERATOR_CLIENT 2u
#define SERIAL_VIRT_OPERATOR_BADGE UINT64_C(0xa0510101)
#define SERIAL_VIRT_OK 0u
#define SERIAL_VIRT_ERR_VERSION 1u
#define SERIAL_VIRT_ERR_AUTHORITY 2u
#define SERIAL_VIRT_ERR_BUSY 3u
#define SERIAL_VIRT_ERR_PROTOCOL 4u
#define SERIAL_VIRT_FRONTEND_BADGE UINT64_C(0xa0510100)

/* Signals to a VMM use a bit distinct from legacy control notifications.
 * Signals to serial_virt use bits 0/1 for VMMs, 2 for operator and 3 for CC-PD.
 * Consumers rescan queues; combined notification bits lose no work. */
#define SERIAL_VIRT_VMM_WAKE_BADGE (UINT64_C(1) << 61)
#define SERIAL_VIRT_FRONTEND_WAKE_BADGE (UINT64_C(1) << 3)
#define SERIAL_VIRT_OPERATOR_WAKE_BADGE (UINT64_C(1) << 2)

/* Bound notifications carry a notification word, not an IPC message tag.
 * Classify these badges before inspecting a receive's label or registers. */
static inline int serial_virt_service_notification(uint64_t badge)
{
    return badge != 0 && (badge & ~UINT64_C(15)) == 0;
}
static inline int serial_virt_vmm_notification(uint64_t badge)
{
    return (badge & SERIAL_VIRT_VMM_WAKE_BADGE) != 0;
}

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
    if (client >= 3u) return 0;
    if (role == SERIAL_VIRT_ROLE_OPERATOR)
        return client == SERIAL_VIRT_OPERATOR_CLIENT && badge == SERIAL_VIRT_OPERATOR_BADGE;
    if (role == SERIAL_VIRT_ROLE_VMM)
        return client < 2u && virt_client_authorized(badge, client, client);
    return role == SERIAL_VIRT_ROLE_FRONTEND &&
           badge == SERIAL_VIRT_FRONTEND_BADGE;
}

_Static_assert(sizeof(serial_virt_attach_req_t) == 12u, "serial attach request ABI");
_Static_assert(sizeof(serial_virt_attach_reply_t) == 8u, "serial attach reply ABI");
#endif
