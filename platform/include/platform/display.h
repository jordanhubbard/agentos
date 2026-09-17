/* Single-producer/single-consumer display queue: framebuffer service ->
 * display driver. Neither guest maps this page or the private scanout banks.
 * One request may be outstanding; payload ownership returns with its reply.
 */
#ifndef AOS_PLATFORM_DISPLAY_H
#define AOS_PLATFORM_DISPLAY_H
#include <stddef.h>
#include <stdint.h>
#define AOS_DISPLAY_VERSION 1u
#define AOS_DISPLAY_CHUNK 65536u
#define AOS_DISPLAY_FRAME_BYTES (1024u * 768u * 4u)
enum aos_display_op { AOS_DISPLAY_BEGIN=1, AOS_DISPLAY_WRITE,
                      AOS_DISPLAY_PRESENT, AOS_DISPLAY_ABORT };
enum aos_display_status { AOS_DISPLAY_OK, AOS_DISPLAY_INVALID,
    AOS_DISPLAY_BUSY, AOS_DISPLAY_STALE, AOS_DISPLAY_INCOMPLETE, AOS_DISPLAY_FAILED };
typedef struct aos_display_request {
    uint32_t version, operation, id, reserved;
    uint64_t cookie;
    uint32_t width, height, offset, length;
} aos_display_request_t;
typedef struct aos_display_response {
    uint32_t version, status, id, reserved;
    uint64_t cookie, sequence;
} aos_display_response_t;
typedef struct aos_display_region {
    uint32_t req_head, req_tail, resp_head, resp_tail;
    aos_display_request_t request;
    aos_display_response_t response;
    uint8_t data[AOS_DISPLAY_CHUNK];
} aos_display_region_t;
typedef struct aos_display_driver {
    aos_display_region_t *region;
    uint8_t *banks[2];
    void *context;
    int (*present)(void *, unsigned bank, uint32_t width, uint32_t height);
    uint64_t cookie, sequence;
    uint32_t width, height, written, bytes;
    unsigned front, active, failed;
} aos_display_driver_t;
/* Banks are exclusively driver-owned and cannot overlap. present resolves
 * the bank to its trusted physical allocation, never a queue-supplied GPA.
 * Return success only after device configuration DMA completes. */
int aos_display_init(aos_display_driver_t *, aos_display_region_t *,
                     uint8_t *, uint8_t *, size_t bank_capacity,
                     int (*present)(void *, unsigned, uint32_t, uint32_t), void *);
int aos_display_submit(aos_display_region_t *, const aos_display_request_t *);
int aos_display_receive(aos_display_region_t *, aos_display_response_t *);
unsigned aos_display_pump(aos_display_driver_t *);
_Static_assert(sizeof(aos_display_request_t)==40, "display request ABI");
_Static_assert(sizeof(aos_display_response_t)==32, "display response ABI");
#endif
