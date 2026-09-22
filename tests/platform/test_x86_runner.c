#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <platform/x86_runner.h>

static unsigned calls;
static bool succeed=true;
static aos_x86_runner_exit_t result;
static bool enter(void *context, const uint64_t *entry, aos_x86_runner_exit_t *out)
{
    assert(context==&calls);
    assert(entry[0]==0xfff0u && entry[1]==0x80u && entry[2]==0u);
    calls++;
    *out=result;
    return succeed;
}
static void refused(aos_x86_runner_t *s, unsigned label,
                    aos_x86_runner_request_t *request, size_t words)
{
    aos_x86_runner_t before=*s;
    aos_x86_runner_reply_t reply, old;
    memset(&reply,0xa5,sizeof(reply)); old=reply;
    unsigned count=calls;
    assert(!aos_x86_runner_step(s,label,request,words,enter,&calls,&reply));
    assert(calls==count && !memcmp(&reply,&old,sizeof(reply)));
    assert(!memcmp(s,&before,sizeof(before)));
}
int main(void)
{
    aos_x86_runner_t state;
    aos_x86_runner_init(&state);
    aos_x86_runner_request_t req={.version=1,.sequence=1,.entry={0xfff0,0x80,0}};
    aos_x86_runner_reply_t reply;
    refused(&state,99,&req,5);
    for (unsigned length=0; length<40; length++)
        if (length!=5) refused(&state,AOS_X86_RUNNER_ENTER,&req,length);
    req.version=2; refused(&state,AOS_X86_RUNNER_ENTER,&req,5); req.version=1;
    req.sequence=0; refused(&state,AOS_X86_RUNNER_ENTER,&req,5);
    req.sequence=2; refused(&state,AOS_X86_RUNNER_ENTER,&req,5); req.sequence=1;
    state.active=true; refused(&state,AOS_X86_RUNNER_ENTER,&req,5); state.active=false;
    result.result=AOS_X86_RUNNER_FAULT;
    for (unsigned i=0; i<25; i++) result.words[i]=0x1000u+i;
    assert(aos_x86_runner_step(&state,AOS_X86_RUNNER_ENTER,&req,5,enter,&calls,&reply));
    assert(calls==1 && state.next_sequence==2 && !state.active && !state.failed);
    assert(aos_x86_runner_reply_valid(&reply,30,1));
    assert(!memcmp(reply.words,result.words,sizeof(result.words)));
    refused(&state,AOS_X86_RUNNER_ENTER,&req,5); /* no duplicate VM entry */
    assert(!aos_x86_runner_reply_valid(&reply,30,2));
    for (unsigned length=0; length<40; length++)
        if (length!=30) assert(!aos_x86_runner_reply_valid(&reply,length,1));
    reply.badge=1; assert(!aos_x86_runner_reply_valid(&reply,30,1)); reply.badge=0;
    reply.count=3; assert(!aos_x86_runner_reply_valid(&reply,8,1));
    req.sequence=2;
    result.result=AOS_X86_RUNNER_NOTIFICATION; result.badge=0x10;
    assert(aos_x86_runner_step(&state,AOS_X86_RUNNER_ENTER,&req,5,enter,&calls,&reply));
    assert(calls==2 && reply.badge==0x10 && aos_x86_runner_reply_valid(&reply,8,2));
    for (unsigned i=0; i<25; i++) assert(reply.words[i]==(i<3 ? 0x1000u+i : 0));
    assert(!aos_x86_runner_reply_valid(&reply,30,2));
    for (unsigned failure=0; failure<3; failure++) {
        aos_x86_runner_init(&state); req.sequence=1;
        result.result=failure==0 ? 99 : AOS_X86_RUNNER_FAULT;
        result.badge=failure==1 ? 1 : 0;
        succeed=failure!=2;
        aos_x86_runner_reply_t old=reply;
        assert(!aos_x86_runner_step(&state,AOS_X86_RUNNER_ENTER,&req,5,enter,&calls,&reply));
        assert(state.failed && !state.active && state.next_sequence==2);
        assert(!memcmp(&reply,&old,sizeof(reply)));
        req.sequence=2; refused(&state,AOS_X86_RUNNER_ENTER,&req,5);
    }
    aos_x86_runner_init(&state); succeed=true;
    state.next_sequence=UINT64_MAX-1; req.sequence=UINT64_MAX-1;
    assert(aos_x86_runner_step(&state,AOS_X86_RUNNER_ENTER,&req,5,enter,&calls,&reply));
    assert(state.next_sequence==UINT64_MAX);
    req.sequence=UINT64_MAX; refused(&state,AOS_X86_RUNNER_ENTER,&req,5);
    assert(!aos_x86_runner_reply_valid(&reply,30,UINT64_MAX));
    puts("PASS: runner sequencing, exact exit snapshots, notification bounds and failed-entry latch");
    return 0;
}
