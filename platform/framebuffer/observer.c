/* SPDX-License-Identifier: BSD-2-Clause */
#include <platform/framebuffer_observer.h>
#include <string.h>

static uint32_t load(const uint32_t *p) { return __atomic_load_n(p,__ATOMIC_ACQUIRE); }
static void publish(uint32_t *p,uint32_t n) { __atomic_store_n(p,n,__ATOMIC_RELEASE); }

int aos_fb_observer_init(aos_fb_observer_t *o, aos_fb_observer_region_t *r,
    aos_fb_client_t *clients, uint32_t count, uint32_t mask, void *snapshot, size_t bytes)
{
    if (!o || !r || !clients || !count || count>AOS_FB_CLIENTS ||
        (mask & ~((1u<<count)-1u)) || !snapshot || bytes<AOS_FB_SURFACE_BYTES) return -1;
    *o=(aos_fb_observer_t){.region=r,.clients=clients,.client_count=count,
        .allowed_mask=mask,.snapshot=snapshot,.next_cookie=1};
    return 0;
}
int aos_fb_observer_submit(aos_fb_observer_region_t *r,const aos_fb_observer_request_t *q)
{
    if (!r || !q) return -1;
    uint32_t head=load(&r->req_head), tail=load(&r->req_tail);
    if (tail-head>=AOS_FB_QUEUE_CAPACITY) return -1;
    r->requests[tail%AOS_FB_QUEUE_CAPACITY]=*q;
    publish(&r->req_tail,tail+1u);
    return 0;
}
int aos_fb_observer_receive(aos_fb_observer_region_t *r,aos_fb_observer_response_t *p)
{
    if (!r || !p) return -1;
    uint32_t head=load(&r->resp_head),tail=load(&r->resp_tail);
    if (head==tail || tail-head>AOS_FB_QUEUE_CAPACITY) return -1;
    *p=r->responses[head%AOS_FB_QUEUE_CAPACITY];
    publish(&r->resp_head,head+1u);
    return 0;
}
static aos_fb_observer_response_t execute(aos_fb_observer_t *o,const aos_fb_observer_request_t *q)
{
    aos_fb_observer_response_t p={.version=AOS_FB_OBSERVER_VERSION,.id=q->id};
    if (q->version!=AOS_FB_OBSERVER_VERSION || q->operation<AOS_FB_CAPTURE ||
        q->operation>AOS_FB_CAPTURE_RELEASE) { p.status=AOS_FB_OBSERVER_BAD_REQUEST; return p; }
    if (q->operation==AOS_FB_CAPTURE) {
        if (q->cookie || q->offset || q->length) { p.status=AOS_FB_OBSERVER_BAD_REQUEST; return p; }
        if (q->client>=o->client_count || !(o->allowed_mask & (1u<<q->client))) {
            p.status=AOS_FB_OBSERVER_DENIED; return p;
        }
        aos_fb_client_t *c=&o->clients[q->client];
        aos_fb_surface_t *s=NULL;
        for (unsigned i=0; c->selected_handle && i<AOS_FB_MAX_SURFACES; ++i)
            if (c->surfaces[i].handle==c->selected_handle) { s=&c->surfaces[i]; break; }
        if (!s || !s->sequence) { p.status=AOS_FB_OBSERVER_NO_FRAME; return p; }
        if (!o->next_cookie) { p.status=AOS_FB_OBSERVER_EXHAUSTED; return p; }
        /* Selection and geometry are private validated service state. */
        uint32_t row_bytes=c->selected_width*AOS_FB_PIXEL_BYTES;
        for (uint32_t row=0; row<c->selected_height; ++row)
            memcpy(o->snapshot+row*row_bytes,
                s->committed+((c->selected_y+row)*s->width+c->selected_x)*AOS_FB_PIXEL_BYTES,row_bytes);
        o->cookie=o->next_cookie++;
        o->sequence=s->sequence;
        o->width=c->selected_width; o->height=c->selected_height;
        o->bytes=o->width*o->height*AOS_FB_PIXEL_BYTES;
    } else {
        if (q->client) { p.status=AOS_FB_OBSERVER_BAD_REQUEST; return p; }
        if (!q->cookie || q->cookie!=o->cookie) { p.status=AOS_FB_OBSERVER_BAD_COOKIE; return p; }
        if (q->operation==AOS_FB_CAPTURE_READ) {
            if (!q->length || q->length>AOS_FB_DATA_BYTES || q->offset>o->bytes ||
                q->length>o->bytes-q->offset) { p.status=AOS_FB_OBSERVER_BAD_BOUNDS; return p; }
            memcpy(o->region->data,o->snapshot+q->offset,q->length);
            p.length=q->length;
        } else {
            if (q->offset || q->length) { p.status=AOS_FB_OBSERVER_BAD_REQUEST; return p; }
            o->cookie=0;
        }
    }
    p.cookie=o->cookie; p.sequence=o->sequence;
    p.width=o->width; p.height=o->height;
    return p;
}
unsigned aos_fb_observer_pump(aos_fb_observer_t *o)
{
    if (!o || !o->region) return 0;
    aos_fb_observer_region_t *r=o->region;
    uint32_t head=load(&r->req_head),tail=load(&r->req_tail);
    uint32_t out=load(&r->resp_tail),consumed=load(&r->resp_head);
    if (head==tail || tail-head>AOS_FB_QUEUE_CAPACITY || out!=consumed) return 0;
    aos_fb_observer_request_t q=r->requests[head%AOS_FB_QUEUE_CAPACITY];
    r->responses[out%AOS_FB_QUEUE_CAPACITY]=execute(o,&q);
    publish(&r->resp_tail,out+1u);
    publish(&r->req_head,head+1u);
    return 1;
}
