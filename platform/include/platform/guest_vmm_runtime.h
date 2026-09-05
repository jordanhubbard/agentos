/*
 * Shared lifecycle state machine for guest VMM protection domains.
 *
 * Guest flavor is data (os_type plus callbacks); Linux and FreeBSD must not
 * grow independent implementations of the common CREATE/BOOT/SUSPEND/
 * RESUME/DESTROY wire protocol.
 */
#ifndef AOS_PLATFORM_GUEST_VMM_RUNTIME_H
#define AOS_PLATFORM_GUEST_VMM_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "sel4_msg_types.h"

typedef struct aos_guest_vmm_runtime {
    uint32_t os_type;
    uint32_t guest_id;
    uint32_t *state;
    bool *started;
    bool (*start)(void);
    void (*suspend)(void);
    void (*resume)(void);
    void (*quiesce_timer)(void);
    bool (*push_input)(uint32_t event_type, const uint8_t *bytes,
                       uint32_t length);
    uint32_t (*drain_console)(uint8_t *bytes, uint32_t capacity);
} aos_guest_vmm_runtime_t;

/*
 * Handle a common lifecycle opcode. Returns true when req->opcode belongs to
 * the common lifecycle protocol, including validation failures.
 */
bool aos_guest_vmm_lifecycle_rpc(const sel4_msg_t *req, sel4_msg_t *rep,
                                 const aos_guest_vmm_runtime_t *runtime);

/* Handle the common SEND_INPUT and CONSOLE_DRAIN wire protocol. */
bool aos_guest_vmm_console_rpc(const sel4_msg_t *req, sel4_msg_t *rep,
                               const aos_guest_vmm_runtime_t *runtime);

/* Shared CC key-event decoder used by every guest console frontend. */
bool aos_guest_vmm_input_event_to_byte(uint32_t event_type, uint32_t keycode,
                                       uint8_t *byte);

#endif /* AOS_PLATFORM_GUEST_VMM_RUNTIME_H */
