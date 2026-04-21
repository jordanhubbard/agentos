# DEFECT-002: js_runtime — JavaScript Eval in Kernel Protection Domain

**Status:** RESOLVED — js_runtime deleted  
**Severity:** Critical (constitutional violation)  
**Detected:** 2026-04-15  
**Resolved:** 2026-04-15  

---

## What js_runtime Did

`kernel/agentos-root-task/src/js_runtime.c` was a passive seL4 Microkit
protection domain (priority 120) that hosted a QuickJS JavaScript engine
inside the kernel personality layer.  It accepted Protected Procedure Calls
(PPCs) on two channels — one from `controller` (ch 60) and one from
`init_agent` (ch 11) — and dispatched five opcodes:

| Opcode | Value | Action |
|---|---|---|
| OP_JS_EVAL | 0xC0 | Evaluate a JS script; auto-create context if needed |
| OP_JS_CALL | 0xC1 | Call a named global function in an existing context |
| OP_JS_LOAD_MODULE | 0xC2 | Register an ES module in a context |
| OP_JS_DESTROY | 0xC3 | Free a context slot and its QuickJS resources |
| OP_JS_HEALTH | 0xC4 | Return active context count + QuickJS version |

Script source and results were passed through a 4MB shared memory region
(`js_staging`) mapped at vaddr 0x7000000 in both the `js_runtime` PD and
`init_agent`.  Up to 8 concurrent JS contexts were supported, each backed by
a `JSRuntime` + `JSContext` pair (stubbed with no-op shims in
`vendor/quickjs/quickjs_stub.c`; full QuickJS was never linked).

State owned:
- `static js_context_t contexts[8]` — context table in BSS
- `static JSRuntime *context_js_rt[8]` / `static JSContext *context_js_ctx[8]`
- `uintptr_t js_staging_vaddr` — set by Microkit via `setvar_vaddr`

---

## Why It Was a Violation

The agentOS project constitution (CLAUDE.md) forbids JavaScript entirely:
the kernel is pure C + Rust + Assembly only.  A QuickJS embedding inside a
seL4 protection domain brings:

1. A large, non-audited C codebase (QuickJS) into trusted kernel space.
2. An arbitrary code-execution surface for any caller that can reach
   `init_agent` channel 11 or `controller` channel 60.
3. A class of JavaScript-level attacks (prototype pollution, eval injection)
   that do not exist in a pure C/WASM/Rust system.

The violation was already documented in `docs/api-surface-audit.md` §V2
before this defect was filed.

---

## What Called js_runtime

### `libs/libagent/libagent.c`

Two public libagent API functions called into js_runtime via seL4_Call on
CSpace slot 9 (`SLOT_JS_RUNTIME`):

- `aos_js_eval(context_id, script, out_buf, out_buf_len, out_len)`  
  Sent OP_JS_EVAL (0xC0) with the script placed in `js_staging` at offset 0.

- `aos_js_call(context_id, func_name, args_json, out_buf, out_buf_len, out_len)`  
  Sent OP_JS_CALL (0xC1) with func_name at offset 0 and args JSON at
  offset 0x1000 of `js_staging`.

These are the only callers found in the codebase.  No agent binary or kernel
PD other than libagent invoked these opcodes directly.

---

## Files Deleted

| File | Reason |
|---|---|
| `kernel/agentos-root-task/src/js_runtime.c` | The offending PD implementation |
| `kernel/agentos-root-task/include/js_runtime.h` | Public interface / opcode definitions |
| `kernel/agentos-root-task/vendor/quickjs/quickjs_stub.c` | QuickJS stub implementation |
| `kernel/agentos-root-task/vendor/quickjs/quickjs_stub.h` | QuickJS stub header |

---

## Files Modified

| File | Change |
|---|---|
| `kernel/agentos-root-task/Makefile` | Removed `PD_JS_RUNTIME_SRCS`, `PD_JS_RUNTIME_VENDOR_SRCS`, `JS_RUNTIME_OBJS`, `js_runtime.elf` from `IMAGES`, vendor build rule, and link rule |
| `kernel/agentos-root-task/agentos-aarch64.system` | Removed `js_staging` memory region, `js_runtime` PD, channels 60 and 61, and `js_staging` map from `init_agent` |
| `boards/rpi5/agentos.system` | Same removals as aarch64 system file |
| `libs/libagent/libagent.c` | Removed `#include "js_runtime.h"`, removed `SLOT_JS_RUNTIME`, replaced `aos_js_eval` and `aos_js_call` bodies with `AOS_ERR_IO` stubs and TODO comments |

---

## Recommended Replacement

Any functionality that required dynamic script execution should be
reimplemented using one of the following constitutional approaches:

### Option A — WASM via vibe_engine (preferred)

Compile agent logic to WASM and submit it via `OP_VIBE_HOTSWAP` to the
`vibe_engine` PD, which already runs a sandboxed wasm3 interpreter.  The
vibe_engine boundary is well-defined and does not bring a JS engine into
kernel space.

### Option B — Rust userspace tool

Implement the required scripting logic as a Rust tool registered in the
`tool-registry` userspace server.  If a JS-like scripting language is
genuinely needed, compile a minimal Lua or WASM-compiled JS interpreter as
a Rust library and expose it through the tool interface — entirely outside
the kernel personality layer.

### Option C — Static C configuration

For the common case of "run this agent-provided script at startup", replace
the dynamic eval with a static C dispatch table that agents populate via
structured IPC rather than arbitrary script strings.

---

## Channel IDs Freed

Removing js_runtime releases channel IDs 60 and 61 in `controller` and
channel IDs 60 and 61 in `init_agent` (from the aarch64 and rpi5 system
files).  These slots are now available for reuse.
