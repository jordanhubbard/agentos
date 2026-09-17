#include <platform/input.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

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
    puts("PASS: isolated keyboard/pointer batches, exact events, validation, atomic backpressure and wrapping queues");
    return 0;
}
