#include <platform/x86_runner.h>

void aos_x86_runner_init(aos_x86_runner_t *state)
{
    if (state) *state=(aos_x86_runner_t){.next_sequence=1u};
}

static unsigned exit_words(uint64_t result)
{
    return result==AOS_X86_RUNNER_FAULT ? AOS_X86_RUNNER_FAULT_WORDS :
        result==AOS_X86_RUNNER_NOTIFICATION ? AOS_X86_RUNNER_NOTIFICATION_WORDS : 0u;
}

bool aos_x86_runner_reply_valid(const aos_x86_runner_reply_t *reply,
                                size_t words, uint64_t expected_sequence)
{
    if (!reply || words<AOS_X86_RUNNER_REPLY_HEADER_WORDS ||
        words>AOS_X86_RUNNER_REPLY_MAX_WORDS ||
        !expected_sequence || expected_sequence==UINT64_MAX ||
        reply->version!=AOS_X86_RUNNER_VERSION || reply->sequence!=expected_sequence)
        return false;
    unsigned count=exit_words(reply->result);
    return count && reply->count==count &&
        words==AOS_X86_RUNNER_REPLY_HEADER_WORDS+count &&
        (reply->result!=AOS_X86_RUNNER_FAULT || !reply->badge);
}

bool aos_x86_runner_step(aos_x86_runner_t *state, unsigned label,
                         const aos_x86_runner_request_t *request, size_t words,
                         aos_x86_runner_enter_fn enter, void *context,
                         aos_x86_runner_reply_t *reply)
{
    if (!state || !request || !enter || !reply || state->failed || state->active ||
        !state->next_sequence || state->next_sequence==UINT64_MAX ||
        label!=AOS_X86_RUNNER_ENTER || words!=AOS_X86_RUNNER_REQUEST_WORDS ||
        request->version!=AOS_X86_RUNNER_VERSION ||
        request->sequence!=state->next_sequence) return false;
    uint64_t sequence=state->next_sequence++;
    state->active=true;
    aos_x86_runner_exit_t returned={0};
    bool success=enter(context,request->entry,&returned);
    unsigned count=exit_words(returned.result);
    if (!success || !count || (returned.result==AOS_X86_RUNNER_FAULT && returned.badge)) {
        state->failed=true;
        state->active=false;
        return false;
    }
    aos_x86_runner_reply_t next={.version=AOS_X86_RUNNER_VERSION,
        .sequence=sequence, .result=returned.result, .badge=returned.badge, .count=count};
    for (unsigned i=0; i<count; i++) next.words[i]=returned.words[i];
    *reply=next;
    state->active=false;
    return true;
}
