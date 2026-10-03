/* Host test: envelope admission table. No seL4 IPC; logic only. */
#include <assert.h>
#include <stdio.h>
#include "contracts/cc_envelope.h"
#include "cc_operator_credential.h"

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
    assert(cc_envelope_admits(CC_ENVELOPE_OPERATOR, MSG_CC_FRAME_CAPTURE));
    assert(cc_envelope_admits(CC_ENVELOPE_OPERATOR, MSG_CC_INPUT_SUBMIT));
    assert(cc_envelope_admits(CC_ENVELOPE_OPERATOR, MSG_CC_OPERATOR_WRITE));
    /* Session management: ordinary admitted opcodes now, since the
     * envelope is established at CONNECTION_SYNC, before any dispatched
     * frame, rather than bypassing admission as a preauth special case. */
    assert(cc_envelope_admits(CC_ENVELOPE_OPERATOR, MSG_CC_CONNECT));
    assert(cc_envelope_admits(CC_ENVELOPE_OPERATOR, MSG_CC_DISCONNECT));

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
    assert(!cc_envelope_admits(CC_ENVELOPE_NONE, MSG_CC_FRAME_CAPTURE));
    assert(!cc_envelope_admits(CC_ENVELOPE_NONE, MSG_CC_INPUT_SUBMIT));
    assert(!cc_envelope_admits(CC_ENVELOPE_NONE, MSG_CC_OPERATOR_WRITE));
    assert(!cc_envelope_admits(CC_ENVELOPE_NONE, MSG_CC_CONNECT));
    assert(!cc_envelope_admits(CC_ENVELOPE_NONE, MSG_CC_DISCONNECT));

    /* Constant-time credential compare. */
    {
        uint8_t a[CC_OPERATOR_TOKEN_BYTES];
        uint8_t b[CC_OPERATOR_TOKEN_BYTES];
        for (unsigned i = 0; i < CC_OPERATOR_TOKEN_BYTES; i++) {
            a[i] = (uint8_t)i;
            b[i] = (uint8_t)i;
        }
        assert(cc_credential_equal(a, b));

        /* Mismatch in the first byte. */
        b[0] ^= 0xffu;
        assert(!cc_credential_equal(a, b));
        b[0] ^= 0xffu;

        /* Mismatch in the last byte must be refused identically — a compare
         * that short-circuits would still pass this, but a compare that
         * stops early on the FIRST byte would not reach here at all. */
        b[CC_OPERATOR_TOKEN_BYTES - 1u] ^= 0x01u;
        assert(!cc_credential_equal(a, b));
    }

    printf("test_cc_envelope: PASS\n");
    return 0;
}
