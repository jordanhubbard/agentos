#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sel4/sel4.h>
#include <contracts/x86_runner.h>

void pd_main(seL4_CPtr,seL4_CPtr);
static jmp_buf done;
static unsigned current, replies, enters, readable, reads;
static seL4_Word mrs[30];
struct command {
    seL4_Word badge,label,length,caps,extra,version,sequence;
    bool accepted;
};
/* Malformed envelopes must not consume sequence 1 or execute the guest.
 * A replay after execution must not consume sequence 2. */
static const struct command commands[]={
    {0,1,5,0,0,1,1,false}, {2,1,5,0,0,1,1,false},
    {1,99,5,0,0,1,1,false}, {1,1,4,0,0,1,1,false},
    {1,1,6,0,0,1,1,false}, {1,1,5,1,0,1,1,false},
    {1,1,5,0,1,1,1,false}, {1,1,5,0,0,2,1,false},
    {1,1,5,0,0,1,2,false}, {1,1,5,0,0,1,1,true},
    {1,1,5,0,0,1,1,false}, {1,1,5,0,0,1,2,true},
};
#ifdef CONFIG_KERNEL_MCS
seL4_MessageInfo_t seL4_Recv(seL4_CPtr ep,seL4_Word *badge,seL4_CPtr reply)
#else
seL4_MessageInfo_t seL4_Recv(seL4_CPtr ep,seL4_Word *badge)
#endif
{
#ifdef CONFIG_KERNEL_MCS
    assert(reply==9);
#endif
    assert(ep==77 && current==replies);
    if (current==sizeof(commands)/sizeof(commands[0])) longjmp(done,1);
    const struct command *c=&commands[current];
    memset(mrs,0xa5,sizeof(mrs));
    mrs[0]=c->version; mrs[1]=c->sequence;
    mrs[2]=0xfff0; mrs[3]=0x80; mrs[4]=0;
    readable=c->length; reads=0; *badge=c->badge;
    return seL4_MessageInfo_new(c->label,c->caps,c->extra,c->length);
}
seL4_Word seL4_GetMR(int i)
{
    assert(i>=0 && (unsigned)i<readable); reads++;
    return mrs[i];
}
void seL4_SetMR(int i,seL4_Word word)
{ assert(i>=0 && i<30); mrs[i]=word; }
seL4_Word seL4_VMEnter(seL4_Word *badge)
{
    assert(commands[current].accepted && reads==5);
    assert(mrs[0]==0xfff0 && mrs[1]==0x80 && mrs[2]==0);
    enters++;
    readable=enters==1 ? 25 : 3;
    for (unsigned i=0; i<30; i++) mrs[i]=0x1200+i;
    *badge=enters==1 ? 0 : 0x40;
    return enters==1 ? 1 : 0;
}
static void replied(seL4_MessageInfo_t info)
{
    const struct command *c=&commands[current];
    assert(info.caps==0 && info.extra==0);
    if (c->accepted) {
        unsigned count=enters==1 ? 25 : 3;
        assert(info.label==AOS_X86_RUNNER_RETURN && info.length==5+count);
        assert(mrs[0]==1 && mrs[1]==c->sequence && mrs[4]==count);
        assert(mrs[2]==(enters==1 ? 1u : 0u));
        assert(mrs[3]==(enters==1 ? 0u : 0x40u));
        for (unsigned i=0; i<count; i++) assert(mrs[5+i]==0x1200+i);
        assert(reads==5+count);
    } else {
        assert(info.label==AOS_X86_RUNNER_REJECT && info.length==0);
        if (current<7) assert(reads==0);
    }
    current++; replies++;
}
#ifdef CONFIG_KERNEL_MCS
void seL4_Send(seL4_CPtr ep,seL4_MessageInfo_t info) { assert(ep==9); replied(info); }
#else
void seL4_Reply(seL4_MessageInfo_t info) { replied(info); }
#endif
int main(void)
{
    if (!setjmp(done)) pd_main(77,0);
    assert(enters==2 && replies==sizeof(commands)/sizeof(commands[0]));
    puts("PASS: production runner server rejects malformed IPC, executes once and replies with exact snapshots");
}
