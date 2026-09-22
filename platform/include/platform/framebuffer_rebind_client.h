#ifndef AOS_FRAMEBUFFER_REBIND_CLIENT_H
#define AOS_FRAMEBUFFER_REBIND_CLIENT_H
#include <stdbool.h>
#include <stddef.h>
#include "contracts/framebuffer_rebind_contract.h"
static inline bool aos_fb_rebind_reply_valid(uint32_t op,const fb_rebind_req_t *q,
    const fb_rebind_reply_t *p,size_t length,unsigned caps,unsigned unwrapped)
{
    return q && p && op>=FB_REBIND_STAGE && op<=FB_REBIND_ABORT &&
        length==sizeof(*p) && p->version==FB_REBIND_VERSION &&
        p->generation==q->generation && p->index==q->index &&
        p->status<=FB_REBIND_RESOURCE && !unwrapped &&
        caps==(unsigned)(p->status==FB_REBIND_OK && op==FB_REBIND_STAGE && q->index==0);
}
/* Exactly one protocol exchange. STAGE(0) receives the queue frame into the
 * empty graphics slot; arena frames stay service-only. Output is unchanged
 * on invalid replies. False does NOT prove the service rejected the request:
 * the stopped reconstruction coordinator must retain attempted ownership.
 * Complete ABORT before pool revocation, or normal detach after COMMIT.
 * Map the queue before COMMIT so an attached generation can always detach. */
bool aos_fb_virt_rebind_exchange(uint32_t operation,const fb_rebind_req_t *,fb_rebind_reply_t *);
bool aos_fb_virt_map_queue(uint32_t client);
#endif
