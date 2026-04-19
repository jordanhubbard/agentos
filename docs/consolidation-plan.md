# agentOS Service Consolidation Plan

**Date:** 2026-04-15  
**Based on:** `docs/api-surface-audit.md`  
**Status:** Partially executed — see §Completed Actions below.

---

## Architecture Summary

agentOS has three implementation layers for every major service:

| Layer | Location | Role | Authoritative? |
|---|---|---|---|
| Kernel Microkit PDs | `kernel/agentos-root-task/src/*.c` | Real seL4 protection domains; real IPC dispatch | **YES** |
| Rust userspace libs | `userspace/servers/<name>/src/lib.rs` | Simulation/test models; no IPC dispatch, no PD | Simulation only |
| C service prototypes | `services/<name>/*.c` | CAmkES/early prototypes; incomplete IPC; no PD | No (see per-service) |

The kernel PDs are the only architecturally correct deployed versions. The `services/` C files
predate Microkit and were never promoted to Microkit PDs. The Rust `userspace/servers/` crates
are `no_std` libraries used to model service behavior in the simulator (`userspace/sim/`), though
notably the sim currently has its own independent implementations (`SimCapStore`, `SimEventBus`)
and does NOT import the `userspace/servers/` crates at all.

---

## Completed Actions

### DELETED: `services/vibe-swap/` (V1 — Hard Constitutional Violation)

**What it was:** A Node.js ES-module HTTP server implementing the WASM hot-swap pipeline
(`stage`, `validate`, `commit`, `rollback`, `GET /status`).

**Why deleted:**
1. JavaScript/Node.js is explicitly forbidden by CLAUDE.md §Non-Negotiable Language Policy.
2. An HTTP server for a kernel service is a second violation.
3. Every function it implemented is already correctly present in:
   - `kernel/agentos-root-task/src/vibe_swap.c` — swap slot machinery (C, Microkit PD)
   - `kernel/agentos-root-task/src/vibe_engine.c` — propose/validate/execute/rollback PD opcodes
   - `userspace/servers/vibe-engine/src/wasm_validator.rs` — WASM magic + export validation
   - `userspace/servers/vibe-engine/src/lib.rs` — full validation pipeline in Rust

**Unique logic check:** The JS `validate()` only checked WASM magic bytes (`0x00 0x61 0x73 0x6D`).
`wasm_validator.rs` checks magic bytes, version, required exports (`init`, `handle_ppc`,
`health_check`, `notified`, `memory`), and scans for the `agentos.capabilities` custom section.
The Rust validator is strictly a superset. Nothing unique was lost.

**Files deleted:**
- `services/vibe-swap/src/index.mjs`
- `services/vibe-swap/src/server.mjs`
- `services/vibe-swap/test/pipeline.test.mjs`
- `services/vibe-swap/package.json`
- `services/vibe-swap/` (directory)

---

## Pending Actions: services/ C Prototypes

### `services/msgbus/` — REDESIGN, do not delete yet

**Contains unique logic:**
- `msgbus_seL4.c` has a real endpoint pool allocator (`alloc_endpoint`, `free_endpoint`,
  pool of 256 `seL4_CPtr` eps + notifications), an agent registry mapping agent IDs to
  notification caps, and a partially-wired `seL4_Call`/`seL4_Reply` RPC path
  (`msgbus_rpc_seL4`). This is more advanced than the kernel `event_bus.c` PD which uses
  a shared-memory ring buffer model, not per-agent endpoint allocation.
- The CAmkES component definition (`MsgBus.camkes`) with `ChannelIface`/`MessageIface`/`RpcIface`
  typed interfaces documents the intended API surface.

**Kernel counterpart:** `kernel/agentos-root-task/src/event_bus.c` (313 lines) implements a
ring-buffer pub/sub with up to 64 subscribers. It does NOT implement point-to-point RPC or
channel-level endpoint pools. The two models are complementary, not duplicates.

**Action:** Before any future deletion, the endpoint-pool and per-agent-registry approach in
`msgbus_seL4.c` must be evaluated against the EventBus PD's design. If point-to-point RPC
is needed (it is — it is how agents call tools), the `msgbus_rpc_seL4` logic should be
absorbed into the NameServer or a dedicated RPC-broker PD, not discarded.

**CMakeLists.txt change required:** Remove `msgbus` from `agentos-init` link libraries
(see §CMake Cleanup below).

---

### `services/capstore/capstore.c` — REDESIGN, do not delete yet

**Contains unique logic:**
- Cascading revocation via parent-chain walk (`capstore_revoke` iterates until no more
  children are marked — a fixed-point loop over 4096 entries).
