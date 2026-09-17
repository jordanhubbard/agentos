#ifndef AOS_PLATFORM_DISPLAY_PRODUCER_H
#define AOS_PLATFORM_DISPLAY_PRODUCER_H
#include <platform/display.h>
#include <platform/framebuffer.h>
typedef struct aos_display_producer {
    aos_display_region_t *region;
    void *context;
    int (*exchange)(void *,const aos_display_request_t *,aos_display_response_t *);
    uint64_t handle,sequence,driver_sequence;
    uint32_t x,y,width,height,id;
    unsigned failed;
} aos_display_producer_t;
/* Called by the single-threaded framebuffer service between pump passes.
 * The client's committed pixels must remain immutable until return. The
 * exchange callback owns a bounded wait and must never retry a submission.
 * Returns 1 for a new complete frame, 0 unchanged/absent, -1 failure. */
int aos_display_forward(aos_display_producer_t *,const aos_fb_client_t *);
#endif
