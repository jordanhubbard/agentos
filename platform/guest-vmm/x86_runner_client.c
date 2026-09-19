#include <platform/x86_runner_client.h>

bool aos_x86_runner_call(aos_x86_runner_t *s, seL4_CPtr endpoint,
    const aos_x86_vmenter_entry_t *entry, aos_x86_vmenter_return_t *out)
{
    if (!s || !entry || !out || !endpoint || s->failed || s->active ||
        !s->next_sequence || s->next_sequence==UINT64_MAX) return false;
    uint64_t sequence=s->next_sequence++;
    s->active=true;
    seL4_SetMR(0,AOS_X86_RUNNER_VERSION);
    seL4_SetMR(1,sequence);
    seL4_SetMR(2,entry->ip);
    seL4_SetMR(3,entry->controls);
    seL4_SetMR(4,entry->interruption_info);
    seL4_MessageInfo_t info=seL4_Call(endpoint,
        seL4_MessageInfo_new(AOS_X86_RUNNER_ENTER,0,0,AOS_X86_RUNNER_REQUEST_WORDS));
    unsigned length=seL4_MessageInfo_get_length(info);
    if (seL4_MessageInfo_get_label(info)!=AOS_X86_RUNNER_RETURN ||
        seL4_MessageInfo_get_extraCaps(info) || seL4_MessageInfo_get_capsUnwrapped(info) ||
        length<AOS_X86_RUNNER_REPLY_HEADER_WORDS || length>AOS_X86_RUNNER_REPLY_MAX_WORDS)
        goto failed;
    aos_x86_runner_reply_t reply={.version=seL4_GetMR(0),.sequence=seL4_GetMR(1),
        .result=seL4_GetMR(2),.badge=seL4_GetMR(3),.count=seL4_GetMR(4)};
    if (!aos_x86_runner_reply_valid(&reply,length,sequence)) goto failed;
    aos_x86_vmenter_return_t returned={.result=reply.result,.badge=reply.badge};
    for (unsigned i=0; i<reply.count; i++)
        returned.words[i]=seL4_GetMR(AOS_X86_RUNNER_REPLY_HEADER_WORDS+i);
    *out=returned;
    s->active=false;
    return true;
failed:
    s->failed=true;
    s->active=false;
    return false;
}
