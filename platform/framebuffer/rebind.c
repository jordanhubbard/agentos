#include <platform/framebuffer_rebind.h>
#include "contracts/virtualizer_authority.h"
#include "contracts/guest_graphics_caps.h"
_Static_assert(FB_REBIND_FRAMES==AOS_GUEST_GRAPHICS_POOL_COUNT,"graphics pool ABI");

uint32_t aos_fb_rebind_validate(const aos_fb_client_t *c, const aos_fb_rebind_t *s,
    uint64_t badge, uint32_t op, const fb_rebind_req_t *q, size_t length)
{
    if (!c || !s || !q || length!=sizeof(*q) || q->version!=FB_REBIND_VERSION ||
        op<FB_REBIND_STAGE || op>FB_REBIND_ABORT) return FB_REBIND_BAD_REQUEST;
    if (q->client>=AOS_FB_CLIENTS || !virt_client_authorized(badge,q->client,q->client))
        return FB_REBIND_DENIED;
    if (c->region || !c->retired || c->generation==UINT32_MAX ||
        !q->generation || q->generation!=c->generation+1u ||
        (s->next_frame && s->generation!=q->generation) ||
        (!s->next_frame && s->generation)) return FB_REBIND_BAD_STATE;
    if (op==FB_REBIND_STAGE)
        return q->index<FB_REBIND_FRAMES && q->index==s->next_frame ?
            FB_REBIND_OK : FB_REBIND_BAD_STATE;
    if (q->index) return FB_REBIND_BAD_REQUEST;
    if (op==FB_REBIND_COMMIT)
        return s->next_frame==FB_REBIND_FRAMES ? FB_REBIND_OK : FB_REBIND_BAD_STATE;
    return s->next_frame && s->next_frame<=FB_REBIND_FRAMES ?
        FB_REBIND_OK : FB_REBIND_BAD_STATE;
}
uint32_t aos_fb_rebind_staged(aos_fb_client_t *c, aos_fb_rebind_t *s,
    uint64_t badge, const fb_rebind_req_t *q)
{
    uint32_t status=aos_fb_rebind_validate(c,s,badge,FB_REBIND_STAGE,q,sizeof(*q));
    if (status!=FB_REBIND_OK) return status;
    s->generation=q->generation;
    ++s->next_frame;
    return FB_REBIND_OK;
}
uint32_t aos_fb_rebind_commit(aos_fb_client_t *c, aos_fb_rebind_t *s,
    uint64_t badge, const fb_rebind_req_t *q, aos_fb_region_t *region, uint8_t *arena)
{
    uint32_t status=aos_fb_rebind_validate(c,s,badge,FB_REBIND_COMMIT,q,sizeof(*q));
    if (status!=FB_REBIND_OK) return status;
    if (aos_fb_client_rebind(c,region,arena,AOS_FB_ARENA_BYTES,q->generation))
        return FB_REBIND_RESOURCE;
    *s=(aos_fb_rebind_t){0};
    return FB_REBIND_OK;
}
uint32_t aos_fb_rebind_aborted(aos_fb_client_t *c, aos_fb_rebind_t *s,
    uint64_t badge, const fb_rebind_req_t *q)
{
    uint32_t status=aos_fb_rebind_validate(c,s,badge,FB_REBIND_ABORT,q,sizeof(*q));
    if (status!=FB_REBIND_OK) return status;
    c->generation=q->generation;
    *s=(aos_fb_rebind_t){0};
    return FB_REBIND_OK;
}
