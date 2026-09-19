#ifndef AOS_FRAMEBUFFER_REBIND_H
#define AOS_FRAMEBUFFER_REBIND_H
#include <platform/framebuffer.h>
#include "contracts/framebuffer_rebind_contract.h"
typedef struct { uint32_t generation, next_frame; } aos_fb_rebind_t;
uint32_t aos_fb_rebind_validate(const aos_fb_client_t *, const aos_fb_rebind_t *,
    uint64_t badge, uint32_t operation, const fb_rebind_req_t *, size_t length);
/* Call only after this stage's cap retype/map succeeds. Failed stages do not
 * advance; discard their partial cap before retry or abort. */
uint32_t aos_fb_rebind_staged(aos_fb_client_t *, aos_fb_rebind_t *,
    uint64_t badge, const fb_rebind_req_t *);
uint32_t aos_fb_rebind_commit(aos_fb_client_t *, aos_fb_rebind_t *,
    uint64_t badge, const fb_rebind_req_t *, aos_fb_region_t *, uint8_t *arena);
/* Only after all staged service caps have been deleted. This does not touch
 * shared memory or revoke VMM pools; it leaves the client retired. */
uint32_t aos_fb_rebind_aborted(aos_fb_client_t *, aos_fb_rebind_t *,
    uint64_t badge, const fb_rebind_req_t *);
#endif
