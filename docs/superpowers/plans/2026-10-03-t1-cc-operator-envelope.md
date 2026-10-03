# T1 — CC Operator Authority Envelope Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bound the `cc_pd` control plane to a build-defined operator authority envelope, so that possession of the CC socket confers only the operations the build permits, and fix the session-reaping defect that lets an unauthenticated peer evict a live session.

**Architecture:** A build-generated credential selects an envelope at `MSG_CC_CONNECT`. The envelope is an **allowlist** of opcodes: anything not explicitly admitted is refused with a distinct error code. Enforcement sits in `cc_dispatch` as defense in depth; the operations that matter most are already structurally excluded because `cc_pd` holds no capability for them, and the allowlist keeps future opcodes excluded by default. Session reaping is restricted to sessions already marked expired.

**Tech Stack:** C11 freestanding (seL4/Microkit), `make test-host` for host logic, `cargo xtask qemu-test --<name>-probe N` for target proof. No new dependencies.

**Spec:** [`docs/superpowers/specs/2026-10-03-trust-delegation-design.md`](../specs/2026-10-03-trust-delegation-design.md)

## Global Constraints

- Language policy: C, Rust, or Assembly only, including tests and tooling. `make policy-check` enforces it.
- Never mock seL4 IPC as proof. Host tests under `-DAGENTOS_TEST_HOST` are a compile/logic pre-filter only (`tests/TARGET_TESTS.md`). Only a booted QEMU image asserted by an automated test proves target behavior.
- Do not extend museum components. `docs/TCB.md`: `cap_broker`, CapStore, `auth_server` and the rest are quarantined — "Do not extend these. Do not add opcodes. Do not 'finish' them." This plan adds no code to any of them.
- Source-grep assertions are not tests (ROADMAP corrective action 7). Every assertion here is behavioral.
- Contracts before callers. IPC contract changes land in `kernel/agentos-root-task/include/contracts/` before the code that uses them.
- Any component that lands updates `docs/TCB.md` in the same change with its qualification boundary stated.
- Threat model: the vendor is trusted, the local operator is not. The credential is **not** secret from the operator — it selects an envelope, it does not authenticate a principal. No step may describe it as a secret or claim it defends against the operator.

## Review Focus

Five failure modes the spec implies that no individual task's happy path exercises. Each has a test assigned to the task that owns the code.

1. **Vacuous refusal.** `MSG_CC_SNAPSHOT` already fails today — `vm_manager` returns not-implemented and `handle_snapshot` rewrites even `CC_OK` to `CC_ERR_RELAY_FAULT` (`cc_pd.c:1378`). A test asserting "snapshot is refused" passes for the wrong reason and would keep passing if the envelope were deleted. Every out-of-envelope test must assert the exact code `CC_ERR_NOT_PERMITTED`, never merely non-`CC_OK`. → Task 3.
2. **Opcode added later escapes the envelope.** A denylist would admit every future opcode by default. The envelope must be an allowlist, and a test must assert that an opcode absent from the table is refused. → Task 3.
3. **Enforcement bypassed by dispatch order.** `MSG_CC_CONNECT`, `MSG_CC_DISCONNECT`, and `MSG_CC_CONNECTION_SYNC` must remain reachable before a session exists, or a client can never connect. Getting the exemption set wrong either bricks the socket or opens a hole. → Task 3.
4. **Reaping still evicts a live session.** The fix must be tested by filling the table with *active* sessions and confirming the next connect is refused rather than succeeding by eviction. Testing only that expired sessions are reclaimed would pass with the defect intact. → Task 4.
5. **Credential compare leaks length or content by timing.** The credential is not secret today, but the same path takes a TPM-backed value later. Compare must be constant-time over a fixed length, and a test must confirm a mismatch in the final byte is refused exactly as a mismatch in the first. → Task 2.

---

### Task 1: Envelope contract and error code

**Files:**
- Create: `kernel/agentos-root-task/include/contracts/cc_envelope.h`
- Modify: `kernel/agentos-root-task/include/contracts/cc_contract.h:246` (add error code)
- Test: `tests/test_cc_envelope.c`