- Rights-subset enforcement in `capstore_derive`: `(rights & parent.rights) != rights` check.
- Explicit `CAP_TYPE_*` enum with 9 semantic types (`TOOL`, `MODEL`, `MEMORY`, `MSG`, `STORE`,
  `SPAWN`, `NET`, `SELF`, `SERVICE`).

**Kernel counterpart:** `kernel/agentos-root-task/src/cap_broker.c` (647 lines) operates on
a binary policy blob with per-rule grant/revoke, but does NOT implement a derivation tree or
cascading revocation. The two models address different capability management concerns.

**Action:** The `capstore_derive` rights-subset check and cascading revocation algorithm should
be ported to the CapBroker PD (`cap_broker.c`) before `capstore.c` is retired. These are
security-critical invariants that the kernel PD currently lacks.

---

### `services/logsvc/logsvc.c` — REDESIGN, do not delete yet

**Contains unique logic:**
- Complete ring-buffer log implementation: 16,384-entry circular buffer, per-entry
  sequence numbers, `logsvc_query()` with since-seq/filter-agent/filter-level parameters.
- JSON serialization (`logsvc_entry_to_json`) with typed level names array.
- `LOG_AUDIT` level that bypasses the min-level filter (audit events always recorded).

**Kernel counterpart:** `kernel/agentos-root-task/src/log.c` exists but the audit found
a buffer-overflow bug in it (per `memory/project_agentos_console.md`). The `logsvc.c`
ring-buffer design is the better reference implementation.

**Action:** The `logsvc_query()` filter logic, the `LOG_AUDIT` bypass, and the JSON
serializer should be ported into the kernel `log.c` before `logsvc.c` is retired. The
16,384-entry ring and 512-byte message limit are reasonable constants to carry forward.
Fix the known buf-overflow in `log.c` first (it is blocking all log-path work).

---

### `services/memfs/memfs.c` — KEEP as reference implementation

**Audit disposition:** KEEP (explicitly designated as the vibe-swap replacement reference).

**Status:** This is the reference implementation that `vibe_swap.c`/`vibe_engine.c` tests
hot-swap correctness against. It implements the `STORAGE_OP_*` ABI from `services/abi/`.
Its 64-file / 4KB-per-file limits are intentional (small enough to be trivially replaceable).

**Action:** None. Do not delete. This file SHOULD remain as the canonical swap target.
It should eventually gain a Microkit PD wrapper so it is directly loadable as a swap slot.
The `services/abi/agentos_service_abi.h` file it aligns with should also be kept.

---

### `services/modelsvc/modelsvc.c` — SAFE TO DELETE (stub, logic present in model-proxy)

**Assessment:** Pure stub. `modelsvc_query()` explicitly says "HTTP inference not yet
implemented" and returns a static placeholder string. The only real logic is the model
registry (register/lookup by model_id) and `modelsvc_register_defaults()` which hardcodes
NVIDIA NIM + OpenAI endpoints.

**Kernel counterpart:** None directly. The `userspace/servers/model-proxy/src/lib.rs`
has a full `ModelRouter`, `PromptCache`, and `TokenBudget` implementation (all `no_std`).
There is no kernel PD for model inference yet — it is an open design task.

**Unique logic:** `modelsvc_register_defaults()` hardcodes specific endpoint URLs and API
key env var names (`NVIDIA_API_KEY`, `OPENAI_API_KEY`) that are not present elsewhere.
These defaults should be preserved in a config file or in the model-proxy Rust lib before
deletion.

**Recommended action:** Copy the three default endpoint registrations into a comment block
in `userspace/servers/model-proxy/src/lib.rs` (or a `defaults.rs`), then delete
`services/modelsvc/`. Safe to do once that extraction is confirmed.

**Do NOT delete yet** — extract defaults first.

---

### `services/toolsvc/toolsvc.c` — SAFE TO DELETE (stub, logic superseded)

**Assessment:** `toolsvc_dispatch()` logs but never makes an IPC call. The cap validation is
a `TODO`. The JSON listing (`toolsvc_list_json`) is the only working logic.

**Kernel counterpart:** None directly. `userspace/servers/tool-registry/src/lib.rs` is a
full `no_std` implementation with `invoke()`, access control (`grant_access`), invocation
logging, and search. It is substantially more complete.

**Unique logic:** `toolsvc_list_json()` produces MCP-compatible JSON including `inputSchema`
and `calls` count. The Rust `tool-registry` lib does not produce JSON output directly (it
returns typed `Vec<&ToolDef>`). This JSON format should be documented or added to the
Rust lib before deletion.

