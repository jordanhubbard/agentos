#include <platform/display.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static aos_display_region_t region;
static uint8_t banks[2][AOS_DISPLAY_FRAME_BYTES];
static unsigned calls, fail;
static uint8_t pixel(size_t offset) { return (uint8_t)(offset*13u+7u); }
static int present(void *context,unsigned bank,uint32_t width,uint32_t height)
{
    assert(context==&region);
    ++calls;
    if (fail) return -1;
    assert(bank==1 && width==1024 && height==768);
    for (size_t i=0;i<AOS_DISPLAY_FRAME_BYTES;++i) assert(banks[bank][i]==pixel(i));
    return 0;
}
static aos_display_response_t transact(aos_display_driver_t *d,aos_display_request_t q)
{
    static uint32_t id;
    q.version=1; q.id=++id;
    assert(aos_display_submit(&region,&q)==0);
    assert(aos_display_submit(&region,&q)==-1);
    assert(aos_display_pump(d)==1);
    assert(aos_display_pump(d)==0);
    assert(aos_display_submit(&region,&q)==-1); /* unread reply owns payload */
    aos_display_response_t p;
    assert(aos_display_receive(&region,&p)==0);
    assert(p.id==q.id && p.version==1 && !p.reserved);
    assert(aos_display_receive(&region,&p)==-1);
    return p;
}
int main(void)
{
    aos_display_driver_t d;
    assert(aos_display_init(&d,&region,banks[0],banks[0]+4,
        AOS_DISPLAY_FRAME_BYTES,present,&region)==-1);
    assert(aos_display_init(&d,&region,banks[0],banks[1],
        AOS_DISPLAY_FRAME_BYTES,present,&region)==0);
    /* Exercise counter wrap as well as full-frame payload bounds. */
    region.req_head=region.req_tail=region.resp_head=region.resp_tail=UINT32_MAX;
    aos_display_request_t q={.operation=AOS_DISPLAY_BEGIN,.width=1024,.height=768};
    aos_display_response_t p=transact(&d,q);
    assert(p.status==AOS_DISPLAY_OK && p.cookie && !p.sequence);
    uint64_t cookie=p.cookie;
    assert(transact(&d,q).status==AOS_DISPLAY_BUSY);
    q=(aos_display_request_t){.operation=AOS_DISPLAY_PRESENT,.cookie=cookie};
    assert(transact(&d,q).status==AOS_DISPLAY_INCOMPLETE && !calls);
    q.operation=AOS_DISPLAY_WRITE; q.offset=1; q.length=4;
    assert(transact(&d,q).status==AOS_DISPLAY_INVALID && d.written==0);
    q.offset=0; q.length=AOS_DISPLAY_CHUNK+1;
    assert(transact(&d,q).status==AOS_DISPLAY_INVALID && d.written==0);
    for (uint32_t offset=0;offset<AOS_DISPLAY_FRAME_BYTES;offset+=AOS_DISPLAY_CHUNK) {
        for (uint32_t i=0;i<AOS_DISPLAY_CHUNK;++i) region.data[i]=pixel(offset+i);
        q.offset=offset; q.length=AOS_DISPLAY_CHUNK;
        assert(transact(&d,q).status==AOS_DISPLAY_OK);
    }
    for (size_t i=0;i<sizeof banks[0];++i) assert(banks[0][i]==0);
    q=(aos_display_request_t){.operation=AOS_DISPLAY_PRESENT,.cookie=cookie};
    p=transact(&d,q);
    assert(p.status==AOS_DISPLAY_OK && p.sequence==1 && calls==1 && d.front==1);
    assert(transact(&d,q).status==AOS_DISPLAY_STALE && calls==1);
    q=(aos_display_request_t){.operation=AOS_DISPLAY_BEGIN,.width=16,.height=16};
    p=transact(&d,q); assert(p.status==AOS_DISPLAY_OK && p.cookie>cookie);
    q=(aos_display_request_t){.operation=AOS_DISPLAY_ABORT,.cookie=p.cookie};
    assert(transact(&d,q).status==AOS_DISPLAY_OK && d.front==1);
    assert(transact(&d,q).status==AOS_DISPLAY_STALE);
    q=(aos_display_request_t){.operation=AOS_DISPLAY_BEGIN,.width=16,.height=16};
    p=transact(&d,q); assert(p.status==AOS_DISPLAY_OK);
    q=(aos_display_request_t){.operation=AOS_DISPLAY_WRITE,.cookie=p.cookie,.length=1024};
    memset(region.data,0,1024);
    assert(transact(&d,q).status==AOS_DISPLAY_OK);
    q.operation=AOS_DISPLAY_PRESENT; q.length=0; fail=1;
    assert(transact(&d,q).status==AOS_DISPLAY_FAILED && calls==2 && d.front==1);
    assert(transact(&d,q).status==AOS_DISPLAY_FAILED && calls==2 && d.sequence==1);
    for (size_t i=0;i<sizeof banks[1];++i) assert(banks[1][i]==pixel(i));
    ++region.req_tail; ++region.req_tail;
    assert(aos_display_pump(&d)==0);
    assert(aos_display_submit(&region,&q)==-1);
    puts("display queue: full-frame pixels, stable front bank, backpressure and failed flip passed");
    return 0;
}