**Interfaces:**
- Consumes: `CC_OK`, `CC_ERR_*` from `cc_contract.h`; `MSG_CC_*` opcodes from `agentos.h`.
- Produces: `CC_ERR_NOT_PERMITTED` (value `11`); `CC_ENVELOPE_VERSION`; `cc_envelope_t`; `CC_ENVELOPE_OPERATOR`; `CC_ENVELOPE_NONE`; `bool cc_envelope_admits(cc_envelope_t envelope, uint32_t opcode)`; `bool cc_envelope_is_preauth(uint32_t opcode)`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_cc_envelope.c`:

```c
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
```

- [ ] **Step 2: Run it to make sure it fails**

Run: `clang -DAGENTOS_TEST_HOST -Itests -Ikernel/agentos-root-task/include -o /tmp/t_env tests/test_cc_envelope.c && /tmp/t_env`
Expected: FAIL — `fatal error: 'contracts/cc_envelope.h' file not found`

- [ ] **Step 3: Add the error code**

In `kernel/agentos-root-task/include/contracts/cc_contract.h`, extend the error enum (after `CC_ERR_WOULD_BLOCK = 10`):

```c
    CC_ERR_NOT_PERMITTED   = 11, /* opcode outside the session's authority envelope */
```

- [ ] **Step 4: Create the envelope contract**

Create `kernel/agentos-root-task/include/contracts/cc_envelope.h`:

```c
/*
 * CC Operator Authority Envelope
 *
 * The build defines which CC operations a local operator may perform. The
 * credential presented at MSG_CC_CONNECT selects an envelope; it does not
 * authenticate a principal. Under the platform threat model the operator is
 * untrusted and can read anything the image contains, so the credential is
 * NOT a secret from the operator. Its purpose is to distinguish an operator
 * session from an unrelated local process, and to be the point where a
 * hardware-backed credential substitutes later.
 *
 * Admission is an ALLOWLIST. An opcode absent from the table is refused, so
 * an opcode added in future is outside the envelope until someone adds it
 * deliberately.
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

/* Opcodes reachable before a session exists. Keep minimal: a client that
 * cannot CONNECT cannot obtain an envelope, and one that cannot DISCONNECT
 * leaks its slot. */
static inline bool cc_envelope_is_preauth(uint32_t opcode)
{
    switch (opcode) {
    case MSG_CC_CONNECT:
    case MSG_CC_DISCONNECT:
    case MSG_CC_CONNECTION_SYNC:
        return true;
    default:
        return false;
    }
}

static inline bool cc_envelope_admits(cc_envelope_t envelope, uint32_t opcode)
{
    if (envelope != CC_ENVELOPE_OPERATOR) return false;

    switch (opcode) {
    /* Console and observation. */
    case MSG_CC_SEND:
    case MSG_CC_RECV:
    case MSG_CC_STATUS:
    case MSG_CC_LIST:
    case MSG_CC_SEND_INPUT:
    case MSG_CC_LOG_STREAM:
    case MSG_CC_INSPECT:
    case MSG_CC_OPERATOR_READ:
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
     *   MSG_CC_OPERATOR_WRITE — mutation through the operator transport.
     * These require vendor-signed authorization, which is not yet
     * implemented; until it is, they are simply unavailable. */
    default:
        return false;
    }
}
```

- [ ] **Step 5: Run the test and make sure it passes**

Run: `clang -DAGENTOS_TEST_HOST -Itests -Ikernel/agentos-root-task/include -o /tmp/t_env tests/test_cc_envelope.c && /tmp/t_env`
Expected: PASS — `test_cc_envelope: PASS`

If any `MSG_CC_*` name fails to resolve, confirm its spelling in `kernel/agentos-root-task/include/agentos.h` and correct the table rather than removing the assertion.

- [ ] **Step 6: Wire the test into the host suite**

Add `test_cc_envelope` to the host test list in `Makefile` alongside the existing `test_cc_contract` entry, following that target's exact pattern.

- [ ] **Step 7: Run the host suite**

Run: `make test-host`
Expected: PASS, including `test_cc_envelope`.

- [ ] **Step 8: Commit**

```bash
git add kernel/agentos-root-task/include/contracts/cc_envelope.h \
        kernel/agentos-root-task/include/contracts/cc_contract.h \
        tests/test_cc_envelope.c Makefile
git commit -m "contracts: add CC operator authority envelope and CC_ERR_NOT_PERMITTED

The build defines which CC operations a local operator may perform.
Admission is an allowlist so future opcodes are excluded by default.
Contract only; no caller yet.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: Build-supplied credential and envelope selection

