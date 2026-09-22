/* SPDX-License-Identifier: BSD-2-Clause */
#include <platform/input_rebind.h>
#include "contracts/virtualizer_authority.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    input_virt_rebind_reply_t reply={AOS_INPUT_OK,INPUT_VIRT_REBIND_VERSION,1};
    assert(aos_input_rebind_reply_valid(&reply,sizeof(reply),1));
    assert(!aos_input_rebind_reply_valid(NULL,sizeof(reply),1));
    assert(!aos_input_rebind_reply_valid(&reply,sizeof(reply)-1,1));
    assert(!aos_input_rebind_reply_valid(&reply,sizeof(reply),0));
    assert(!aos_input_rebind_reply_valid(&reply,sizeof(reply),2));
    reply.status=AOS_INPUT_DENIED;
    assert(!aos_input_rebind_reply_valid(&reply,sizeof(reply),1));
    reply.status=AOS_INPUT_OK;
    reply.version++;
    assert(!aos_input_rebind_reply_valid(&reply,sizeof(reply),1));
    aos_input_frontend_t frontend={0};
    aos_input_client_region_t old={0}, peer={0}, fresh={0};
    aos_input_client_region_t *clients[]={&old,&peer};
    aos_input_service_t service;
    assert(!aos_input_service_init(&service,&frontend,clients,3));
    input_virt_rebind_req_t request={INPUT_VIRT_REBIND_VERSION,0,1};
    uint64_t owner=virt_client_badge(0);
    assert(aos_input_rebind_validate(&service,owner,&request,sizeof(request))==AOS_INPUT_WOULD_BLOCK);
    old.detach=(aos_input_detach_t){AOS_INPUT_DETACH_VERSION,1,0};
    aos_input_request_t press={.version=1,.client=0,.count=2,
        .events={{1,88,1},{0,0,0}}};
    assert(!aos_input_submit(&frontend,&press));
    frontend.resp_tail=AOS_INPUT_REQUEST_CAPACITY;
    uint32_t ready;
    assert(aos_input_pump(&service,&ready)==1 && ready==1 && old.detach.ack==1);
    assert(aos_input_rebind_validate(&service,owner,&request,sizeof(request))==AOS_INPUT_WOULD_BLOCK);
    frontend.resp_head=frontend.resp_tail;
    assert(aos_input_pump(&service,&ready)==1 && ready==0);
    aos_input_response_t response;
    assert(!aos_input_receive(&frontend,&response) && response.status==AOS_INPUT_DENIED);
    assert(aos_input_rebind_validate(&service,virt_client_badge(1),&request,sizeof(request))==AOS_INPUT_DENIED);
    assert(aos_input_rebind_validate(&service,owner,&request,sizeof(request)-1)==AOS_INPUT_BAD_REQUEST);
    aos_input_service_t before=service;
    fresh.devices[0].tail=1;
    assert(aos_input_rebind_commit(&service,owner,&request,sizeof(request),&fresh)==AOS_INPUT_BAD_REQUEST);
    assert(!memcmp(&before,&service,sizeof(service)));
    fresh.devices[0].tail=0;
    assert(aos_input_rebind_commit(&service,owner,&request,sizeof(request),&peer)==AOS_INPUT_DENIED);
    assert(aos_input_rebind_commit(&service,owner,&request,sizeof(request),&fresh)==AOS_INPUT_OK);
    assert(service.clients[0]==&fresh && service.clients[1]==&peer && service.generation[0]==1);
    assert(!fresh.devices[0].tail); /* Old queued press never reaches new guest. */
    assert(!aos_input_submit(&frontend,&press));
    assert(aos_input_pump(&service,&ready)==1 && ready==1);
    assert(fresh.devices[0].tail==2 && !peer.devices[0].tail);
    assert(old.detach.ack==1 && !old.devices[0].tail);
    fresh.detach=(aos_input_detach_t){AOS_INPUT_DETACH_VERSION,1,0};
    assert(aos_input_pump(&service,&ready)==1);
    assert(aos_input_rebind_validate(&service,owner,&request,sizeof(request))==AOS_INPUT_WOULD_BLOCK);
    request.generation=2;
    assert(aos_input_rebind_validate(&service,owner,&request,sizeof(request))==AOS_INPUT_OK);
    /* Failed mapping cleanup needs no access through the VMM's old frame. */
    assert(aos_input_rebind_retire(&service,owner,&request,sizeof(request))==AOS_INPUT_OK);
    aos_input_client_region_t second={0};
    assert(aos_input_rebind_commit(&service,owner,&request,sizeof(request),&second)==AOS_INPUT_OK);
    before=service;
    assert(aos_input_rebind_retire(&service,virt_client_badge(1),&request,sizeof(request))==AOS_INPUT_DENIED);
    request.generation=1;
    assert(aos_input_rebind_retire(&service,owner,&request,sizeof(request))==AOS_INPUT_DENIED);
    assert(!memcmp(&before,&service,sizeof(service)));
    request.generation=2;
    assert(aos_input_rebind_retire(&service,owner,&request,sizeof(request)-1)==AOS_INPUT_BAD_REQUEST);
    service.releasing[0]=3;
    service.held[0][0][0]=1;
    assert(aos_input_rebind_retire(&service,owner,&request,sizeof(request))==AOS_INPUT_OK);
    assert(!service.clients[0] && second.detach.ack && !service.releasing[0] && !service.held[0][0][0]);
    assert(service.clients[1]==&peer);
    assert(aos_input_rebind_retire(&service,owner,&request,sizeof(request))==AOS_INPUT_OK);
    service.generation[0]=UINT32_MAX;
    request.generation=0;
    assert(aos_input_rebind_validate(&service,owner,&request,sizeof(request))==AOS_INPUT_WOULD_BLOCK);
    puts("PASS: input rebind fences old requests, preserves peer, rejects stale generation and dirty frames");
    return 0;
}