**Recommended action:** Add a `list_json()` method to the Rust `ToolRegistry` that produces
the same MCP-compatible format, then delete `services/toolsvc/`. The JSON format is
straightforward (5 lines of snprintf logic).

**Do NOT delete yet** — add `list_json()` to Rust lib first.

---

### `services/dev-shell/` and `services/generic-devices/` — Assess separately

These directories exist but contain only README files. No C source files.
They are documentation placeholders, not code. Safe to retain as-is.

---

## Pending Actions: Rust `userspace/servers/` Crates

### Relationship to Simulator

The `userspace/sim/` crate does **not** import any `userspace/servers/` crate.
The sim has its own independent implementations:
- `userspace/sim/src/eventbus.rs` — `SimEventBus` (closure-based, std, independent of `agentos-event-bus`)
- `userspace/sim/src/caps.rs` — `SimCapStore` (independent of `agentos-capability-broker`)

This means the `userspace/servers/` crates are currently **orphaned** — no other crate in
the workspace depends on them. They compile as workspace members but nothing links against them.

### `userspace/servers/capability-broker/` — LABEL as simulation model; add sim dependency

**Status:** 14 unit tests, clean `no_std` Rust. Well-implemented token-based grant/delegate/
revoke API with audit log. Currently orphaned (sim has its own `SimCapStore`).

**Recommended action:**
1. Add `description = "Simulation model of the CapBroker PD — NOT deployed on seL4"` to
   `Cargo.toml`.
2. Add a `[features] simulation = []` gate and annotate the crate accordingly.
3. Evaluate merging `SimCapStore` (sim/caps.rs) into this crate under the `simulation`
   feature, so there is one canonical test model rather than two separate implementations.
4. Long-term: the `no_std` types (`Grant`, `GrantToken`, `AuditEntry`, `CapKind`) should
   be usable by the kernel PD via FFI or shared header generation.

### `userspace/servers/event-bus/` — LABEL as simulation model; merge with SimEventBus

**Status:** 14 unit tests, clean `no_std` Rust. Currently orphaned.

**Recommended action:**
1. Add `description = "Simulation model of the EventBus PD — NOT deployed on seL4"` to
   `Cargo.toml`.
2. Merge `userspace/sim/src/eventbus.rs` (`SimEventBus`) into this crate under a
   `simulation` feature flag. Currently `SimEventBus` uses `std` (closures, `HashMap`)
   while `agentos-event-bus` is `no_std`. They model the same system.
3. Long-term: publish the `EventEntry` type and topic constants as shared definitions
   usable by both the simulator and the kernel PD integration tests.

### `userspace/servers/tool-registry/` — LABEL as simulation model; add list_json

**Status:** 17 unit tests. Depends on `agentos-sdk`. Currently orphaned.

**Recommended action:**
1. Add `description = "Simulation model of the ToolSvc — NOT deployed on seL4"` to
   `Cargo.toml`.
2. Add a `list_json(caller_badge) -> String` method that produces MCP-compatible JSON
   (needed before `services/toolsvc/toolsvc.c` can be deleted).
3. Have `userspace/sim/` depend on this crate for tool simulation.

### `userspace/servers/model-proxy/` — LABEL as simulation model; guard HTTP behind feature

**Status:** No tests visible. Depends on `agentos-sdk`. Optional `std` feature enables
reqwest/tokio HTTP backend. `BackendType::CodingCli { cli_path }` is dev-only.

**Recommended action:**
1. Add `description = "Simulation model of the ModelProxy PD — NOT deployed on seL4"` to
   `Cargo.toml`.
2. The `BackendType::CodingCli` variant must be gated behind a `dev` feature and removed
   from any seL4 build path.
3. `BackendType::HttpApi` HTTP calls must route through the NetServer PD (`OP_NET_HTTP_POST
   = 0x500`) in the deployed case — add a `seL4_ipc` feature that replaces reqwest calls
   with the kernel PD path.
4. Add unit tests.

### `userspace/servers/vibe-engine/` — KEEP, already has sim integration

**Status:** Correctly integrated. The crate has an optional `agentos-sim` dependency
(behind the `std` feature) and is the most complete of all the server crates. `wasm_validator.rs`
has tests exercised via the sim. This crate is architecturally correct.

**No changes needed** beyond adding the `description` label.

### `userspace/servers/http-gateway/` — KEEP with constraints (see audit)

**Status:** Standalone Tokio/hyper binary. Legitimate thin HTTP ingress. Not a simulation
model — it is an actual deployable (but not a seL4 PD).