**Files:**
- Create: `kernel/agentos-root-task/include/cc_operator_credential.h`
- Modify: `services/command-console/cc_pd.c:567` (add `g_envelope`), `:1068-1085` (`handle_connect`), `:2040-2070` (connection-close paths)
- Modify: `tools/agentctl/agentctl.c:394-402` (`cmd_connect`)
- Modify: `kernel/agentos-root-task/Makefile` (credential define), possibly `tools/agentctl/Makefile`
- Test: `tests/test_cc_envelope.c` (extend)

**Interfaces:**
- Consumes: `cc_envelope_t`, `CC_ENVELOPE_OPERATOR`, `CC_ENVELOPE_NONE` from Task 1.
- Produces: `CC_OPERATOR_TOKEN_BYTES` (`32`); `const uint8_t cc_operator_token[32]`; `bool cc_credential_equal(const uint8_t *a, const uint8_t *b)`; file-scope `static uint32_t g_envelope` in `cc_pd.c`, which Task 3 reads at dispatch and Task 4 must not touch.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_cc_envelope.c`, before `printf`/`return` in `main`:

```c
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
```

Add `#include "cc_operator_credential.h"` to the test's includes.

- [ ] **Step 2: Run it to make sure it fails**

Run: `clang -DAGENTOS_TEST_HOST -Itests -Ikernel/agentos-root-task/include -o /tmp/t_env tests/test_cc_envelope.c && /tmp/t_env`
Expected: FAIL — `'cc_operator_credential.h' file not found`

- [ ] **Step 3: Create the credential header**

Create `kernel/agentos-root-task/include/cc_operator_credential.h`:

```c
/*
 * CC operator credential.
 *
 * Supplied by the build. NOT a secret from the operator: the platform threat
 * model treats the local operator as untrusted and assumes they can read the
 * image. The credential selects an authority envelope and distinguishes an
 * operator session from an unrelated local process. It does not, and must not
 * be described as, authenticating the operator.
 *
 * AGENTOS_CC_OPERATOR_TOKEN is a 32-byte initializer supplied at build time.
 * When it is not defined the build uses a well-known development value, and
 * that fact is logged at boot so a development image is never mistaken for a
 * provisioned one.
 */

#pragma once
#include <stdbool.h>
#include <stdint.h>

#define CC_OPERATOR_TOKEN_BYTES 32u

#ifdef AGENTOS_CC_OPERATOR_TOKEN
#define CC_OPERATOR_TOKEN_IS_DEVELOPMENT 0
static const uint8_t cc_operator_token[CC_OPERATOR_TOKEN_BYTES] = AGENTOS_CC_OPERATOR_TOKEN;
#else
#define CC_OPERATOR_TOKEN_IS_DEVELOPMENT 1
static const uint8_t cc_operator_token[CC_OPERATOR_TOKEN_BYTES] = {
    0x61, 0x67, 0x65, 0x6e, 0x74, 0x4f, 0x53, 0x2d,
    0x64, 0x65, 0x76, 0x2d, 0x6f, 0x70, 0x65, 0x72,
    0x61, 0x74, 0x6f, 0x72, 0x2d, 0x74, 0x6f, 0x6b,
    0x65, 0x6e, 0x2d, 0x76, 0x31, 0x00, 0x00, 0x00,
};
#endif

/* Constant-time over the full length. The credential is not secret today,
 * but this path takes a hardware-backed value later and must not leak
 * position of first difference by timing. */
static inline bool cc_credential_equal(const uint8_t *a, const uint8_t *b)
{
    uint8_t diff = 0u;
    for (uint32_t i = 0u; i < CC_OPERATOR_TOKEN_BYTES; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0u;
}
```

- [ ] **Step 4: Run the test and make sure it passes**

Run: `clang -DAGENTOS_TEST_HOST -Itests -Ikernel/agentos-root-task/include -o /tmp/t_env tests/test_cc_envelope.c && /tmp/t_env`
Expected: PASS

- [ ] **Step 5: Add the connection-scoped envelope**

The envelope is connection-scoped, not per-session: MR0 is not uniformly a
session id, so a per-session lookup at dispatch would read the wrong entry.
Every session on this transport shares one serialized socket stream and gets
the same build-fixed envelope.

In `services/command-console/cc_pd.c`, immediately after the `g_sessions`
declaration (around line 567):

