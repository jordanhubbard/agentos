/* Host test: dispatch-level envelope admission. Logic only, no seL4 IPC. */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "contracts/cc_envelope.h"
#include "contracts/cc_contract.h"

int main(void)
{
    /* The authority envelope is established at CONNECTION_SYNC, before any
     * frame reaches cc_dispatch, so by the time cc_dispatch ever sees a
     * frame — including CONNECT/DISCONNECT — the envelope is already
     * CC_ENVELOPE_OPERATOR or the connection was refused at the handshake
     * and never reached dispatch at all. CONNECT/DISCONNECT are therefore
     * ordinary admitted opcodes, not a preauth bypass. */
    assert(!cc_envelope_permits(MSG_CC_CONNECT,    CC_ENVELOPE_NONE));
    assert(!cc_envelope_permits(MSG_CC_DISCONNECT, CC_ENVELOPE_NONE));
    assert(cc_envelope_permits(MSG_CC_CONNECT,    CC_ENVELOPE_OPERATOR));
    assert(cc_envelope_permits(MSG_CC_DISCONNECT, CC_ENVELOPE_OPERATOR));

    /* Everything else is refused without an envelope. */
    assert(!cc_envelope_permits(MSG_CC_SEND_INPUT,  CC_ENVELOPE_NONE));
    assert(!cc_envelope_permits(MSG_CC_LIST_GUESTS, CC_ENVELOPE_NONE));

    /* In-envelope operations are admitted. */
    assert(cc_envelope_permits(MSG_CC_SEND_INPUT,    CC_ENVELOPE_OPERATOR));
    assert(cc_envelope_permits(MSG_CC_CREATE_GUEST,  CC_ENVELOPE_OPERATOR));
    assert(cc_envelope_permits(MSG_CC_DESTROY_GUEST, CC_ENVELOPE_OPERATOR));

    /* Out-of-envelope operations are refused even with a valid envelope.
     * These must be refused by the ENVELOPE, not because the downstream
     * service is unimplemented — Task 5's target test pins the exact code. */
    assert(!cc_envelope_permits(MSG_CC_SNAPSHOT,     CC_ENVELOPE_OPERATOR));
    assert(!cc_envelope_permits(MSG_CC_RESTORE,      CC_ENVELOPE_OPERATOR));
    assert(!cc_envelope_permits(MSG_CC_FAULT_INJECT, CC_ENVELOPE_OPERATOR));
    assert(!cc_envelope_permits(MSG_CC_TRACE_START,  CC_ENVELOPE_OPERATOR));

    /* An opcode nobody has defined is refused. Allowlist, not denylist. */
    assert(!cc_envelope_permits(0x26FFu, CC_ENVELOPE_OPERATOR));

    printf("test_cc_envelope_dispatch: PASS\n");
    return 0;
}
