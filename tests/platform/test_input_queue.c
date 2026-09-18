#define _GNU_SOURCE
#include <platform/input.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static aos_input_frontend_t frontend;
static aos_input_client_region_t regions[AOS_INPUT_CLIENTS];
static aos_input_service_t service;
static void setup(void)
{
    memset(&frontend,0,sizeof(frontend));
    memset(regions,0,sizeof(regions));
    aos_input_client_region_t *clients[]={&regions[0],&regions[1]};
    assert(aos_input_service_init(&service,&frontend,clients,3)==0);
}
static aos_input_request_t key(uint32_t client)
{
    return (aos_input_request_t){.version=1,.id=23,.client=client,.device=AOS_INPUT_KEYBOARD,.count=3,
        .events={{1,30,1},{1,30,0},{0,0,0}}};
}
static uint32_t submit(aos_input_request_t q, uint32_t expected_status)
{
    uint32_t ready=99;
    assert(aos_input_submit(&frontend,&q)==0);
    assert(aos_input_pump(&service,&ready)==1);
    aos_input_response_t response;
    assert(aos_input_receive(&frontend,&response)==0);
    assert(response.version==1 && response.id==q.id && response.status==expected_status);
    assert(response.accepted==(expected_status==AOS_INPUT_OK ? q.count : 0));
    assert(ready==(expected_status==AOS_INPUT_OK ? 1u<<q.client : 0));
    return response.accepted;
}
static void expect_batch(aos_input_request_t q)
{
    aos_input_event_queue_t *queue=&regions[q.client].devices[q.device];
    aos_input_event_t event;
    for (unsigned i=0;i<q.count;++i) {
        assert(aos_input_event_receive(queue,&event)==0);
        assert(memcmp(&event,&q.events[i],sizeof(event))==0);
    }
    assert(aos_input_event_receive(queue,&event)==-1);
}
static void test_terminal_detach(void)
{
    setup();
    aos_input_client_region_t *retired=mmap(NULL,AOS_INPUT_FRAME_SIZE,
        PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert(retired!=MAP_FAILED);
    service.clients[0]=retired;
    aos_input_request_t press=key(0);
    press.count=2;
    press.events[1]=(aos_input_event_t){0,0,0};
    submit(press,AOS_INPUT_OK);
    retired->devices[0].tail=AOS_INPUT_EVENT_CAPACITY;
    aos_input_request_t release={.version=AOS_INPUT_RELEASE_VERSION,.client=0,
        .device=AOS_INPUT_KEYBOARD};
    assert(aos_input_submit(&frontend,&release)==0);
    uint32_t ready;
    assert(aos_input_pump(&service,&ready)==1 && !ready);
    aos_input_response_t response;
    assert(aos_input_receive(&frontend,&response)==0 && response.status==AOS_INPUT_OK);
    assert(service.releasing[0] && service.held[0][0][0]);
    /* A full frontend response ring must not prevent control completion. */
    frontend.resp_tail=frontend.resp_head+AOS_INPUT_REQUEST_CAPACITY;
    aos_input_request_t pending=key(0);
    assert(aos_input_submit(&frontend,&pending)==0);
    retired->detach.version=AOS_INPUT_DETACH_VERSION+1;
    retired->detach.request=1;
    assert(aos_input_pump(&service,&ready)==0 && service.allowed_mask==3 && !retired->detach.ack);
    retired->detach.version=AOS_INPUT_DETACH_VERSION;
    retired->detach.request=2;
    assert(aos_input_pump(&service,&ready)==0 && service.allowed_mask==3 && !retired->detach.ack);
    retired->detach.request=1;
    assert(aos_input_pump(&service,&ready)==1 && ready==1 && retired->detach.ack==1);
    assert(service.allowed_mask==2 && !service.clients[0] && !service.releasing[0]);
    for (unsigned d=0;d<AOS_INPUT_DEVICES;d++)
        for (unsigned w=0;w<8;w++) assert(!service.held[0][d][w]);
    assert(mprotect(retired,AOS_INPUT_FRAME_SIZE,PROT_NONE)==0);
    frontend.resp_head=frontend.resp_tail;
    aos_input_request_t peer=key(1);
    assert(aos_input_submit(&frontend,&peer)==0);
    assert(aos_input_pump(&service,&ready)==2 && ready==2);
    assert(aos_input_receive(&frontend,&response)==0 && response.status==AOS_INPUT_DENIED && !response.accepted);
    assert(aos_input_receive(&frontend,&response)==0 && response.status==AOS_INPUT_OK && response.accepted==3);
    expect_batch(peer);
    assert(aos_input_pump(&service,&ready)==0 && !ready);
    assert(munmap(retired,AOS_INPUT_FRAME_SIZE)==0);
}

int main(void)
{
    setup();
    aos_input_request_t keyboard=key(0);
    aos_input_request_t pointer={.version=1,.id=24,.client=1,.device=AOS_INPUT_POINTER,.count=5,
        .events={{2,0,-17},{2,1,29},{1,0x110,1},{2,8,-1},{0,0,0}}};
    submit(keyboard,AOS_INPUT_OK);
    submit(pointer,AOS_INPUT_OK);
    assert(regions[0].devices[AOS_INPUT_POINTER].tail==0);
    assert(regions[1].devices[AOS_INPUT_KEYBOARD].tail==0);
    expect_batch(pointer);
    expect_batch(keyboard);

    /* Reject whole frames, never publishing the valid prefix of an invalid one. */
    aos_input_request_t invalid=keyboard;
    invalid.events[1].value=3; submit(invalid,AOS_INPUT_BAD_REQUEST);
    invalid=keyboard; invalid.events[2].code=1; submit(invalid,AOS_INPUT_BAD_REQUEST);
    invalid=keyboard; invalid.count=65; submit(invalid,AOS_INPUT_BAD_REQUEST);
    invalid=keyboard; invalid.device=UINT32_MAX; submit(invalid,AOS_INPUT_BAD_REQUEST);
    invalid=keyboard; invalid.reserved[2]=1; submit(invalid,AOS_INPUT_BAD_REQUEST);
    invalid=keyboard; invalid.version=2; submit(invalid,AOS_INPUT_BAD_REQUEST);
    invalid=keyboard; invalid.events[0].type=0; submit(invalid,AOS_INPUT_BAD_REQUEST);
    invalid=pointer; invalid.events[0].code=7; submit(invalid,AOS_INPUT_BAD_REQUEST);
    invalid=pointer; invalid.events[2].code=30; submit(invalid,AOS_INPUT_BAD_REQUEST);
    invalid=keyboard; invalid.client=UINT32_MAX; submit(invalid,AOS_INPUT_DENIED);
    service.allowed_mask=1;
    submit(pointer,AOS_INPUT_DENIED);
    assert(regions[0].devices[0].head==regions[0].devices[0].tail);
    assert(regions[1].devices[1].head==regions[1].devices[1].tail);

    /* A nearly full destination refuses the entire batch and recovers on drain. */
    setup();
    aos_input_event_queue_t *queue=&regions[0].devices[0];
    for (unsigned i=0;i<85;++i) submit(keyboard,AOS_INPUT_OK);
    uint32_t tail=queue->tail;
    submit(keyboard,AOS_INPUT_WOULD_BLOCK);
    assert(queue->tail==tail);
    aos_input_event_t event;
    for (unsigned i=0;i<3;++i) assert(aos_input_event_receive(queue,&event)==0);
    submit(keyboard,AOS_INPUT_OK);
    for (unsigned i=0;i<85*3;++i) {
        assert(aos_input_event_receive(queue,&event)==0);
        assert(memcmp(&event,&keyboard.events[i%3],sizeof(event))==0);
    }
    assert(aos_input_event_receive(queue,&event)==-1);

    /* No side effects when acknowledgments cannot be published. */
    setup();
    for (unsigned i=0;i<AOS_INPUT_REQUEST_CAPACITY;++i) {
        assert(aos_input_submit(&frontend,&keyboard)==0);
        assert(aos_input_pump(&service,NULL)==1);
    }
    tail=regions[0].devices[0].tail;
    assert(aos_input_submit(&frontend,&keyboard)==0);
    assert(aos_input_pump(&service,NULL)==0 && regions[0].devices[0].tail==tail);
    aos_input_response_t response;
    assert(aos_input_receive(&frontend,&response)==0);
    assert(aos_input_pump(&service,NULL)==1 && regions[0].devices[0].tail==tail+3);

    setup();
    frontend.req_head=frontend.req_tail=UINT32_MAX;
    frontend.resp_head=frontend.resp_tail=UINT32_MAX;
    regions[0].devices[0].head=regions[0].devices[0].tail=UINT32_MAX-1;
    submit(keyboard,AOS_INPUT_OK);
    expect_batch(keyboard);
    assert(frontend.req_tail==0 && frontend.resp_tail==0 && regions[0].devices[0].tail==1);
    frontend.req_tail=frontend.req_head+AOS_INPUT_REQUEST_CAPACITY+1;
    assert(aos_input_pump(&service,NULL)==0 && aos_input_submit(&frontend,&keyboard)==-1);
    frontend.req_tail=frontend.req_head;
    regions[0].devices[0].tail=regions[0].devices[0].head+AOS_INPUT_EVENT_CAPACITY+1;
    submit(keyboard,AOS_INPUT_WOULD_BLOCK);
    assert(aos_input_event_receive(&regions[0].devices[0],&event)==-1);
    /* Release acceptance is asynchronous: preserve queued presses and retain
     * the release privately until the guest makes room. */
    setup();
    keyboard.count=2; keyboard.events[1]=(aos_input_event_t){0,0,0};
    for (unsigned i=0;i<128;i++) submit(keyboard,AOS_INPUT_OK);
    aos_input_request_t release={.version=AOS_INPUT_RELEASE_VERSION,.id=99,.client=0,.device=0};
    uint32_t ready=99;
    assert(aos_input_submit(&frontend,&release)==0);
    assert(aos_input_pump(&service,&ready)==1 && ready==0);
    assert(aos_input_receive(&frontend,&response)==0);
    assert(response.version==AOS_INPUT_RELEASE_VERSION && response.id==99 &&
        response.status==AOS_INPUT_OK && response.accepted==0);
    assert(service.releasing[0]==1 && regions[0].devices[0].tail==256);
    submit(keyboard,AOS_INPUT_WOULD_BLOCK);
    for (unsigned i=0;i<2;i++) assert(aos_input_event_receive(&regions[0].devices[0],&event)==0);
    assert(aos_input_pump(&service,&ready)>0 && ready==1 && service.releasing[0]==0);
    for (unsigned i=0;i<254;i++) assert(aos_input_event_receive(&regions[0].devices[0],&event)==0);
    assert(aos_input_event_receive(&regions[0].devices[0],&event)==0 &&
        event.type==1 && event.code==30 && event.value==0);
    assert(aos_input_event_receive(&regions[0].devices[0],&event)==0 && event.type==0);
    assert(aos_input_pump(&service,&ready)==0 && ready==0);
    assert(regions[1].devices[0].tail==0 && regions[0].devices[1].tail==0);
    submit(keyboard,AOS_INPUT_OK);

    /* Pointer cleanup is isolated; repeats alone do not invent held keys. */
    setup(); submit(pointer,AOS_INPUT_OK); expect_batch(pointer);
    release.client=1; release.device=AOS_INPUT_POINTER;
    assert(aos_input_submit(&frontend,&release)==0);
    assert(aos_input_pump(&service,&ready)>0 && ready==2);
    assert(aos_input_receive(&frontend,&response)==0 && response.status==AOS_INPUT_OK);
    assert(aos_input_event_receive(&regions[1].devices[1],&event)==0 &&
        event.type==1 && event.code==0x110 && event.value==0);
    assert(aos_input_event_receive(&regions[1].devices[1],&event)==0 && event.type==0);
    keyboard.events[0].value=2; submit(keyboard,AOS_INPUT_OK); expect_batch(keyboard);
    release.client=0; release.device=AOS_INPUT_KEYBOARD;
    assert(aos_input_submit(&frontend,&release)==0);
    assert(aos_input_pump(&service,&ready)>0 && ready==0);
    assert(aos_input_receive(&frontend,&response)==0 && response.status==AOS_INPUT_OK);
    assert(aos_input_event_receive(&regions[0].devices[0],&event)==-1);
    setup();
    for (unsigned start=1;start<=255;start+=63) {
        keyboard=key(0); keyboard.count=0;
        for (unsigned code=start;code<=255 && code<start+63;code++)
            keyboard.events[keyboard.count++]=(aos_input_event_t){1,(uint16_t)code,1};
        keyboard.events[keyboard.count++]=(aos_input_event_t){0,0,0};
        submit(keyboard,AOS_INPUT_OK); expect_batch(keyboard);
    }
    assert(aos_input_submit(&frontend,&release)==0);
    assert(aos_input_pump(&service,&ready)>0);
    assert(aos_input_receive(&frontend,&response)==0 && response.status==AOS_INPUT_OK);
    while (aos_input_pump(&service,&ready)) {}
    assert(service.releasing[0]==1);
    unsigned released=0, packets=0;
    while (aos_input_event_receive(&regions[0].devices[0],&event)==0) {
        if (event.type==1) { assert(event.code==++released && event.value==0); }
        else { assert(event.type==0 && event.code==0 && event.value==0); packets++; }
    }
    assert(released==252 && packets==4);
    assert(aos_input_pump(&service,&ready)>0 && ready==1 && !service.releasing[0]);
    while (aos_input_event_receive(&regions[0].devices[0],&event)==0) {
        if (event.type==1) { assert(event.code==++released && event.value==0); }
        else packets++;
    }
    assert(released==255 && packets==5 && aos_input_pump(&service,&ready)==0);
    test_terminal_detach();
    puts("PASS: input isolation, release, backpressure, wrap and terminal detach with protected retired memory");
    return 0;
}
