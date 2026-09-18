/* Bounded framebuffer queue contract. Pixel traffic never uses IPC registers.
 * Each client has a separate root-provisioned region; only the framebuffer
 * service maps all clients. Surface storage is private to the service.
 * SPSC request/response rings use fixed private capacity and release/acquire
 * publication, like the other sDDF-shaped virtualizer contracts.
 */
#ifndef AOS_PLATFORM_FRAMEBUFFER_H
#define AOS_PLATFORM_FRAMEBUFFER_H

#include <stddef.h>
#include <stdint.h>

#define AOS_FB_VERSION 1u
#define AOS_FB_DETACH_VERSION 1u
#define AOS_FB_QUEUE_CAPACITY 16u
#define AOS_FB_DATA_BYTES 65536u
#define AOS_FB_MAX_SURFACES 4u
#define AOS_FB_MAX_WIDTH 1024u
#define AOS_FB_MAX_HEIGHT 768u
#define AOS_FB_PIXEL_BYTES 4u
#define AOS_FB_SURFACE_BYTES (AOS_FB_MAX_WIDTH * AOS_FB_MAX_HEIGHT * AOS_FB_PIXEL_BYTES)
#define AOS_FB_ARENA_BYTES (2u * AOS_FB_MAX_SURFACES * AOS_FB_SURFACE_BYTES)
#define AOS_FB_CLIENTS 2u
#define AOS_FB_SHMEM_VA 0x2c000000UL
#define AOS_FB_CLIENT_STRIDE 0x200000UL
#define AOS_FB_ARENA_VA 0x30000000UL
#define AOS_FB_ARENA_FRAMES (AOS_FB_CLIENTS * AOS_FB_ARENA_BYTES / AOS_FB_CLIENT_STRIDE)
_Static_assert(AOS_FB_ARENA_BYTES % AOS_FB_CLIENT_STRIDE == 0, "private arena alignment");

enum aos_fb_operation {
    AOS_FB_CREATE = 1, AOS_FB_WRITE, AOS_FB_FLIP,
    AOS_FB_STATUS, AOS_FB_READ, AOS_FB_DESTROY, AOS_FB_SELECT
};
enum aos_fb_status {
    AOS_FB_OK = 0, AOS_FB_BAD_VERSION, AOS_FB_BAD_OPERATION,
    AOS_FB_BAD_HANDLE, AOS_FB_BAD_BOUNDS, AOS_FB_NO_SPACE
};

/* XRGB8888, tightly packed rows. CREATE uses width/height; WRITE uses the
 * rectangle x/y/width/height and data_offset into the client's payload.
 * READ exports that rectangle from the last committed frame. FLIP snapshots
 * staging pixels atomically with respect to other service requests. STATUS
 * returns geometry and committed sequence. Successful CREATE returns a fresh
 * nonzero handle; destroyed handles never become valid again in this session.
 * A request's payload must remain owned by its producer until its response.
 */
typedef struct aos_fb_request {
    uint32_t version, operation, id, reserved;
    uint64_t handle;
    uint32_t x, y, width, height;
    uint32_t data_offset, data_length;
} aos_fb_request_t;

typedef struct aos_fb_response {
    uint32_t version, status, id, reserved;
    uint64_t handle, sequence;
    uint32_t width, height;
} aos_fb_response_t;

typedef struct aos_fb_region {
    uint32_t req_head, req_tail, resp_head, resp_tail;
    aos_fb_request_t requests[AOS_FB_QUEUE_CAPACITY];
    aos_fb_response_t responses[AOS_FB_QUEUE_CAPACITY];
    uint8_t data[AOS_FB_DATA_BYTES];
    /* One-shot terminal detach, independent of ring capacity. A stopped VMM
     * release-publishes version/request=1 and signals the service. The service
     * drops all client queue/surface pointers before its final page access:
     * release-store ack=1. Root initially clears these fields. A retired
     * client requires an explicit future reconstruction/generation protocol. */
    struct { uint32_t version, request, ack; } detach;
} aos_fb_region_t;

/* Private service state: never put these pointers or bounds in shared RAM. */
typedef struct aos_fb_surface {
    uint64_t handle, sequence;
    uint32_t width, height;
    uint8_t *staging, *committed;
} aos_fb_surface_t;

typedef struct aos_fb_client {
    aos_fb_region_t *region;
    uint64_t next_handle;
    uint64_t selected_handle;
    uint32_t selected_x, selected_y, selected_width, selected_height;
    aos_fb_surface_t surfaces[AOS_FB_MAX_SURFACES];
} aos_fb_client_t;

/* The arena is exclusively owned by the service and must have ARENA_BYTES.
 * The caller provisions/clears the shared region before client execution.
 * Return 0 on success, -1 for invalid configuration. */
int aos_fb_client_init(aos_fb_client_t *client, aos_fb_region_t *region,
                       uint8_t *arena, size_t arena_bytes);
/* A pump processes at most QUEUE_CAPACITY requests and never consumes a
 * request without space for its response. Callers signal the peer after
 * submitting requests AND after draining responses: this resumes work after
 * response backpressure. Use persistent notifications; no dropped NBSends.
 * Return responses published, or one for a terminal detach acknowledgment.
 * Detach takes priority even with full/malformed rings, abandons queued work,
 * and clears private surface pointers. Malformed rings otherwise make no
 * progress. Detached clients remain inert on subsequent pumps. */
unsigned aos_fb_pump(aos_fb_client_t *client);
int aos_fb_submit(aos_fb_region_t *region, const aos_fb_request_t *request);
int aos_fb_receive(aos_fb_region_t *region, aos_fb_response_t *response);

_Static_assert(sizeof(aos_fb_request_t) == 48u, "framebuffer request ABI");
_Static_assert(sizeof(aos_fb_response_t) == 40u, "framebuffer response ABI");
_Static_assert(sizeof(aos_fb_region_t) < 0x200000u, "client fits isolated large page");
#endif
