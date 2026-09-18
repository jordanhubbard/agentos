#include <platform/framebuffer_observer.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static aos_fb_response_t client_call(aos_fb_client_t *c,aos_fb_request_t q)
{
    q.version=AOS_FB_VERSION;
    aos_fb_response_t p;
    assert(aos_fb_submit(c->region,&q)==0 && aos_fb_pump(c)==1);
    assert(aos_fb_receive(c->region,&p)==0 && p.status==AOS_FB_OK);
    return p;
}
static aos_fb_observer_response_t call(aos_fb_observer_t *o,aos_fb_observer_request_t q)
{
    q.version=AOS_FB_OBSERVER_VERSION;
    aos_fb_observer_response_t p;
    assert(aos_fb_observer_submit(o->region,&q)==0 && aos_fb_observer_pump(o)==1);
    assert(aos_fb_observer_receive(o->region,&p)==0 && p.id==q.id);
    return p;
}
int main(void)
{
    aos_fb_client_t clients[AOS_FB_CLIENTS];
    void *arenas[AOS_FB_CLIENTS];
    for (unsigned i=0;i<AOS_FB_CLIENTS;++i) {
        aos_fb_region_t *r=calloc(1,sizeof(*r));
        arenas[i]=malloc(AOS_FB_ARENA_BYTES);
        assert(r && arenas[i] && aos_fb_client_init(&clients[i],r,arenas[i],AOS_FB_ARENA_BYTES)==0);
    }
    aos_fb_observer_region_t *r=calloc(1,sizeof(*r));
    void *snapshot=malloc(AOS_FB_SURFACE_BYTES);
    aos_fb_observer_t observer;
    assert(r && snapshot && aos_fb_observer_init(&observer,r,clients,AOS_FB_CLIENTS,1,snapshot,AOS_FB_SURFACE_BYTES)==0);
    aos_fb_observer_request_t capture={.operation=AOS_FB_CAPTURE,.client=0};
    assert(call(&observer,capture).status==AOS_FB_OBSERVER_NO_FRAME);
    aos_fb_client_t *c=&clients[0];
    uint64_t handle=client_call(c,(aos_fb_request_t){.operation=AOS_FB_CREATE,.width=8,.height=6}).handle;
    client_call(c,(aos_fb_request_t){.operation=AOS_FB_SELECT,.handle=handle,.x=2,.y=1,.width=3,.height=2});
    assert(call(&observer,capture).status==AOS_FB_OBSERVER_NO_FRAME);
    for (unsigned i=0;i<8*6*4;++i) c->region->data[i]=(uint8_t)(i+1);
    client_call(c,(aos_fb_request_t){.operation=AOS_FB_WRITE,.handle=handle,.width=8,.height=6,.data_length=8*6*4});
    client_call(c,(aos_fb_request_t){.operation=AOS_FB_FLIP,.handle=handle});
    aos_fb_observer_response_t p=call(&observer,capture);
    assert(p.status==AOS_FB_OBSERVER_OK && p.cookie && p.sequence==1 && p.width==3 && p.height==2);
    uint64_t cookie=p.cookie;
    uint8_t expected[24];
    for (unsigned row=0;row<2;++row)
        for (unsigned i=0;i<12;++i) expected[row*12+i]=(uint8_t)(((row+1)*8+2)*4+i+1);

    memset(c->region->data,0xff,8*6*4);
    client_call(c,(aos_fb_request_t){.operation=AOS_FB_WRITE,.handle=handle,.width=8,.height=6,.data_length=8*6*4});
    client_call(c,(aos_fb_request_t){.operation=AOS_FB_FLIP,.handle=handle});
    aos_fb_observer_request_t read={.operation=AOS_FB_CAPTURE_READ,.cookie=cookie,.length=24};
    p=call(&observer,read);
    assert(p.status==AOS_FB_OBSERVER_OK && p.sequence==1 && p.length==24 && !memcmp(r->data,expected,24));
    capture.client=1;
    assert(call(&observer,capture).status==AOS_FB_OBSERVER_DENIED);
    capture.client=UINT32_MAX;
    assert(call(&observer,capture).status==AOS_FB_OBSERVER_DENIED);
    assert(call(&observer,read).status==AOS_FB_OBSERVER_OK); /* rejected capture preserves snapshot */

    read.version=AOS_FB_OBSERVER_VERSION; read.length=7;
    assert(aos_fb_observer_submit(r,&read)==0 && aos_fb_observer_pump(&observer)==1);
    read.offset=7;
    assert(aos_fb_observer_submit(r,&read)==0 && aos_fb_observer_pump(&observer)==0);
    assert(!memcmp(r->data,expected,7)); /* bytes stay owned until response drained */
    assert(aos_fb_observer_receive(r,&p)==0 && p.length==7);
    assert(aos_fb_observer_pump(&observer)==1 && aos_fb_observer_receive(r,&p)==0);
    assert(p.length==7 && !memcmp(r->data,expected+7,7));
    read.offset=UINT32_MAX;
    assert(call(&observer,read).status==AOS_FB_OBSERVER_BAD_BOUNDS);
    read.offset=0; read.length=AOS_FB_DATA_BYTES+1;
    assert(call(&observer,read).status==AOS_FB_OBSERVER_BAD_BOUNDS);
    read.length=24;
    capture.client=0;
    uint64_t fresh=call(&observer,capture).cookie;
    assert(fresh!=cookie && call(&observer,read).status==AOS_FB_OBSERVER_BAD_COOKIE);
    client_call(c,(aos_fb_request_t){.operation=AOS_FB_DESTROY,.handle=handle});
    assert(call(&observer,capture).status==AOS_FB_OBSERVER_NO_FRAME);
    read.cookie=fresh;
    assert(call(&observer,read).status==AOS_FB_OBSERVER_OK);
    for (unsigned i=0;i<24;++i) assert(r->data[i]==0xff);
    assert(call(&observer,(aos_fb_observer_request_t){.operation=AOS_FB_CAPTURE_RELEASE,.cookie=fresh}).status==AOS_FB_OBSERVER_OK);
    assert(call(&observer,read).status==AOS_FB_OBSERVER_BAD_COOKIE);

    r->req_head=r->req_tail=r->resp_head=r->resp_tail=UINT32_MAX;
    assert(call(&observer,capture).status==AOS_FB_OBSERVER_NO_FRAME && !r->req_head && !r->resp_tail);
    r->req_tail=AOS_FB_QUEUE_CAPACITY+1;
    assert(aos_fb_observer_pump(&observer)==0);
    r->req_tail=r->req_head;
    handle=client_call(c,(aos_fb_request_t){.operation=AOS_FB_CREATE,
        .width=AOS_FB_MAX_WIDTH,.height=AOS_FB_MAX_HEIGHT}).handle;
    client_call(c,(aos_fb_request_t){.operation=AOS_FB_SELECT,.handle=handle,
        .width=AOS_FB_MAX_WIDTH,.height=AOS_FB_MAX_HEIGHT});
    for (unsigned row=0;row<AOS_FB_MAX_HEIGHT;row+=16) {
        unsigned base=row*AOS_FB_MAX_WIDTH*4;
        for (unsigned i=0;i<AOS_FB_DATA_BYTES;++i) c->region->data[i]=(uint8_t)((base+i)*17u+3u);
        client_call(c,(aos_fb_request_t){.operation=AOS_FB_WRITE,.handle=handle,
            .y=row,.width=AOS_FB_MAX_WIDTH,.height=16,.data_length=AOS_FB_DATA_BYTES});
    }
    client_call(c,(aos_fb_request_t){.operation=AOS_FB_FLIP,.handle=handle});
    observer.next_cookie=UINT64_MAX;
    p=call(&observer,capture);
    assert(p.cookie==UINT64_MAX && p.width==AOS_FB_MAX_WIDTH && p.height==AOS_FB_MAX_HEIGHT);
    read.cookie=p.cookie; read.length=AOS_FB_DATA_BYTES;
    for (unsigned offset=0;offset<AOS_FB_SURFACE_BYTES;offset+=AOS_FB_DATA_BYTES) {
        read.offset=offset;
        assert(call(&observer,read).status==AOS_FB_OBSERVER_OK);
        for (unsigned i=0;i<AOS_FB_DATA_BYTES;++i) assert(r->data[i]==(uint8_t)((offset+i)*17u+3u));
    }
    assert(call(&observer,capture).status==AOS_FB_OBSERVER_EXHAUSTED);
    aos_fb_region_t *retired = c->region;
    retired->detach.version = AOS_FB_DETACH_VERSION;
    __atomic_store_n(&retired->detach.request, 1u, __ATOMIC_RELEASE);
    assert(aos_fb_pump(c) == 1 && retired->detach.ack == 1);
    assert(!c->region && !c->selected_handle);
    free(retired);
    free(arenas[0]);
    arenas[0] = NULL;
    assert(aos_fb_pump(c) == 0);
    assert(call(&observer,capture).status == AOS_FB_OBSERVER_NO_FRAME);
    /* A completed snapshot is an independent service-owned copy and remains
     * readable after the source queue and arena have been released. */
    read.offset = 0;
    assert(call(&observer,read).status == AOS_FB_OBSERVER_OK);
    for (unsigned i = 0; i < AOS_FB_DATA_BYTES; i++)
        assert(r->data[i] == (uint8_t)(i * 17u + 3u));
    for (unsigned i=0;i<AOS_FB_CLIENTS;++i) { free(clients[i].region); free(arenas[i]); }
    free(r);free(snapshot);
    puts("PASS: bounded immutable frame snapshots, selection, authorization, chunks, stale cookies and response ownership");
    return 0;
}