```c
/* Authority envelope for this connection. Set by a successful CONNECT,
 * cleared when the connection closes. Not per-session: MR0 is a badge, a
 * guest handle or a slot id depending on opcode, so it cannot index a
 * session table at dispatch time. */
static uint32_t g_envelope = (uint32_t)CC_ENVELOPE_NONE;
```

Add to the includes near the top of the file:

```c
#include "contracts/cc_envelope.h"
#include "cc_operator_credential.h"
```

- [ ] **Step 6: Verify the credential at CONNECT**

Replace `handle_connect` (`cc_pd.c:1068`) in full:

```c
/*
 * MSG_CC_CONNECT — establish a session and select its authority envelope.
 *
 * Wire: the 32-byte operator credential occupies the first
 * CC_OPERATOR_TOKEN_BYTES of the request shmem. MR1 retains the caller's
 * requested badge, which is advisory only and confers nothing.
 */
static void handle_connect(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    if (!cc_credential_equal(req->shmem, cc_operator_token)) {
        rep->mr[0] = CC_ERR_NOT_PERMITTED;
        rep->mr[1] = 0u;
        return;
    }

    int s = alloc_session();
    if (s < 0) {
        rep->mr[0] = CC_ERR_NO_SESSIONS;
        rep->mr[1] = 0u;
        return;
    }
    g_sessions[s].active             = true;
    g_sessions[s].client_badge       = req->mr[0]; /* advisory; grants nothing */
    g_sessions[s].state              = CC_SESSION_STATE_CONNECTED;
    g_sessions[s].ticks_since_active = 0u;
    g_sessions[s].resp_pending       = 0u;
    g_sessions[s].resp_len           = 0u;

    g_envelope = (uint32_t)CC_ENVELOPE_OPERATOR;

    rep->mr[0] = CC_OK;
    rep->mr[1] = (uint32_t)s;
}
```

Confirm the request shmem field is named `shmem` in `cc_req_wire_t` (`cc_pd.c:543`); if it differs, use the actual name.

- [ ] **Step 6b: Reset the envelope when the connection drops**

In the main loop in `cc_pd_main`, wherever `connection_active` is set back to
false (the `close_pending` paths around `cc_pd.c:2040-2070`), also clear the
envelope:

```c
    g_envelope = (uint32_t)CC_ENVELOPE_NONE;
```

A new client must present the credential again; the previous client's
authority must not survive its connection. Read the surrounding loop and place
the reset on every path that invalidates the connection.

- [ ] **Step 6c: Update agentctl to send the credential**

`tools/agentctl/agentctl.c:397` calls CONNECT with a NULL payload, so it sends
an all-zero credential and will be refused after Step 6. agentctl drives
`make test-inspect` and the release harness, so this must land in the same
change — the repo's rule is contracts before callers, together.

Change `cmd_connect` to send the development credential in the request shmem:

```c
static int cmd_connect(void)
{
    cc_reply_wire_t r;
    uint8_t payload[CC_OPERATOR_TOKEN_BYTES];
    for (unsigned i = 0; i < CC_OPERATOR_TOKEN_BYTES; i++)
        payload[i] = cc_operator_token[i];
    if (!cc_call(MSG_CC_CONNECT, MY_BADGE, CC_CONNECT_FLAG_BINARY, 0,
                 payload, sizeof(payload), &r)) return 1;
    printf("{\"ok\":%" PRIu32 ",\"session_id\":%" PRIu32 "}\n",
           r.mr[0], r.mr[1]);
    return r.mr[0] == CC_OK ? 0 : 1;
}
```

Add `#include "cc_operator_credential.h"` to agentctl's includes. Confirm
`cc_call`'s payload parameters are `(const void *payload, size_t len)` in that
position by reading its definition; adapt the call if the signature differs.

agentctl builds against the same credential header, so a build configured with
`AGENTOS_CC_OPERATOR_TOKEN` produces a matching agentctl automatically. Check
whether `tools/agentctl/Makefile` needs the same `-D` plumbing and add it if so.

- [ ] **Step 7: Announce a development credential at boot**

In `cc_pd_main`, immediately before the existing `agentOS boot complete` print, add:

```c
#if CC_OPERATOR_TOKEN_IS_DEVELOPMENT
    sel4_dbg_puts("[cc_pd] WARNING: development operator credential in use\n");
#endif
```

- [ ] **Step 8: Add the build hook**

In `kernel/agentos-root-task/Makefile`, near the existing `AGENTOS_FAULT_INJECT` block (line ~749), add:

