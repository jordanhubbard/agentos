#include <platform/display.h>
#include <string.h>

static uint32_t load(const uint32_t *p) { return __atomic_load_n(p,__ATOMIC_ACQUIRE); }
static void publish(uint32_t *p,uint32_t v) { __atomic_store_n(p,v,__ATOMIC_RELEASE); }

int aos_display_init(aos_display_driver_t *d,aos_display_region_t *r,
                     uint8_t *a,uint8_t *b,size_t capacity,
                     int (*present)(void *,unsigned,uint32_t,uint32_t),void *context)
{
    const uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    if (!d || !r || !a || !b || !present || capacity<AOS_DISPLAY_FRAME_BYTES ||
        x>UINTPTR_MAX-capacity || y>UINTPTR_MAX-capacity ||
        (x<y+capacity && y<x+capacity)) return -1;
    *d=(aos_display_driver_t){.region=r,.banks={a,b},.present=present,.context=context};
    return 0;
}
int aos_display_submit(aos_display_region_t *r,const aos_display_request_t *q)
{
    if (!r || !q) return -1;
    uint32_t tail=load(&r->req_tail);
    if (tail!=load(&r->req_head) || tail!=load(&r->resp_head) ||
        tail!=load(&r->resp_tail)) return -1;
    r->request=*q; publish(&r->req_tail,tail+1u); return 0;
}
int aos_display_receive(aos_display_region_t *r,aos_display_response_t *p)
{
    if (!r || !p) return -1;
    uint32_t head=load(&r->resp_head);
    if (load(&r->resp_tail)-head!=1u) return -1;
    *p=r->response; publish(&r->resp_head,head+1u); return 0;
}
static uint32_t execute(aos_display_driver_t *d,const aos_display_request_t *q)
{
    if (d->failed) return AOS_DISPLAY_FAILED;
    if (q->version!=AOS_DISPLAY_VERSION || q->reserved) return AOS_DISPLAY_INVALID;
    if (q->operation==AOS_DISPLAY_BEGIN) {
        if (d->active) return AOS_DISPLAY_BUSY;
        if (q->cookie || q->offset || q->length || q->width<16 || q->width>1024 ||
            q->height<16 || q->height>768 || d->cookie==UINT64_MAX ||
            d->sequence==UINT64_MAX) return AOS_DISPLAY_INVALID;
        ++d->cookie; d->width=q->width; d->height=q->height;
        d->bytes=q->width*q->height*4u; d->written=0; d->active=1;
        return AOS_DISPLAY_OK;
    }
    if (!d->active || q->cookie!=d->cookie) return AOS_DISPLAY_STALE;
    if (q->width || q->height) return AOS_DISPLAY_INVALID;
    if (q->operation==AOS_DISPLAY_WRITE) {
        if (q->offset!=d->written || !q->length || q->length>AOS_DISPLAY_CHUNK ||
            q->length>d->bytes-d->written) return AOS_DISPLAY_INVALID;
        memcpy(d->banks[1u-d->front]+d->written,d->region->data,q->length);
        d->written+=q->length; return AOS_DISPLAY_OK;
    }
    if (q->offset || q->length) return AOS_DISPLAY_INVALID;
    if (q->operation==AOS_DISPLAY_ABORT) { d->active=0; return AOS_DISPLAY_OK; }
    if (q->operation!=AOS_DISPLAY_PRESENT) return AOS_DISPLAY_INVALID;
    if (d->written!=d->bytes) return AOS_DISPLAY_INCOMPLETE;
    if (d->present(d->context,1u-d->front,d->width,d->height)!=0) {
        d->failed=1; return AOS_DISPLAY_FAILED;
    }
    d->front=1u-d->front; ++d->sequence; d->active=0;
    return AOS_DISPLAY_OK;
}
unsigned aos_display_pump(aos_display_driver_t *d)
{
    if (!d || !d->region) return 0;
    aos_display_region_t *r=d->region;
    uint32_t head=load(&r->req_head),tail=load(&r->req_tail);
    if (tail-head!=1u || load(&r->resp_tail)!=head || load(&r->resp_head)!=head)
        return 0;
    const aos_display_request_t q=r->request;
    uint32_t status=execute(d,&q);
    r->response=(aos_display_response_t){.version=AOS_DISPLAY_VERSION,
        .status=status,.id=q.id,.cookie=d->cookie,.sequence=d->sequence};
    publish(&r->req_head,head+1u); publish(&r->resp_tail,head+1u);
    return 1;
}
