/*
 * Data-driven lifecycle state machine shared by all agentOS guest VMMs.
 */

#include <platform/guest_vmm_runtime.h>
#include <platform/console_input.h>

#include "agentos.h"
#include "contracts/cc_contract.h"
#include "contracts/guest_contract.h"

static bool valid_guest_id(const sel4_msg_t *req,
                           const aos_guest_vmm_runtime_t *runtime)
{
    return req->length >= sizeof(uint32_t) &&
           msg_u32(req, 0u) == runtime->guest_id;
}

bool aos_guest_vmm_lifecycle_rpc(const sel4_msg_t *req, sel4_msg_t *rep,
                                 const aos_guest_vmm_runtime_t *runtime)
{
    if (req == NULL || rep == NULL || runtime == NULL ||
        runtime->state == NULL || runtime->started == NULL) {
        return false;
    }

    switch (req->opcode) {
    case MSG_GUEST_CREATE: {
        uint32_t requested_os = req->length >= sizeof(uint32_t)
                              ? msg_u32(req, 0u) : 0u;
        if (requested_os != 0u && requested_os != runtime->os_type) {
            rep->opcode = GUEST_ERR_BAD_OS_TYPE;
            return true;
        }
        if (*runtime->state == GUEST_STATE_DEAD) {
            /*
             * A VMM remains terminal unless it explicitly supplies reset
             * support.  Do not expose a reusable target slot merely because
             * its lifecycle word was DEAD.
             */
            if (runtime->reset == NULL) {
                rep->opcode = GUEST_ERR_DEAD;
                return true;
            }
            if (!runtime->reset()) {
                rep->opcode = GUEST_ERR_NOT_READY;
                return true;
            }
            *runtime->started = false;
            *runtime->state = GUEST_STATE_READY;
        }
        rep_u32(rep, 0u, GUEST_OK);
        rep_u32(rep, 4u, runtime->guest_id);
        rep->length = 8u;
        rep->opcode = GUEST_OK;
        return true;
    }
    case MSG_GUEST_BOOT:
        if (!valid_guest_id(req, runtime)) {
            rep->opcode = GUEST_ERR_BAD_GUEST_ID;
        } else if (*runtime->state == GUEST_STATE_DEAD) {
            rep->opcode = GUEST_ERR_DEAD;
        } else if (*runtime->state != GUEST_STATE_READY &&
                   *runtime->state != GUEST_STATE_RUNNING) {
            rep->opcode = GUEST_ERR_BAD_STATE;
        } else if (!*runtime->started &&
                   (runtime->start == NULL || !runtime->start())) {
            rep->opcode = GUEST_ERR_NOT_READY;
        } else {
            *runtime->state = GUEST_STATE_RUNNING;
            rep->opcode = GUEST_OK;
        }
        return true;
    case MSG_GUEST_SUSPEND:
        if (!valid_guest_id(req, runtime)) {
            rep->opcode = GUEST_ERR_BAD_GUEST_ID;
        } else if (*runtime->state == GUEST_STATE_DEAD) {
            rep->opcode = GUEST_ERR_DEAD;
        } else if (!*runtime->started ||
                   (*runtime->state != GUEST_STATE_RUNNING &&
                    *runtime->state != GUEST_STATE_SUSPENDED)) {
            rep->opcode = GUEST_ERR_BAD_STATE;
        } else {
            if (*runtime->state != GUEST_STATE_SUSPENDED) {
                if (runtime->suspend == NULL || !runtime->suspend()) {
                    rep->opcode = GUEST_ERR_NOT_READY;
                    return true;
                }
                if (runtime->quiesce_timer != NULL) runtime->quiesce_timer();
                *runtime->state = GUEST_STATE_SUSPENDED;
            }
            rep->opcode = GUEST_OK;
        }
        return true;
    case MSG_GUEST_RESUME:
        if (!valid_guest_id(req, runtime)) {
            rep->opcode = GUEST_ERR_BAD_GUEST_ID;
        } else if (*runtime->state == GUEST_STATE_DEAD) {
            rep->opcode = GUEST_ERR_DEAD;
        } else if (!*runtime->started ||
                   (*runtime->state != GUEST_STATE_RUNNING &&
                    *runtime->state != GUEST_STATE_SUSPENDED)) {
            rep->opcode = GUEST_ERR_BAD_STATE;
        } else {
            if (*runtime->state == GUEST_STATE_SUSPENDED &&
                (runtime->resume == NULL || !runtime->resume())) {
                rep->opcode = GUEST_ERR_NOT_READY;
                return true;
            }
            *runtime->state = GUEST_STATE_RUNNING;
            rep->opcode = GUEST_OK;
        }
        return true;
    case MSG_GUEST_DESTROY:
        if (!valid_guest_id(req, runtime)) {
            rep->opcode = GUEST_ERR_BAD_GUEST_ID;
        } else {
            if (*runtime->state != GUEST_STATE_DEAD) {
                if (*runtime->state != GUEST_STATE_SUSPENDED &&
                    (runtime->suspend == NULL || !runtime->suspend())) {
                    rep->opcode = GUEST_ERR_NOT_READY;
                    return true;
                }
                if (*runtime->state != GUEST_STATE_SUSPENDED) {
                    if (runtime->quiesce_timer != NULL) {
                        runtime->quiesce_timer();
                    }
                    /*
                     * Suspend has already detached the execution context.
                     * If teardown then fails, retain that truthful state
                     * rather than falsely reporting RUNNING or READY.
                     */
                    *runtime->state = GUEST_STATE_SUSPENDED;
                }
                if (runtime->teardown != NULL && !runtime->teardown()) {
                    rep->opcode = GUEST_ERR_NOT_READY;
                    return true;
                }
                *runtime->started = false;
                *runtime->state = GUEST_STATE_DEAD;
            }
            rep->opcode = GUEST_OK;
        }
        return true;
    default:
        return false;
    }
}