```make
ifneq ($(AGENTOS_CC_OPERATOR_TOKEN),)
  CFLAGS += -DAGENTOS_CC_OPERATOR_TOKEN='$(AGENTOS_CC_OPERATOR_TOKEN)'
endif
```

- [ ] **Step 9: Build and run the host suite**

Run: `make test-host && make build TARGET_ARCH=aarch64 GUEST_OS=none`
Expected: both succeed. The build must compile `cc_pd.elf` without warnings.

- [ ] **Step 10: Commit**

```bash
git add kernel/agentos-root-task/include/cc_operator_credential.h \
        services/command-console/cc_pd.c kernel/agentos-root-task/Makefile \
        tests/test_cc_envelope.c
git commit -m "cc_pd: select authority envelope from a build-supplied credential

handle_connect previously recorded a caller-supplied badge with no check.
The credential now selects an envelope; it is not a secret from the
operator and does not authenticate one. Development images announce
that they carry the well-known credential.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 3: Enforce the envelope at dispatch

**Files:**
- Modify: `services/command-console/cc_pd.c:1901` (`cc_dispatch`)
- Test: `tests/test_cc_envelope_dispatch.c`

**Interfaces:**
- Consumes: `cc_envelope_admits`, `cc_envelope_is_preauth` (Task 1); file-scope `static uint32_t g_envelope` in `cc_pd.c` (Task 2).
- Produces: `static inline bool cc_envelope_permits(uint32_t opcode, uint32_t envelope)` in `cc_envelope.h`. This is the only name for this function; `cc_pd.c` calls it directly and defines no wrapper.

- [ ] **Step 1: Write the failing test**

Create `tests/test_cc_envelope_dispatch.c`:

```c
/* Host test: dispatch-level envelope admission. Logic only, no seL4 IPC. */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "contracts/cc_envelope.h"
#include "contracts/cc_contract.h"

