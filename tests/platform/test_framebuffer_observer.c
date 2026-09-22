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

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}

/* Independent decoder checks the wire contract, not encoder internals. */
static uint32_t decode_packed(const uint8_t *wire,uint32_t bytes,uint8_t *out,uint32_t limit)
{
    assert(bytes>=12 && bytes<=4056);
    uint32_t decoded=le32(wire), encoding=le32(wire+4), count=0;
    assert(decoded && !(decoded&3u) && decoded<=limit);
    if (encoding==0) {
        assert(bytes==decoded+8);
        memcpy(out,wire+8,decoded);
    } else {
        assert(encoding==1 && !((bytes-8)&7u));
        for (uint32_t pos=8;pos<bytes;pos+=8) {
            uint32_t pixels=le32(wire+pos);
            assert(pixels && pixels<=(decoded-count)/4);
            for (uint32_t i=0;i<pixels;++i) {
                memcpy(out+count,wire+pos+4,4);
                count+=4;
            }
        }
        assert(count==decoded);
    }
    return decoded;
}

static void packed_cases(aos_fb_observer_t *o,aos_fb_client_t *c,uint64_t handle)
{
    uint8_t expected[65536], decoded[65536];
    static const uint32_t lengths[]={4,8,12,4044,4048,4052,4056,65532,65536};
    for (unsigned pattern=0;pattern<4;++pattern) {
        for (uint32_t pixel=0;pixel<16384;++pixel) {
            uint32_t value=pattern==0 ? 0x12345678u : pattern==1 ? pixel :
                pattern==2 ? pixel/1024u : (pixel<510 ? pixel : 0xaabbccddu);
            for (unsigned byte=0;byte<4;++byte)
                expected[pixel*4+byte]=(uint8_t)(value>>(byte*8));
        }
        memcpy(c->region->data,expected,sizeof(expected));
        client_call(c,(aos_fb_request_t){.operation=AOS_FB_WRITE,.handle=handle,
            .width=1024,.height=16,.data_length=sizeof(expected)});
        client_call(c,(aos_fb_request_t){.operation=AOS_FB_FLIP,.handle=handle});
        aos_fb_observer_response_t frame=call(o,(aos_fb_observer_request_t){.operation=AOS_FB_CAPTURE});
        assert(frame.status==AOS_FB_OBSERVER_OK && frame.cookie);
        /* Mutating the source after CAPTURE must not change packed pixels. */
        memset(c->region->data,0xee,sizeof(expected));
        client_call(c,(aos_fb_request_t){.operation=AOS_FB_WRITE,.handle=handle,
            .width=1024,.height=16,.data_length=sizeof(expected)});
        client_call(c,(aos_fb_request_t){.operation=AOS_FB_FLIP,.handle=handle});
        aos_fb_observer_request_t q={.operation=AOS_FB_CAPTURE_READ_PACKED,.cookie=frame.cookie};
        for (unsigned i=0;i<sizeof(lengths)/sizeof(lengths[0]);++i) {
            q.offset=65536-lengths[i]; q.length=lengths[i];
            memset(o->region->data,0xa5,AOS_FB_DATA_BYTES);
            aos_fb_observer_response_t p=call(o,q);
            assert(p.status==AOS_FB_OBSERVER_OK && p.cookie==frame.cookie && p.sequence==frame.sequence);
            uint32_t n=decode_packed(o->region->data,p.length,decoded,q.length);
            assert(!memcmp(decoded,expected+q.offset,n));
            for (unsigned j=4056;j<AOS_FB_DATA_BYTES;++j) assert(o->region->data[j]==0xa5);
        }
        /* Repeated prefix replies must reconstruct the whole range exactly. */
        for (uint32_t offset=0;offset<65536;) {
            q.offset=offset; q.length=65536-offset;
            aos_fb_observer_response_t p=call(o,q);
            assert(p.status==AOS_FB_OBSERVER_OK);
            uint32_t n=decode_packed(o->region->data,p.length,decoded,q.length);
            assert(!memcmp(decoded,expected+offset,n));
            if (pattern==0) assert(n==65536 && p.length==16);
            if (pattern==1) assert(le32(o->region->data+4)==AOS_FB_PACKED_RAW);
            offset+=n;
        }
        q.offset=0;
        q.length=0; assert(call(o,q).status==AOS_FB_OBSERVER_BAD_BOUNDS);
        q.length=65540; assert(call(o,q).status==AOS_FB_OBSERVER_BAD_BOUNDS);
        q.length=3; assert(call(o,q).status==AOS_FB_OBSERVER_BAD_BOUNDS);
        q.length=4; q.offset=1; assert(call(o,q).status==AOS_FB_OBSERVER_BAD_BOUNDS);
        q.offset=AOS_FB_SURFACE_BYTES; assert(call(o,q).status==AOS_FB_OBSERVER_BAD_BOUNDS);
        q.offset=UINT32_MAX-3u; assert(call(o,q).status==AOS_FB_OBSERVER_BAD_BOUNDS);
        q.offset=0; q.cookie=0; assert(call(o,q).status==AOS_FB_OBSERVER_BAD_COOKIE);
    }
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
    packed_cases(&observer,c,handle);
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
