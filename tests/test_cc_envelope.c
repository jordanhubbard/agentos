/* Host test: envelope admission table. No seL4 IPC; logic only. */
#include <assert.h>
#include <stdio.h>
#include "contracts/cc_envelope.h"

int main(void)
{
    /* In-envelope: console and guest lifecycle. */
    assert(cc_envelope_admits(CC_ENVELOPE_OPERATOR, MSG_CC_SEND_INPUT));
    assert(cc_envelope_admits(CC_ENVELOPE_OPERATOR, MSG_CC_CREATE_GUEST));
    assert(cc_envelope_admits(CC_ENVELOPE_OPERATOR, MSG_CC_DESTROY_GUEST));
    assert(cc_envelope_admits(CC_ENVELOPE_OPERATOR, MSG_CC_SUSPEND_GUEST));
    assert(cc_envelope_admits(CC_ENVELOPE_OPERATOR, MSG_CC_RESUME_GUEST));
    assert(cc_envelope_admits(CC_ENVELOPE_OPERATOR, MSG_CC_LIST_GUESTS));
    assert(cc_envelope_admits(CC_ENVELOPE_OPERATOR, MSG_CC_LOG_STREAM));

    /* Out of envelope: exfiltration and debug primitives. */
    assert(!cc_envelope_admits(CC_ENVELOPE_OPERATOR, MSG_CC_SNAPSHOT));
    assert(!cc_envelope_admits(CC_ENVELOPE_OPERATOR, MSG_CC_RESTORE));
    assert(!cc_envelope_admits(CC_ENVELOPE_OPERATOR, MSG_CC_FAULT_INJECT));
    assert(!cc_envelope_admits(CC_ENVELOPE_OPERATOR, MSG_CC_TRACE_START));
    assert(!cc_envelope_admits(CC_ENVELOPE_OPERATOR, MSG_CC_TRACE_DUMP));

    /* Allowlist, not denylist: an unknown opcode is refused. */
    assert(!cc_envelope_admits(CC_ENVELOPE_OPERATOR, 0x26FFu));

    /* No envelope admits nothing. */
    assert(!cc_envelope_admits(CC_ENVELOPE_NONE, MSG_CC_SEND_INPUT));
    assert(!cc_envelope_admits(CC_ENVELOPE_NONE, MSG_CC_LIST_GUESTS));

    /* Pre-auth opcodes are reachable without a session. */
    assert(cc_envelope_is_preauth(MSG_CC_CONNECT));
    assert(cc_envelope_is_preauth(MSG_CC_DISCONNECT));
    assert(!cc_envelope_is_preauth(MSG_CC_SEND_INPUT));
    assert(!cc_envelope_is_preauth(MSG_CC_SNAPSHOT));

    printf("test_cc_envelope: PASS\n");
    return 0;
}
