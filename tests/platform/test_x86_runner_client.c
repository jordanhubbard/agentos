#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <platform/x86_runner_client.h>

static seL4_Word mrs[30];
static unsigned mode, calls, reads, reply_length;
static const aos_x86_vmenter_entry_t entry={0xfff0,0x80,0x80000031};
void seL4_SetMR(int i,seL4_Word value)
{ assert(i>=0 && i<5); mrs[i]=value; }
seL4_Word seL4_GetMR(int i)
{ assert(i>=0 && (unsigned)i<reply_length); reads++; return mrs[i]; }
seL4_MessageInfo_t seL4_Call(seL4_CPtr ep,seL4_MessageInfo_t info)
{
    assert(ep==474 && info.label==1 && info.length==5 && !info.caps && !info.extra);
    assert(mrs[0]==1 && mrs[1]==1 && mrs[2]==entry.ip &&
        mrs[3]==entry.controls && mrs[4]==entry.interruption_info);
    calls++;
    bool notification=mode==1;
    mrs[0]=1; mrs[1]=1; mrs[2]=notification ? 0 : 1;
    mrs[3]=notification ? 0x40 : 0; mrs[4]=notification ? 3 : 25;
    for (unsigned i=5; i<30; i++) mrs[i]=0x1500+i-5;
    info=seL4_MessageInfo_new(2,0,0,notification ? 8 : 30);
    switch (mode) {
    case 2: info.label=3; break;
    case 3: info.length=4; break;
    case 4: info.length=31; break;
    case 5: info.caps=1; break;
    case 6: info.extra=1; break;
    case 7: mrs[0]=2; break;
    case 8: mrs[1]=2; break;
    case 9: mrs[2]=99; break;
    case 10: mrs[3]=1; break;
    case 11: mrs[4]=UINT64_MAX; break;
    default: break;
    }
    reply_length=info.length;
    return info;
}
int main(void)
{
    for (mode=0; mode<12; mode++) {
        aos_x86_runner_t state;
        aos_x86_runner_init(&state);
        aos_x86_vmenter_return_t out,old;
        memset(&out,0xa5,sizeof(out)); old=out;
        calls=reads=0;
        bool ok=aos_x86_runner_call(&state,474,&entry,&out);
        assert(ok==(mode<2) && calls==1 && !state.active && state.next_sequence==2);
        if (ok) {
            unsigned count=mode ? 3 : 25;
            assert(!state.failed && out.result==(mode ? 0u : 1u));
            assert(out.badge==(mode ? 0x40u : 0u) && reads==5+count);
            for (unsigned i=0; i<25; i++) assert(out.words[i]==(i<count ? 0x1500+i : 0));
        } else {
            assert(state.failed && !memcmp(&out,&old,sizeof(out)));
            assert(!aos_x86_runner_call(&state,474,&entry,&out) && calls==1);
            assert(reads==(mode<=6 ? 0u : 5u));
        }
    }
    puts("PASS: runner client validates full reply before publication and never retries ambiguous execution");
}