bool aos_guest_vmm_console_rpc(const sel4_msg_t *req, sel4_msg_t *rep,
                               const aos_guest_vmm_runtime_t *runtime)
{
    if (req == NULL || rep == NULL || runtime == NULL ||
        runtime->state == NULL) {
        return false;
    }

    switch (req->opcode) {
    case MSG_GUEST_SEND_INPUT: {
        if (req->length < 28u || !valid_guest_id(req, runtime)) {
            rep->opcode = GUEST_ERR_BAD_GUEST_ID;
            return true;
        }
        if (*runtime->state == GUEST_STATE_DEAD) {
            rep->opcode = GUEST_ERR_DEAD;
            return true;
        }
        if (*runtime->state != GUEST_STATE_RUNNING) {
            rep->opcode = GUEST_ERR_BAD_STATE;
            return true;
        }

        uint8_t byte = 0u;
        const uint32_t event_type = msg_u32(req, 4u);
        const uint32_t keycode = msg_u32(req, 8u);
        const uint8_t *bytes = &req->data[28u];
        uint32_t length = 0u;
        if (event_type == CC_INPUT_TEXT) {
            if (keycode > CC_INPUT_TEXT_MAX || req->length < 28u + keycode) {
                rep->opcode = GUEST_ERR_PROTOCOL_VIOLATION;
                return true;
            }
            length = keycode;
        } else if (aos_guest_vmm_input_event_to_byte(
                       event_type, keycode, &byte)) {
            bytes = &byte;
            length = 1u;
        }
        if (length != 0u &&
            (runtime->push_input == NULL ||
             !runtime->push_input(event_type, bytes, length))) {
            rep->opcode = GUEST_ERR_DEVICE_UNAVAILABLE;
            return true;
        }
        rep->opcode = GUEST_OK;
        return true;
    }
    case MSG_GUEST_CONSOLE_DRAIN: {
        if (req->length < 8u || !valid_guest_id(req, runtime)) {
            rep->opcode = GUEST_ERR_BAD_GUEST_ID;
            return true;
        }
        if (*runtime->state == GUEST_STATE_DEAD) {
            rep->opcode = GUEST_ERR_DEAD;
            return true;
        }
        uint32_t capacity = msg_u32(req, 4u);
        if (capacity > SEL4_MSG_DATA_BYTES) capacity = SEL4_MSG_DATA_BYTES;
        rep->length = runtime->drain_console == NULL
                    ? 0u : runtime->drain_console(rep->data, capacity);
        if (rep->length > capacity) {
            rep->length = 0u;
            rep->opcode = GUEST_ERR_PROTOCOL_VIOLATION;
            return true;
        }
        rep->opcode = GUEST_OK;
        return true;
    }
    default:
        return false;
    }
}

bool aos_guest_vmm_input_event_to_byte(uint32_t event_type, uint32_t keycode,
                                       uint8_t *byte)
{
    return aos_console_input_event_to_byte(event_type, keycode, byte);
}