**Required changes (from audit recommendations):**
1. Remove the `"hint"` field from 404 JSON responses (`src/main.rs` line 341).
2. Add bearer-token authentication to the `/_gateway/routes` management endpoint.
3. Add a `[features] ipc_bridge = []` feature; gate the HTTP fallback dispatch behind
   `#[cfg(not(feature = "ipc_bridge"))]`.
4. Add at least route-table and health-check integration tests.

---

## CMake Cleanup Required

`CMakeLists.txt` has two phantom library targets that reference source files that do not exist:

```cmake
# These source files DO NOT EXIST on disk:
add_library(netstack STATIC services/netstack/netstack.c)  # line 105
add_library(blobsvc  STATIC services/blobsvc/blobsvc.c)   # line 112
```

Both `netstack` and `blobsvc` are linked into `agentos-init` (line 141–143) but their
source directories do not exist. The CMake build is currently broken for these targets.

**Action:** Remove the `netstack` and `blobsvc` library declarations and their corresponding
`target_link_libraries` entries from `CMakeLists.txt`. The real network functionality is in
the `net_server.c` Microkit PD; there is no equivalent CAmkES service for block storage.

Also remove `msgbus`, `toolsvc`, `modelsvc`, and `logsvc` from the `agentos-init` link
libraries once those services are confirmed superseded by their kernel PD counterparts.
`capstore` and `memfs` may be retained temporarily as library dependencies for the old
CAmkES init task.

---

## V2 Violation: `kernel/agentos-root-task/src/js_runtime.c` (Deferred)

The audit identified `js_runtime.c` as a kernel PD that evaluates JavaScript (opcodes
`OP_JS_EVAL`, `OP_JS_CALL`, `OP_JS_LOAD_MODULE`). This is a constitutional violation even
if the interpreter itself is C. Evaluation is deferred pending:

1. Confirmation of which agents (if any) currently use `OP_JS_EVAL`.
2. A replacement plan using the WASM3 runtime (`wasm3_host.c`) which is already present.

Do NOT delete `js_runtime.c` until the above is confirmed — if any currently-deployed
agent calls `OP_JS_EVAL`, removal would break the system without a migration path.

---

## V3 Violation: Missing `contracts/` Directory (Deferred)

Per the audit, the constitution requires `contracts/<service-name>/interface.h` for every
service before it may be called. Zero contract files exist. This is a systemic gap that
requires a separate work item. The six highest-priority candidates for contract files are:

1. `contracts/event-bus/` — most-called PD; subscribe/publish API must be documented
2. `contracts/cap-broker/` — security-critical; policy blob format must be specified
3. `contracts/agentfs/` — content-store used by attestation; object model must be published
4. `contracts/vibe-engine/` — hot-swap pipeline; WASM binary constraints must be specified
5. `contracts/nameserver/` — service discovery; lookup semantics must be documented
6. `contracts/http-svc/` — HTTP routing table; prefix format must be specified

---

## Summary Table

| Service | Location | Safe to Delete? | Unique Logic? | Action |
|---|---|---|---|---|
| `vibe-swap` | `services/vibe-swap/` | **YES — DONE** | No (covered by wasm_validator.rs) | Deleted |
| `modelsvc` | `services/modelsvc/` | Yes, after extraction | Default endpoints only | Extract defaults, then delete |
| `toolsvc` | `services/toolsvc/` | Yes, after extraction | JSON list format | Add list_json() to Rust lib, then delete |
| `msgbus` | `services/msgbus/` | No | Endpoint pool, per-agent registry, RPC path | Evaluate vs EventBus PD; absorb RPC design |
| `capstore` | `services/capstore/` | No | Cascading revoke, derivation tree | Port to cap_broker.c, then delete |
| `logsvc` | `services/logsvc/` | No | Ring buffer query, AUDIT bypass, JSON | Port to log.c (after fixing buf overflow), then delete |
| `memfs` | `services/memfs/` | No | Reference swap target | KEEP permanently |
| `capability-broker` | `userspace/servers/` | No | Simulation model | Label, merge SimCapStore |
| `event-bus` | `userspace/servers/` | No | Simulation model | Label, merge SimEventBus |
| `tool-registry` | `userspace/servers/` | No | Simulation model | Label, add list_json |
| `model-proxy` | `userspace/servers/` | No | Simulation model | Label, gate CodingCli |
| `vibe-engine` | `userspace/servers/` | No | Sim + validator | KEEP as-is |
| `http-gateway` | `userspace/servers/` | No | Real deployable | Remove hint, add auth, add ipc_bridge feature |
