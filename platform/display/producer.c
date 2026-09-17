#include <platform/display_producer.h>
#include <string.h>

static int exchange(aos_display_producer_t *p,aos_display_request_t q,
                    uint64_t *cookie)
{
    if (p->id==UINT32_MAX) return -1;
    q.version=AOS_DISPLAY_VERSION; q.id=++p->id;
    aos_display_response_t response={0};
    if (p->exchange(p->context,&q,&response)!=0 ||
        response.version!=AOS_DISPLAY_VERSION || response.id!=q.id ||
        response.reserved || response.status!=AOS_DISPLAY_OK ||
        !response.cookie || (q.cookie && response.cookie!=q.cookie)) return -1;
    uint64_t expected=p->driver_sequence+(q.operation==AOS_DISPLAY_PRESENT);
    if (response.sequence!=expected) return -1;
    *cookie=response.cookie;
    return 0;
}
int aos_display_forward(aos_display_producer_t *p,const aos_fb_client_t *c)
{
    if (!p || !c || !p->region || !p->exchange || p->failed) return -1;
    const aos_fb_surface_t *s=0;
    for (unsigned i=0;c->selected_handle && i<AOS_FB_MAX_SURFACES;++i)
        if (c->surfaces[i].handle==c->selected_handle) { s=&c->surfaces[i]; break; }
    if (!s || !s->sequence) return 0;
    uint32_t w=c->selected_width,h=c->selected_height,x=c->selected_x,y=c->selected_y;
    if (p->handle==s->handle && p->sequence==s->sequence && p->x==x && p->y==y &&
        p->width==w && p->height==h) return 0;
    if (!s->committed || s->width>AOS_FB_MAX_WIDTH || s->height>AOS_FB_MAX_HEIGHT ||
        w<16 || h<16 || x>s->width || w>s->width-x ||
        y>s->height || h>s->height-y || p->driver_sequence==UINT64_MAX) return -1;
    uint64_t cookie=0;
    aos_display_request_t q={.operation=AOS_DISPLAY_BEGIN,.width=w,.height=h};
    if (exchange(p,q,&cookie)!=0) goto failed;
    const uint32_t row_bytes=w*4u,bytes=row_bytes*h;
    for (uint32_t offset=0;offset<bytes;) {
        uint32_t count=bytes-offset;
        if (count>AOS_DISPLAY_CHUNK) count=AOS_DISPLAY_CHUNK;
        for (uint32_t copied=0;copied<count;) {
            uint32_t at=offset+copied,row=at/row_bytes,column=at%row_bytes;
            uint32_t part=row_bytes-column;
            if (part>count-copied) part=count-copied;
            memcpy(p->region->data+copied,
                s->committed+((size_t)(y+row)*s->width+x)*4u+column,part);
            copied+=part;
        }
        q=(aos_display_request_t){.operation=AOS_DISPLAY_WRITE,.cookie=cookie,
            .offset=offset,.length=count};
        if (exchange(p,q,&cookie)!=0) goto failed;
        offset+=count;
    }
    q=(aos_display_request_t){.operation=AOS_DISPLAY_PRESENT,.cookie=cookie};
    if (exchange(p,q,&cookie)!=0) goto failed;
    ++p->driver_sequence;
    p->handle=s->handle; p->sequence=s->sequence;
    p->x=x; p->y=y; p->width=w; p->height=h;
    return 1;
failed:
    p->failed=1;
    return -1;
}
