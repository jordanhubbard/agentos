/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef AOS_PLATFORM_INPUT_H
#define AOS_PLATFORM_INPUT_H
#include <stddef.h>
#include <stdint.h>

#define AOS_INPUT_VERSION 1u
#define AOS_INPUT_CLIENTS 2u
#define AOS_INPUT_DEVICES 2u
#define AOS_INPUT_BATCH_EVENTS 64u
#define AOS_INPUT_EVENT_CAPACITY 256u
#define AOS_INPUT_REQUEST_CAPACITY 16u
#define AOS_INPUT_FRAME_SIZE 0x200000UL
#define AOS_INPUT_SHMEM_VA 0x2d000000UL
#define AOS_INPUT_FRONTEND_VA (AOS_INPUT_SHMEM_VA + AOS_INPUT_CLIENTS * AOS_INPUT_FRAME_SIZE)

enum aos_input_device { AOS_INPUT_KEYBOARD=0, AOS_INPUT_POINTER=1 };
enum aos_input_status {
    AOS_INPUT_OK=0, AOS_INPUT_BAD_REQUEST, AOS_INPUT_DENIED, AOS_INPUT_WOULD_BLOCK
};
/* Linux input event payload used by virtio-input, in little-endian wire order.
 * Keyboard: EV_KEY codes 1..255, values release/press/repeat (0..2).
 * Pointer: EV_KEY buttons 0x110..0x117 (0..1), EV_REL X/Y/wheels (0,1,6,8).
 * Every atomic batch ends with exactly one EV_SYN/SYN_REPORT (0,0,0).
 * Capability advertising by the guest backend must match these bounds. */
typedef struct { uint16_t type, code; int32_t value; } aos_input_event_t;
typedef struct {
    uint32_t head, tail;
    aos_input_event_t events[AOS_INPUT_EVENT_CAPACITY];
} aos_input_event_queue_t;
/* One separately root-granted page per VMM; no VMM maps its peer or frontend.
 * The input virtualizer alone produces events. The VMM consumes them into
 * its guest's emulated virtio-input event queue after GPA validation. */
typedef struct { aos_input_event_queue_t devices[AOS_INPUT_DEVICES]; } aos_input_client_region_t;
typedef struct {
    uint32_t version, id, client, device, count, reserved[3];
    aos_input_event_t events[AOS_INPUT_BATCH_EVENTS];
} aos_input_request_t;
typedef struct { uint32_t version, id, status, accepted; } aos_input_response_t;
typedef struct {
    uint32_t req_head, req_tail, resp_head, resp_tail;
    aos_input_request_t requests[AOS_INPUT_REQUEST_CAPACITY];
    aos_input_response_t responses[AOS_INPUT_REQUEST_CAPACITY];
} aos_input_frontend_t;
/* Private authorization and pointers never reside in the shared pages. */
typedef struct {
    aos_input_frontend_t *frontend;
    aos_input_client_region_t *clients[AOS_INPUT_CLIENTS];
    uint32_t allowed_mask;
} aos_input_service_t;

int aos_input_service_init(aos_input_service_t *, aos_input_frontend_t *,
    aos_input_client_region_t *const clients[AOS_INPUT_CLIENTS], uint32_t allowed_mask);
int aos_input_submit(aos_input_frontend_t *, const aos_input_request_t *);
int aos_input_receive(aos_input_frontend_t *, aos_input_response_t *);
int aos_input_event_receive(aos_input_event_queue_t *, aos_input_event_t *);
/* One bounded pass. Return response count and a bitmask of clients receiving
 * events. Signal frontend after responses, corresponding VMMs after events,
 * and service after submitting requests or draining responses/events.
 * A full event queue accepts none of the batch (WOULD_BLOCK); no partial
 * keyboard/pointer state is published. Full response queues defer work.
 * Attach never clears queues. Lifecycle reset requires coordinated quiescence. */
unsigned aos_input_pump(aos_input_service_t *, uint32_t *clients_ready);

_Static_assert(sizeof(aos_input_event_t)==8, "virtio input event ABI");
_Static_assert(sizeof(aos_input_request_t)==544, "input request ABI");
_Static_assert(sizeof(aos_input_response_t)==16, "input response ABI");
_Static_assert(sizeof(aos_input_client_region_t)<AOS_INPUT_FRAME_SIZE, "input client page bounds");
_Static_assert(sizeof(aos_input_frontend_t)<AOS_INPUT_FRAME_SIZE, "input frontend page bounds");
#endif
