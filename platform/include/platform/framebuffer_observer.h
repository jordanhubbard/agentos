/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef AOS_FRAMEBUFFER_OBSERVER_H
#define AOS_FRAMEBUFFER_OBSERVER_H
#include <platform/framebuffer.h>

#define AOS_FB_OBSERVER_VERSION 1u
#define AOS_FB_OBSERVER_CLIENT AOS_FB_CLIENTS
#define AOS_FB_OBSERVER_VA (AOS_FB_SHMEM_VA + AOS_FB_CLIENTS * AOS_FB_CLIENT_STRIDE)
#define AOS_FB_SNAPSHOT_VA (AOS_FB_ARENA_VA + AOS_FB_CLIENTS * AOS_FB_ARENA_BYTES)
#define AOS_FB_SNAPSHOT_FRAMES ((AOS_FB_SURFACE_BYTES + AOS_FB_CLIENT_STRIDE - 1u) / AOS_FB_CLIENT_STRIDE)
enum aos_fb_observer_operation { AOS_FB_CAPTURE=1, AOS_FB_CAPTURE_READ, AOS_FB_CAPTURE_RELEASE };
enum aos_fb_observer_status {
    AOS_FB_OBSERVER_OK=0, AOS_FB_OBSERVER_BAD_REQUEST, AOS_FB_OBSERVER_DENIED,
    AOS_FB_OBSERVER_NO_FRAME, AOS_FB_OBSERVER_BAD_COOKIE, AOS_FB_OBSERVER_BAD_BOUNDS,
    AOS_FB_OBSERVER_EXHAUSTED
};

/* A separate root-granted observer region, never mapped by a guest/VMM.
 * CAPTURE names a client allowed by the observer's private mask. It copies
 * the selected committed rectangle into private snapshot storage and returns
 * a fresh cookie. READ uses cookie/offset/length, at most DATA_BYTES at once.
 * RELEASE invalidates the cookie. Operations cannot mutate guest surfaces.
 * All unused request fields must be zero. */
typedef struct {
    uint32_t version, operation, id, client;
    uint64_t cookie;
    uint32_t offset, length;
} aos_fb_observer_request_t;
typedef struct {
    uint32_t version, status, id, length;
    uint64_t cookie, sequence;
    uint32_t width, height;
} aos_fb_observer_response_t;
typedef struct {
    uint32_t req_head, req_tail, resp_head, resp_tail;
    aos_fb_observer_request_t requests[AOS_FB_QUEUE_CAPACITY];
    aos_fb_observer_response_t responses[AOS_FB_QUEUE_CAPACITY];
    uint8_t data[AOS_FB_DATA_BYTES];
} aos_fb_observer_region_t;

typedef struct {
    aos_fb_observer_region_t *region;
    aos_fb_client_t *clients;
    uint32_t client_count, allowed_mask;
    uint8_t *snapshot;
    uint64_t next_cookie, cookie, sequence;
    uint32_t width, height, bytes;
} aos_fb_observer_t;

int aos_fb_observer_init(aos_fb_observer_t *, aos_fb_observer_region_t *,
    aos_fb_client_t *, uint32_t client_count, uint32_t allowed_mask,
    void *private_snapshot, size_t bytes);
/* At most one response outstanding: shared READ bytes remain stable until
 * the observer consumes their response. Signal after submitting requests and
 * after draining responses, as for the framebuffer client queues. */
unsigned aos_fb_observer_pump(aos_fb_observer_t *);
int aos_fb_observer_submit(aos_fb_observer_region_t *, const aos_fb_observer_request_t *);
int aos_fb_observer_receive(aos_fb_observer_region_t *, aos_fb_observer_response_t *);
_Static_assert(sizeof(aos_fb_observer_request_t)==32, "observer request ABI");
_Static_assert(sizeof(aos_fb_observer_response_t)==40, "observer response ABI");
_Static_assert(sizeof(aos_fb_observer_region_t)<AOS_FB_CLIENT_STRIDE, "observer page bounds");
#endif
