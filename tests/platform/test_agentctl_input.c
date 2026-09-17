/* Real CLI wire exchange with the canonical queue service. */
#define main agentctl_main
#include "../../tools/agentctl/agentctl.c"
#undef main
#include <assert.h>
#include <sys/wait.h>
static void server(int fd,unsigned mode)
{
    cc_req_wire_t req;
    assert(read_full(fd,&req,sizeof(req)));
    assert(req.opcode==MSG_CC_INPUT_SUBMIT && req.mr[0]==7 && !req.mr[1] && !req.mr[2]);
    aos_input_request_t query;
    memcpy(&query,req.shmem,sizeof(query));
    assert(query.version==1 && !query.id && !query.client && query.device==1 && query.count==3);
    assert(query.events[0].type==2 && query.events[0].code==0 && query.events[0].value==-17);
    assert(query.events[1].type==1 && query.events[1].code==0x110 && query.events[1].value==1);
    assert(!query.events[2].type && !query.events[2].code && !query.events[2].value);
    aos_input_frontend_t frontend={0};
    aos_input_client_region_t clients[2]={0};
    aos_input_client_region_t *pages[]={&clients[0],&clients[1]};
    aos_input_service_t service;
    assert(aos_input_service_init(&service,&frontend,pages,3)==0);
    if (mode==1) clients[0].devices[1].tail=AOS_INPUT_EVENT_CAPACITY;
    assert(aos_input_submit(&frontend,&query)==0 && aos_input_pump(&service,NULL)==1);
    aos_input_response_t response;
    assert(aos_input_receive(&frontend,&response)==0);
    if (!mode) {
        for (unsigned i=0;i<3;++i) {
            aos_input_event_t event;
            assert(aos_input_event_receive(&clients[0].devices[1],&event)==0);
            assert(!memcmp(&event,&query.events[i],sizeof(event)));
        }
        assert(!clients[1].devices[1].tail);
    }
    cc_reply_wire_t reply={.mr={CC_OK,sizeof(response),response.status,1}};
    if (mode==2) ++response.accepted;
    if (mode==3) response.version=2;
    if (mode==4) response.id=9;
    if (mode==5) reply.mr[1]=4096;
    memcpy(reply.shmem,&response,sizeof(response));
    assert(write_full(fd,&reply,sizeof(reply)));
    close(fd);
}
static void release_server(int fd,unsigned mode)
{
    cc_req_wire_t req;
    assert(read_full(fd,&req,sizeof(req)));
    assert(req.opcode==MSG_CC_INPUT_SUBMIT && req.mr[0]==7 && !req.mr[1] && !req.mr[2]);
    aos_input_request_t query;
    memcpy(&query,req.shmem,sizeof(query));
    aos_input_request_t expected={.version=AOS_INPUT_RELEASE_VERSION,.device=AOS_INPUT_POINTER};
    assert(!memcmp(&query,&expected,sizeof(query)));
    aos_input_frontend_t frontend={0};
    aos_input_client_region_t clients[2]={0};
    aos_input_client_region_t *pages[]={&clients[0],&clients[1]};
    aos_input_service_t service;
    assert(aos_input_service_init(&service,&frontend,pages,3)==0);
    aos_input_request_t press={.version=AOS_INPUT_VERSION,.device=AOS_INPUT_POINTER,.count=2,
        .events={{1,0x110,1},{0,0,0}}};
    aos_input_response_t response;
    aos_input_event_t event;
    assert(!aos_input_submit(&frontend,&press));
    assert(aos_input_pump(&service,NULL)==1);
    assert(!aos_input_receive(&frontend,&response) && response.accepted==2);
    for (unsigned i=0;i<2;++i) assert(!aos_input_event_receive(&clients[0].devices[1],&event));
    assert(!aos_input_submit(&frontend,&query));
    assert(aos_input_pump(&service,NULL)>0);
    assert(!aos_input_receive(&frontend,&response));
    assert(response.version==AOS_INPUT_RELEASE_VERSION && response.status==AOS_INPUT_OK && !response.accepted);
    assert(!aos_input_event_receive(&clients[0].devices[1],&event));
    assert(event.type==1 && event.code==0x110 && event.value==0);
    assert(!aos_input_event_receive(&clients[0].devices[1],&event));
    assert(!event.type && !event.code && !event.value);
    assert(aos_input_event_receive(&clients[0].devices[1],&event)!=0);
    assert(!clients[1].devices[1].tail);
    cc_reply_wire_t reply={.mr={CC_OK,sizeof(response),response.status,AOS_INPUT_RELEASE_VERSION}};
    if (mode==1) response.version=AOS_INPUT_VERSION;
    if (mode==2) response.accepted=1;
    memcpy(reply.shmem,&response,sizeof(response));
    assert(write_full(fd,&reply,sizeof(reply)));
    close(fd);
}
int main(void)
{
    char *args[]={"7","pointer","2","0","-17","1","272","1"};
    for (unsigned mode=0;mode<6;++mode) {
        int fds[2]; assert(socketpair(AF_UNIX,SOCK_STREAM,0,fds)==0);
        pid_t child=fork(); assert(child>=0);
        if (!child) { close(fds[0]); server(fds[1],mode); _exit(0); }
        close(fds[1]); g_stream_fd=fds[0];
        assert(cmd_input_batch(8,args)==(mode ? 1 : 0));
        close(fds[0]); g_stream_fd=-1;
        int status;
        assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    }
    char *bad[]={"7","pointer","2","0","2147483648"};
    assert(cmd_input_batch(5,bad)==2);
    bad[4]="-2147483649"; assert(cmd_input_batch(5,bad)==2);
    bad[4]="0"; bad[2]="0"; assert(cmd_input_batch(5,bad)==2); /* SYN supplied by CLI */
    char *release[]={"7","pointer"};
    for (unsigned mode=0;mode<3;++mode) {
        int fds[2]; assert(socketpair(AF_UNIX,SOCK_STREAM,0,fds)==0);
        pid_t child=fork(); assert(child>=0);
        if (!child) { close(fds[0]); release_server(fds[1],mode); _exit(0); }
        close(fds[1]); g_stream_fd=fds[0];
        assert(cmd_input_release(2,release)==(mode ? 1 : 0));
        close(fds[0]); g_stream_fd=-1;
        int status;
        assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    }
    assert(cmd_input_release(1,release)==2);
    puts("PASS: input CLI exact signed events, complete batches, backpressure and reply validation");
    return 0;
}
