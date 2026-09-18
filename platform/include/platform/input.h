/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef AOS_PLATFORM_INPUT_H
#define AOS_PLATFORM_INPUT_H
#include <stddef.h>
#include <stdint.h>

#define AOS_INPUT_VERSION 1u
/* Version 2 with count=0 requests asynchronous release of all held state for
 * one client/device. Reserved words remain zero; response accepted is zero.
 * Version 1 batches and their wire sizes remain unchanged. */
#define AOS_INPUT_RELEASE_VERSION 2u
#define AOS_INPUT_CLIENTS 2u
#define AOS_INPUT_DEVICES 2u
#define AOS_INPUT_BATCH_EVENTS 64u
#define AOS_INPUT_EVENT_CAPACITY 256u
#define AOS_INPUT_REQUEST_CAPACITY 16u
#define AOS_INPUT_FRAME_SIZE 0x200000UL
#define AOS_INPUT_SHMEM_VA 0x2d000000UL
#define AOS_INPUT_FRONTEND_VA (AOS_INPUT_SHMEM_VA + AOS_INPUT_CLIENTS * AOS_INPUT_FRAME_SIZE)
#define AOS_INPUT_VMM_WAKE_BADGE (UINT64_C(1) << 58)

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
/* One-shot terminal detach on the existing per-VMM page. Root initializes
 * all words to zero. The stopped VMM publishes version then request=1 and
 * signals input_virt. The service clears private admission, held/release
 * state and pointers before publishing ack=1 and waking that VMM. No service
 * access to this page follows the acknowledgment. Pending events may be
 * abandoned; a new generation requires a separate reset contract. Event and
 * frontend wire layouts are unchanged. Page ownership authorizes detach. */
#define AOS_INPUT_DETACH_VERSION 1u
typedef struct { uint32_t version, request, ack; } aos_input_detach_t;
typedef struct {
    aos_input_event_queue_t devices[AOS_INPUT_DEVICES];
    aos_input_detach_t detach;
} aos_input_client_region_t;
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
    uint32_t held[AOS_INPUT_CLIENTS][AOS_INPUT_DEVICES][8];
    uint32_t releasing[AOS_INPUT_CLIENTS];
} aos_input_service_t;

int aos_input_service_init(aos_input_service_t *, aos_input_frontend_t *,
    aos_input_client_region_t *const clients[AOS_INPUT_CLIENTS], uint32_t allowed_mask);
int aos_input_submit(aos_input_frontend_t *, const aos_input_request_t *);
int aos_input_receive(aos_input_frontend_t *, aos_input_response_t *);
int aos_input_event_receive(aos_input_event_queue_t *, aos_input_event_t *);
/* One bounded pass. Return activity count and a bitmask of clients receiving
 * events or detach acknowledgments. Signal frontend after responses, VMMs after events,
 * and service after submitting requests or draining responses/events.
 * A full event queue accepts none of the batch (WOULD_BLOCK); no partial
 * keyboard/pointer state is published. Full response queues defer work.
 * Accepted releases persist privately across backpressure and reject new
 * batches for that client/device until every release is queued. Queues are
 * never reset or overwritten. Completion requires the guest to drain them.
 * Attach never clears queues. Lifecycle reset requires coordinated quiescence. */
unsigned aos_input_pump(aos_input_service_t *, uint32_t *clients_ready);

_Static_assert(sizeof(aos_input_event_t)==8, "virtio input event ABI");
_Static_assert(sizeof(aos_input_request_t)==544, "input request ABI");
_Static_assert(sizeof(aos_input_response_t)==16, "input response ABI");
_Static_assert(sizeof(aos_input_client_region_t)<AOS_INPUT_FRAME_SIZE, "input client page bounds");
_Static_assert(sizeof(aos_input_frontend_t)<AOS_INPUT_FRAME_SIZE, "input frontend page bounds");
#endif
