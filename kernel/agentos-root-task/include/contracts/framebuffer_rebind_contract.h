/* Graphics reconstruction control; pixels remain in shared queues. */
#ifndef AOS_FRAMEBUFFER_REBIND_CONTRACT_H
#define AOS_FRAMEBUFFER_REBIND_CONTRACT_H
#include <stdint.h>
#define FB_REBIND_VERSION 1u
#define FB_REBIND_STAGE 1u
#define FB_REBIND_COMMIT 2u
#define FB_REBIND_ABORT 3u
#define FB_REBIND_FRAMES 13u
/* STAGE transfers one empty owning pool, in index order: queue, then arena
 * pages. Only the queue stage returns a frame cap to the VMM. COMMIT/ABORT
 * carry no capabilities and use index zero. The service exposes no client
 * pointers until all mappings exist and COMMIT succeeds. ABORT first deletes
 * every staged service frame cap, then consumes this generation. The VMM
 * revokes its pools before retrying. Root badges authorize client selection. */
typedef struct { uint32_t version, client, generation, index; } fb_rebind_req_t;
typedef struct { uint32_t status, version, generation, index; } fb_rebind_reply_t;
enum fb_rebind_status {
    FB_REBIND_OK, FB_REBIND_BAD_REQUEST, FB_REBIND_DENIED,
    FB_REBIND_BAD_STATE, FB_REBIND_RESOURCE
};
_Static_assert(sizeof(fb_rebind_req_t)==16, "graphics rebind request ABI");
_Static_assert(sizeof(fb_rebind_reply_t)==16, "graphics rebind reply ABI");
#endif
