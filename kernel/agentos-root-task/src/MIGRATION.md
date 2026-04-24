# agentOS PD Migration Template: Microkit to Raw seL4 IPC

This document is the canonical reference for the E5 migration track.
E5-S1 migrated `log_drain.c` and `event_bus.c` as the first two examples.
E5-S2 through E5-S8 follow this template.

---

## Overview

**Before (Microkit):** A PD exposes `void init(void)`, `void notified(ch)`,
and `microkit_msginfo protected(ch, msginfo)`.  The build system wires them
up via the `.system` file.

**After (raw seL4):** A PD exposes a single entry-point function
`void <pd>_main(seL4_CPtr my_ep, ...)` called by the root task boot
dispatcher.  Dispatch is handled by `sel4_server_t` from `sel4_server.h`.

---

## Step-by-Step

### Step 1 — Replace the includes

Remove:

```c
#include "agentos.h"   /* pulls in microkit.h and Microkit ABI */
```

Add:

```c
#ifdef AGENTOS_TEST_HOST
/* inline type stubs for host-side tests (see nameserver.c for the full
   boilerplate) */
#else
#include "sel4_ipc.h"     /* sel4_msg_t, sel4_badge_t, SEL4_ERR_* */
#include "sel4_server.h"  /* sel4_server_t, sel4_server_init/register/run */
#include "sel4_client.h"  /* sel4_client_t, sel4_client_connect, sel4_client_call
                             — only if the PD makes outbound calls */
#endif
```

For PDs that need contract opcodes but no longer pull in `agentos.h`,
copy the relevant `#define` block at the top of the file under a
`#ifndef` guard so that host-test builds can override them.

### Step 2 — Replace `void init(void)` with `<pd>_main`

```c
/* Before */
void init(void) {
    /* one-time setup */
}

/* After */
void my_pd_main(seL4_CPtr my_ep, seL4_CPtr ns_ep /*, other eps */) {
    /* one-time setup */
    register_with_nameserver(ns_ep);  /* see Step 3 */
    sel4_server_init(&g_srv, my_ep);
    /* ... sel4_server_register calls ... */
    sel4_server_run(&g_srv);  /* NEVER RETURNS */
}
```

The root task's boot dispatcher must be updated to call this entry point
instead of relying on Microkit's `init()` hook.

### Step 3 — Register with the nameserver

At startup, call `OP_NS_REGISTER` on `ns_ep`:

```c
static void register_with_nameserver(seL4_CPtr ns_ep) {
    if (!ns_ep) return;
    sel4_msg_t req, rep;
    req.opcode = OP_NS_REGISTER;
    /* data[0..3]   = channel_id (0 or static channel number) */
    /* data[4..7]   = pd_id (TRACE_PD_* constant) */
    /* data[8..11]  = cap_classes (CAP_CLASS_* bitmask) */
    /* data[12..15] = version (1) */
    /* data[16..47] = name (NS_NAME_MAX bytes, NUL-padded) */
    req.length = 48;
    sel4_call(ns_ep, &req, &rep);
    /* ignore result — if nameserver is offline, continue */
}
```

### Step 4 — Replace `protected(ch, msginfo)` with handler functions

```c
/* Before */
microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo) {
    uint32_t op = (uint32_t)microkit_mr_get(0);
    switch (op) {
        case OP_FOO: handle_foo(); break;
        default: ...;
    }
    return microkit_msginfo_new(0, 4);
}

/* After: one function per opcode */
static uint32_t handle_foo(sel4_badge_t badge, const sel4_msg_t *req,
                            sel4_msg_t *rep, void *ctx) {
    (void)badge; (void)ctx;
    /* read args from req->data[] using data_rd32/data_rd64 helpers */
    /* write results to rep->data[] using data_wr32/data_wr64 helpers */
    rep->length = N;   /* number of valid bytes in rep->data[] */
    return SEL4_ERR_OK;
}

/* Register in main: */
sel4_server_register(&g_srv, OP_FOO, handle_foo, NULL);
```

Handler return value is stored in `rep->opcode` by the dispatch loop;
callers read `rep->opcode` as the status code (`SEL4_ERR_OK` = 0).

### Step 5 — Replace `notified(ch)`

**PD listens for notifications (e.g., drain trigger):**

```c
/* Before */
void notified(microkit_channel ch) {
    (void)ch;
    do_drain();
}

/* After (option A): poll notification in server loop
 * Not yet supported by sel4_server_run.  Use a separate thread or
 * handle notifications as a special opcode via a notification endpoint.
 * Recommended approach for E5: model the notification as a seL4_Call
 * with a dedicated OP_DRAIN opcode; retire Microkit notifications.
 */
```

**PD sends notifications (e.g., eventbus -> subscribers):**

```c
/* Before */
microkit_notify(subscriber_ch);

/* After */
seL4_Signal(subscriber_ntfn_cap);
```

