/*
 * CC Operator Authority Envelope
 *
 * The build defines which CC operations a local operator may perform. The
 * credential presented at MSG_CC_CONNECTION_SYNC — the connection handshake
 * every client sends exactly once, as its first frame, or cc_pd closes the
 * connection — selects an envelope; it does not authenticate a principal.
 * Under the platform threat model the operator is untrusted and can read
 * anything the image contains, so the credential is NOT a secret from the
 * operator. Its purpose is to distinguish an operator connection from an
 * unrelated local process, and to be the point where a hardware-backed
 * credential substitutes later.
 *
 * The envelope is connection-scoped, established once at CONNECTION_SYNC
 * and held in cc_pd's file-scope g_envelope for the life of the connection.
 * MSG_CC_CONNECT is session-scoped and narrower: it only allocates a slot
 * for the SEND/RECV relay, and plenty of real traffic (the INSPECT and
 * OPERATOR_READ/WRITE path in particular) is sessionless and never sends
 * it. Attaching authority to CONNECT instead of the connection handshake
 * left that sessionless traffic with no way to ever hold an envelope; that
 * is why authority now comes from CONNECTION_SYNC instead.
 *
 * Admission is an ALLOWLIST. An opcode absent from the table is refused, so
 * an opcode added in future is outside the envelope until someone adds it
 * deliberately. With the envelope established at the handshake, before the
 * first dispatched frame, there is nothing left that must bypass this
 * check: CONNECT and DISCONNECT are ordinary admitted opcodes like any
 * other.
 *
 * This is defense in depth, not the primary control. Operations whose
 * authority is a separate capability are excluded structurally: cc_pd holds
 * no fault-injection endpoint unless AGENTOS_FAULT_INJECT is defined, so
 * MSG_CC_FAULT_INJECT cannot be performed in the default image regardless of
 * what this table says.
 */

#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "../agentos.h"

#define CC_ENVELOPE_VERSION 1u

typedef enum {
    CC_ENVELOPE_NONE     = 0u, /* admits nothing; pre-credential state */
    CC_ENVELOPE_OPERATOR = 1u, /* console + guest lifecycle */
} cc_envelope_t;

static inline bool cc_envelope_admits(cc_envelope_t envelope, uint32_t opcode)
{
    if (envelope != CC_ENVELOPE_OPERATOR) return false;

    switch (opcode) {
    /* Session management. By the time any frame reaches cc_dispatch, the
     * connection has already passed the CONNECTION_SYNC credential check,
     * so these are ordinary admitted opcodes rather than a special preauth
     * case. */
    case MSG_CC_CONNECT:
    case MSG_CC_DISCONNECT:
    /* Console and observation. */
    case MSG_CC_SEND:
    case MSG_CC_RECV:
    case MSG_CC_STATUS:
    case MSG_CC_LIST:
    case MSG_CC_SEND_INPUT:
    case MSG_CC_FRAME_CAPTURE:
    case MSG_CC_INPUT_SUBMIT:
    case MSG_CC_LOG_STREAM:
    case MSG_CC_INSPECT:
    case MSG_CC_OPERATOR_READ:
    case MSG_CC_OPERATOR_WRITE:
    case MSG_CC_LIST_GUESTS:
    case MSG_CC_LIST_DEVICES:
    case MSG_CC_LIST_POLECATS:
    case MSG_CC_GUEST_STATUS:
    case MSG_CC_DEVICE_STATUS:
    case MSG_CC_ATTACH_FRAMEBUFFER:
    /* Guest lifecycle: an operator who cannot restart a guest cannot run
     * the box. */
    case MSG_CC_CREATE_GUEST:
    case MSG_CC_SUSPEND_GUEST:
    case MSG_CC_RESUME_GUEST:
    case MSG_CC_DESTROY_GUEST:
        return true;

    /* Deliberately absent, each for a stated reason:
     *   MSG_CC_SNAPSHOT / MSG_CC_RESTORE — snapshot reads guest RAM in full
     *     and is an exfiltration primitive under this threat model.
     *   MSG_CC_FAULT_INJECT — a deliberate attack tool.
     *   MSG_CC_TRACE_* — debug surface.
     * These require vendor-signed authorization, which is not yet
     * implemented; until it is, they are simply unavailable. */
    default:
        return false;
    }
}

/*
 * Envelope admission. Defense in depth: the operations that matter most are
 * already excluded structurally (cc_pd holds no fault-injection endpoint in
 * the default image), and this check must never be the sole control for
 * anything, because it is exactly the layer an input-parsing defect bypasses.
 */
static inline bool cc_envelope_permits(uint32_t opcode, uint32_t envelope)
{
    return cc_envelope_admits((cc_envelope_t)envelope, opcode);
}