int main(void)
{
    /* Pre-auth opcodes bypass the envelope: a client with no session must
     * still be able to connect. */
    assert(cc_envelope_permits(MSG_CC_CONNECT,    CC_ENVELOPE_NONE));
    assert(cc_envelope_permits(MSG_CC_DISCONNECT, CC_ENVELOPE_NONE));

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
```

- [ ] **Step 2: Run it to make sure it fails**

Run: `clang -DAGENTOS_TEST_HOST -Itests -Ikernel/agentos-root-task/include -o /tmp/t_disp tests/test_cc_envelope_dispatch.c && /tmp/t_disp`
Expected: FAIL — implicit declaration of `cc_envelope_permits`

- [ ] **Step 3: Implement the decision function in the contract header**

The function lives in the header, not in `cc_pd.c`, so the host test can
exercise the production code without linking all of `cc_pd.c`.

Append to `kernel/agentos-root-task/include/contracts/cc_envelope.h`:

```c
/*
 * Envelope admission. Defense in depth: the operations that matter most are
 * already excluded structurally (cc_pd holds no fault-injection endpoint in
 * the default image), and this check must never be the sole control for
 * anything, because it is exactly the layer an input-parsing defect bypasses.
 */
static inline bool cc_envelope_permits(uint32_t opcode, uint32_t envelope)
{
    if (cc_envelope_is_preauth(opcode)) return true;
    return cc_envelope_admits((cc_envelope_t)envelope, opcode);
}
```

- [ ] **Step 4: Run the test and make sure it passes**

Run: `clang -DAGENTOS_TEST_HOST -Itests -Ikernel/agentos-root-task/include -o /tmp/t_disp tests/test_cc_envelope_dispatch.c && /tmp/t_disp`
Expected: PASS

- [ ] **Step 5: Gate the dispatcher**

In `cc_dispatch` (`cc_pd.c:1901`), immediately after the `cc_age_sessions();` call and before the `switch`:

```c
    /* Envelope admission.
     *
     * The envelope is connection-scoped, not per-session. MR0 is NOT uniformly
     * a session id — handle_connect reads it as a badge, handle_snapshot as a
     * guest handle, handle_fault_inject as a slot id — so indexing g_sessions[]
     * with it would read an unrelated session's envelope. Every session on this
     * transport shares one serialized socket stream and receives the same
     * build-fixed envelope, so one module-level value is both correct and
     * simpler. */
    if (!cc_envelope_permits(req->opcode, g_envelope)) {
        sel4_dbg_puts("[cc_pd] refused: outside operator envelope\n");
        rep->mr[0] = CC_ERR_NOT_PERMITTED;
        cc_trace_record(req->opcode);
        return;
    }
```

- [ ] **Step 6: Build and run the full host suite**

Run: `make test-host && make build TARGET_ARCH=aarch64 GUEST_OS=none`
Expected: PASS, including both new tests.

- [ ] **Step 7: Boot test**

Run: `make test TARGET_ARCH=aarch64 GUEST_OS=none`
Expected: boots and prints `agentOS boot complete`. If it hangs, the pre-auth set is wrong — the harness's own CC handshake is being refused. Check which opcode the harness sends first and whether it is in `cc_envelope_is_preauth`.

- [ ] **Step 8: Commit**

```bash
git add services/command-console/cc_pd.c \
        kernel/agentos-root-task/include/contracts/cc_envelope.h \
        tests/test_cc_envelope_dispatch.c Makefile
git commit -m "cc_pd: refuse operations outside the session authority envelope

Admission is checked once at dispatch, before the opcode switch. Allowlist
semantics mean an opcode added later is outside the envelope until admitted
deliberately. This is defense in depth, not the primary control.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: Restrict session reaping to expired sessions

**Files:**
- Modify: `services/command-console/cc_pd.c:1012-1031` (`reap_oldest_session`)
- Test: `tests/test_cc_session_reap.c`

**Interfaces:**
- Consumes: `cc_session_t`, `CC_MAX_SESSIONS`, `CC_SESSION_STATE_EXPIRED`.
- Produces: no new external symbols; behavior change only.

- [ ] **Step 1: Write the failing test**

Create `tests/test_cc_session_reap.c`:

```c
/* Host test: a live session must never be evicted to serve a new connect.
 *
 * Models the session table and the reap rule. The rule under test is the
 * predicate reap_oldest_session uses to choose a victim. */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "contracts/cc_contract.h"

typedef struct {
    bool     active;
    uint32_t state;
    uint32_t ticks_since_active;
} model_session_t;

static model_session_t g[CC_MAX_SESSIONS];

/* Mirrors the corrected rule: only a session already marked EXPIRED is a
 * candidate. Age alone is not sufficient. */
static int reap_candidate(void)
{
    int victim = -1;
    uint32_t oldest = 0u;
    for (int i = 0; i < (int)CC_MAX_SESSIONS; i++) {
        if (!g[i].active) continue;
        if (g[i].state != CC_SESSION_STATE_EXPIRED) continue;
        if (g[i].ticks_since_active >= oldest) {
            oldest = g[i].ticks_since_active;
            victim = i;
        }
    }
    return victim;
}

int main(void)
{
    /* Table full of LIVE sessions, all heavily aged. No victim may be
     * chosen — a new connect must be refused, not served by eviction. */
    for (int i = 0; i < (int)CC_MAX_SESSIONS; i++) {
        g[i].active = true;
        g[i].state = CC_SESSION_STATE_CONNECTED;
        g[i].ticks_since_active = 1000u + (uint32_t)i;
    }
    assert(reap_candidate() == -1);

    /* One session expires: it becomes the victim even though it is not the
     * oldest. */
    g[2].state = CC_SESSION_STATE_EXPIRED;
    g[2].ticks_since_active = 1u;
    assert(reap_candidate() == 2);

    /* Two expired: the older one is chosen. */
    g[5].state = CC_SESSION_STATE_EXPIRED;
    g[5].ticks_since_active = 9u;
    assert(reap_candidate() == 5);

    printf("test_cc_session_reap: PASS\n");
    return 0;
}
```

- [ ] **Step 2: Run it to make sure it fails**

It will not fail yet — the test models the corrected rule. Run it to confirm the model is self-consistent:

Run: `clang -DAGENTOS_TEST_HOST -Itests -Ikernel/agentos-root-task/include -o /tmp/t_reap tests/test_cc_session_reap.c && /tmp/t_reap`
Expected: PASS

Now demonstrate the defect the fix removes. Temporarily change `reap_candidate`'s expired check to the current production rule (`if (g[i].ticks_since_active < 1u) continue;` in place of the state check), rerun, and confirm the **first** assertion fails — a live session was chosen. Restore the correct rule before continuing. Record the observed failure in the commit message.

- [ ] **Step 3: Apply the fix**

Replace the victim-selection loop in `reap_oldest_session` (`cc_pd.c:1016`):

```c
static int reap_oldest_session(void)
{
    int victim = -1;
    uint32_t oldest = 0u;
    for (int i = 0; i < (int)CC_MAX_SESSIONS; i++) {
        if (!g_sessions[i].active) continue;
        /* Only a session already marked EXPIRED may be reclaimed. Age alone
         * is not sufficient: reaping on age let an unauthenticated peer evict
         * a live session by exhausting the table. */
        if (g_sessions[i].state != (uint32_t)CC_SESSION_STATE_EXPIRED) continue;
        if (g_sessions[i].ticks_since_active >= oldest) {
            oldest = g_sessions[i].ticks_since_active;
            victim = i;
        }
    }
    if (victim < 0) return -1;

    g_sessions[victim].active       = false;
    g_sessions[victim].state        = CC_SESSION_STATE_EXPIRED;
    g_sessions[victim].resp_pending = 0u;
    g_sessions[victim].resp_len     = 0u;
    return victim;
}
```

Do not clear `g_envelope` here. It is connection-scoped, not session-scoped —
reaping one abandoned session must not revoke the authority of the connection
that is still using the socket. Task 2 resets it on connection close.

- [ ] **Step 4: Update the stale comment above `cc_age_sessions`**

The comment at `cc_pd.c:994-1002` states "unconditionally reap it", which is now false. Replace that sentence with:

```c
 * full.  Only sessions already marked EXPIRED are reclaimed; reaping purely
 * on age allowed an unauthenticated peer to evict a live session by filling
 * the table.  A table full of live sessions now refuses new connects.
```

- [ ] **Step 5: Confirm sessions can still reach EXPIRED**

Read the aging path and confirm something transitions a session to `CC_SESSION_STATE_EXPIRED`. If nothing does, the table can now fill permanently. Add an age threshold in `cc_age_sessions`:

```c
#define CC_SESSION_EXPIRY_TICKS 4096u

        if (g_sessions[i].ticks_since_active >= CC_SESSION_EXPIRY_TICKS &&
            g_sessions[i].state != (uint32_t)CC_SESSION_STATE_EXPIRED) {
            g_sessions[i].state = (uint32_t)CC_SESSION_STATE_EXPIRED;
        }
```

This is a liveness requirement, not a security one: a stale session must eventually become reclaimable or the socket becomes unusable after eight abandoned clients.

- [ ] **Step 6: Run host suite and boot test**

Run: `make test-host && make test TARGET_ARCH=aarch64 GUEST_OS=none`
Expected: both PASS.

- [ ] **Step 7: Commit**

```bash
git add services/command-console/cc_pd.c tests/test_cc_session_reap.c Makefile
git commit -m "cc_pd: reclaim only expired sessions, never live ones

reap_oldest_session evicted the least-recently-active live session when the
table was full, so an unauthenticated peer could displace an established
session by connecting CC_MAX_SESSIONS times. Only sessions already marked
EXPIRED are now candidates, and aging marks a session expired after a
bounded idle period so the table still drains.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

### Task 5: Target proof

**Files:**
- Modify: `xtask/src/lib.rs:107` area (add `cc_envelope_probe` arg), `xtask/src/cmd_test.rs` (probe oracle)
- Modify: `Makefile` (add `test-cc-envelope` target, add to `gate`)
- Modify: `docs/TCB.md` (record the boundary and its scope)

**Interfaces:**
- Consumes: everything from Tasks 1–4.
- Produces: `make test-cc-envelope`; xtask flag `--cc-envelope-probe N` for N in 1..=3.

- [ ] **Step 1: Read the existing probe pattern**

Read how `virtualizer_authority_probe` is declared in `xtask/src/lib.rs:107` and consumed in `xtask/src/cmd_test.rs`. Mirror it exactly — argument declaration, `conflicts_with_all` list, and the marker-matching oracle. Do not invent a new mechanism.

- [ ] **Step 2: Define the three probes**

- Probe 1: connect with the correct credential, then `MSG_CC_LIST_GUESTS`. Expect `CC_OK`.
- Probe 2: connect with a credential differing in the final byte. Expect `CC_ERR_NOT_PERMITTED` and no session allocated.
- Probe 3: connect with the correct credential, then `MSG_CC_SNAPSHOT`. Expect **exactly** `CC_ERR_NOT_PERMITTED` (`11`), **not** `CC_ERR_RELAY_FAULT` (`8`).

Probe 3 is the one that matters. `handle_snapshot` already fails with `CC_ERR_RELAY_FAULT` because `vm_manager` returns not-implemented, so an oracle that accepts "any error" would pass with the envelope deleted. The oracle must compare the exact value.

- [ ] **Step 3: Add the Makefile target**

```make
test-cc-envelope:
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os none --timeout-secs $(QEMU_TEST_TIMEOUT) --cc-envelope-probe 1
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os none --timeout-secs $(QEMU_TEST_TIMEOUT) --cc-envelope-probe 2
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os none --timeout-secs $(QEMU_TEST_TIMEOUT) --cc-envelope-probe 3
```

- [ ] **Step 4: Run each probe individually**

Run: `make test-cc-envelope`
Expected: all three PASS. If probe 3 reports `8` rather than `11`, the dispatch gate is running after the handler — move it above the `switch`.

- [ ] **Step 5: Verify probe 3 is not vacuous**

Temporarily revert the `cc_dispatch` gate (Task 3 Step 5), rebuild, and run probe 3. It must now FAIL with `8`. Restore the gate and confirm it passes again. This is the check that the test tests the envelope rather than the missing snapshot implementation.

- [ ] **Step 6: Add to the gate**

Add `test-cc-envelope` to the `gate` target's prerequisite list, beside the existing guest I/O proofs.

- [ ] **Step 7: Update `docs/TCB.md`**

Add to the CC section, stating the boundary and its limits honestly:

```markdown
`cc_pd` admits only the operations in its build-defined operator envelope
(`contracts/cc_envelope.h`). The credential presented at CONNECT selects the
envelope; under the platform threat model the local operator is untrusted and
can read the image, so the credential is not secret from them and does not
authenticate a principal. Snapshot, restore, trace and fault injection are
outside the default envelope. `make test-cc-envelope` verifies admission,
credential mismatch, and that an out-of-envelope operation is refused with
CC_ERR_NOT_PERMITTED rather than failing for an unrelated reason. This bounds
what the CC transport conveys; it does not make the transport a capability
boundary, and vendor-signed authorization for out-of-envelope operations is
not implemented.
```

- [ ] **Step 8: Run the full gate**

Run: `make gate`
Expected: PASS.

- [ ] **Step 9: Commit**

```bash
git add xtask/src/lib.rs xtask/src/cmd_test.rs Makefile docs/TCB.md
git commit -m "test: target proof for the CC operator authority envelope

Three probes: an in-envelope operation succeeds, a mismatched credential is
refused, and an out-of-envelope operation is refused with the exact envelope
error rather than the unrelated relay fault it would otherwise return.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

## Notes for the implementer

**What this task does not accomplish.** The CC socket remains a privileged transport. The envelope bounds what that transport conveys; it does not make possession of the socket harmless, and it provides no defense against the local operator, who is assumed to be able to read the credential. Say so in any documentation you touch. The project's existing habit of stating qualification boundaries precisely is load-bearing — do not describe this change as "authenticating" the control plane.

**Layer 2 is deliberately deferred, not forgotten.** The spec describes three
enforcement layers. Layer 2 — `vm_manager` checking the caller's badge so that
snapshot and restore can be denied while guest lifecycle over the same endpoint
is permitted — is not implemented here, because `vm_manager` returns
not-implemented for both operations. Building badge enforcement for an
operation that does not exist would be speculative, and the allowlist covers
the risk it would address: when snapshot is implemented, it stays outside the
envelope until someone adds it deliberately. Layer 2 becomes required work in
the same change that implements snapshot, and the spec should be updated to say
so when that happens.

**Two exclusions were already true by accident.** `MSG_CC_FAULT_INJECT` is compiled out unless `AGENTOS_FAULT_INJECT` is defined, and `cc_pd` holds no fault-injection endpoint in the default image. Snapshot and restore already fail because `vm_manager` returns not-implemented. This plan makes both exclusions *intentional* so that implementing snapshot later does not silently make it operator-reachable. That is the whole value of the allowlist; preserve it.

**If a task reveals the envelope is wrong.** Open question 6 in the spec records that the envelope contents are an assumption taken in the user's absence, not a confirmed decision. If implementation shows the split is impractical — for instance if the test harness itself needs `MSG_CC_TRACE_*` — stop and raise it rather than widening the envelope to make a test pass.