The caller passes its notification cap (as a badge token or via
cap-transfer IPC) when subscribing.

### Step 6 — Replace `microkit_ppcall(ch, msg)` with `sel4_call`

```c
/* Before */
microkit_mr_set(0, OP_TARGET_OP);
microkit_mr_set(1, arg1);
microkit_ppcall(CH_TARGET, microkit_msginfo_new(0, 2));
uint32_t result = (uint32_t)microkit_mr_get(0);

/* After */
sel4_msg_t req = {0}, rep = {0};
req.opcode = OP_TARGET_OP;
data_wr32(req.data, 0, arg1);
req.length = 4;
sel4_call(target_ep_cap, &req, &rep);
uint32_t result = rep.opcode;  /* status */
uint32_t value  = data_rd32(rep.data, 0);
```

`target_ep_cap` comes from either:
- A static root-task allocation passed to `<pd>_main` as an argument, or
- A runtime nameserver lookup via `sel4_client_connect(&client, "target_svc", &ep)`.

### Step 7 — Replace MR access with data[] byte offsets

```c
/* Before */
uint32_t slot  = (uint32_t)microkit_mr_get(1);
uint32_t pd_id = (uint32_t)microkit_mr_get(2);
microkit_mr_set(0, 0);  /* ok */

/* After */
uint32_t slot  = data_rd32(req->data, 0);
uint32_t pd_id = data_rd32(req->data, 4);
data_wr32(rep->data, 0, 0u);  /* ok */
rep->length = 4;
```

Layout convention (arbitrary but must be documented in the contract):
- Arguments packed sequentially in 4-byte or 8-byte slots.
- `data[0..3]` = first arg, `data[4..7]` = second arg, etc.
- Matches the layout used by the nameserver (see `nameserver.c`).

### Step 8 — Replace `microkit_dbg_puts(s)` with seL4_DebugPutChar loop

```c
/* Before */
microkit_dbg_puts("[pd] message\n");

/* After */
static void dbg_puts(const char *s) {
    for (; *s; s++) seL4_DebugPutChar(*s);
}
dbg_puts("[pd] message\n");
```

In test builds, provide a `static inline void seL4_DebugPutChar(char c) { (void)c; }` stub.

---

## Test Pattern

For each migrated PD, add `tests/api/test_<pd>.c`.

Compile with:

```sh
cc -DAGENTOS_TEST_HOST -I tests/api \
   -I kernel/agentos-root-task/include \
   -o /tmp/test_<pd> tests/api/test_<pd>.c && /tmp/test_<pd>
```

Test structure (mirrors `nameserver.c`):

```c
#ifdef AGENTOS_TEST_HOST
#include "framework.h"
#define AGENTOS_TEST_HOST  /* ensure PD source sees the flag */
#include "../../kernel/agentos-root-task/src/<pd>.c"

static void test_opcode_foo(void) {
    <pd>_test_init();
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = OP_FOO;
    data_wr32(req.data, 0, arg);
    req.length = 4;
    uint32_t rc = <pd>_dispatch_one(0, &req, &rep);
    ASSERT_EQ(rc, SEL4_ERR_OK, "OP_FOO returns OK");
}

int main(void) {
    TAP_PLAN(N);
    test_opcode_foo();
    return tap_exit();
}
#endif
```

---

## Quick-reference: What Changed

| Microkit                         | Raw seL4 IPC                               |
|----------------------------------|--------------------------------------------|
| `#include "agentos.h"`           | `#include "sel4_ipc.h"` + `sel4_server.h`  |
| `void init(void)`                | `void pd_main(seL4_CPtr my_ep, ...)`       |
| `microkit_msginfo protected(ch, msg)` | `sel4_handler_fn` per opcode          |
| `void notified(ch)`              | `seL4_Signal(ntfn_cap)` / separate handler |
| `microkit_ppcall(ch, msg)`       | `sel4_call(ep_cap, &req, &rep)`            |
| `microkit_mr_get(n)`             | `data_rd32(req->data, n*4)`                |
| `microkit_mr_set(n, v)`          | `data_wr32(rep->data, n*4, v)`             |
| `microkit_notify(ch)`            | `seL4_Signal(ntfn_cap)`                    |
| `microkit_dbg_puts(s)`           | `for (; *s; s++) seL4_DebugPutChar(*s)`    |
| Static channel graph (`.system`) | Nameserver + dynamic `sel4_client_connect` |

---

## Files Changed per PD

Every migrated PD touches exactly these files:

1. `kernel/agentos-root-task/src/<pd>.c` — replace Microkit callbacks with
   `sel4_server_t` dispatch and `<pd>_main` entry point.
2. `tests/api/test_<pd>.c` — new file, TAP tests covering every opcode.
3. `kernel/agentos-root-task/src/MIGRATION.md` — this file (updated once, not per PD).

The `.system` manifest and root task boot dispatcher are updated in a
separate pass (E5-S9) after all PDs are migrated.
