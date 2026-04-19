# agentOS API Surface Audit

**Task:** 6 — Full API surface inventory  
**Date:** 2026-04-15  
**Auditor:** Claude Sonnet 4.6 (automated)  
**Scope:** All service source files in `userspace/servers/`, `services/`, and `kernel/agentos-root-task/src/`

---

## Executive Summary

The agentOS codebase contains **17 distinct service components** spread across three implementation layers: kernel-resident C protection domains, Rust userspace library servers, and C services that partially pre-date the Microkit architecture. The overall API surface is **large, partially duplicated, and contains at least three outright constitutional violations**.

Key findings:

1. **Two parallel implementations of every major service exist.** For example, `services/msgbus/msgbus.c` and `kernel/agentos-root-task/src/event_bus.c` both implement inter-agent messaging; `services/toolsvc/toolsvc.c` and `userspace/servers/tool-registry/src/lib.rs` both implement tool registration. Only the kernel PD version has a real seL4 IPC dispatch loop and is in the right place architecturally. The `services/` C files appear to be earlier prototypes or simulation stubs.

2. **Three services violate the language policy directly:**
   - `services/vibe-swap/` is JavaScript (Node.js) — a hard constitutional violation.
   - `services/vibe-swap/src/server.mjs` runs an HTTP server — a double violation (JS + HTTP UI exposure).
   - `userspace/servers/http-gateway/src/main.rs` runs a full hyper HTTP/1.1 proxy with Tokio — the binary itself is Rust (compliant), but its architecture (binding on `0.0.0.0:8080`, serving 404 JSON with HTML hints, acting as a general HTTP reverse proxy) blurs the line between a thin seL4 IPC bridge and a web-facing application tier.

3. **No service has a contract file in `contracts/`.** The constitution mandates `contracts/<service-name>/interface.h` and `contracts/<service-name>/README.md` for every service before it may be called. Zero exist today.

4. **Test coverage is uneven.** The Rust userspace servers (capability-broker, event-bus, tool-registry) have good unit test suites. The kernel C PDs have no automated tests visible in-tree. The JavaScript vibe-swap service has one test file (`test/pipeline.test.mjs`) but in a forbidden language.

5. **Critical gaps exist:** There is no formal spawn API contract, no VFS contract, no crypto-IPC contract, and no agent-identity contract available to callers — only source-code-level constants. The `contracts/` directory does not exist at all.

---

## Service Inventory Table

| Service | Language | Layer | Interface Type | Operations | Tested? | Constitution Compliant? | Disposition |
|---|---|---|---|---|---|---|---|
| **capability-broker** (userspace) | Rust | Userspace lib | Internal (no_std lib) | `allow`, `issue`, `validate`, `delegate`, `revoke`, `grant`, `check`, `check_audited`, `revoke_by_agent_cap`, `audit_recent` | Yes (unit tests in lib.rs) | Yes — but no contract file | REDESIGN |
| **event-bus** (userspace) | Rust | Userspace lib | Internal (no_std lib) | `create_topic`, `create_topic_owned`, `subscribe`, `unsubscribe`, `publish`, `publish_as`, `drain` | Yes (unit tests in lib.rs) | Yes — but no contract file | REDESIGN |
| **http-gateway** | Rust | Userspace server | HTTP/1.1 on TCP (Tokio + hyper) | `GET /health`, `GET /healthz`, `GET /_gateway/health`, `GET /_gateway/routes`, all-methods proxy via IPC dispatch | No | Questionable — see §Recommendations | REDESIGN |
| **model-proxy** (userspace) | Rust | Userspace lib | Internal (no_std lib) + optional HTTP | `register_endpoint`, `route`, `score_model`, `cache.get`, `cache.put`, `budget.check`, `budget.consume`, `budget.reset_period` | No (no tests visible) | Disputed — see §Recommendations | REDESIGN |
| **tool-registry** (userspace) | Rust | Userspace lib | Internal (no_std lib) | `register`, `deregister`, `unregister`, `grant_access`, `invoke`, `list`, `list_tools`, `search`, `invocation_log_recent`, `stats` | Yes (unit tests in lib.rs) | Yes — but no contract file | REDESIGN |
| **vibe-engine** (userspace) | Rust | Userspace lib + kernel PD | Internal lib + seL4 IPC PD | `validate_proposal`, `parse_agentos_caps_section`, `check_capability_policy` (lib); `OP_VIBE_PROPOSE/VALIDATE/EXECUTE/STATUS/ROLLBACK/HEALTH/REGISTER_SERVICE/LIST_SERVICES` (PD) | Partial (wasm_validator tests via sim) | Yes | KEEP |
| **MsgBus** (services/msgbus) | C | CAmkES + seL4 | seL4 IPC (CAmkES + direct) | `MSGBUS_OP_CREATE_CHANNEL (0x100)`, `DELETE_CHANNEL (0x101)`, `SUBSCRIBE (0x102)`, `UNSUBSCRIBE (0x103)`, `PUBLISH (0x104)`, `SEND_DIRECT (0x105)`, `RECV (0x106)`, `LIST_CHANNELS (0x107)`, `CHANNEL_INFO (0x108)`, `CALL_RPC (0x109)`, `REPLY_RPC (0x10A)` | No | Yes (C, CAmkES) — but stubs | REDESIGN |
| **CapStore** (services/capstore) | C | seL4 service | Internal C API | `capstore_init`, `capstore_register`, `capstore_derive`, `capstore_revoke`, `capstore_query_by_owner` | No | Yes (C) — prototype quality | REDESIGN |
| **LogSvc** (services/logsvc) | C | seL4 service | Internal C API | `logsvc_init`, `logsvc_write`, `logsvc_writef`, `logsvc_query`, `logsvc_entry_to_json` | No | Yes (C) | REDESIGN |
| **MemFS** (services/memfs) | C | seL4 service | Internal C API (maps to STORAGE_OP_* ABI) | `memfs_init`, `memfs_write`, `memfs_read`, `memfs_list`, `memfs_delete`, `memfs_tag`, `memfs_create` | No | Yes (C) — reference impl | KEEP |
| **ModelSvc** (services/modelsvc) | C | seL4 service | Internal C API | `modelsvc_init`, `modelsvc_register`, `modelsvc_register_defaults`, `modelsvc_query`, `modelsvc_stats` | No | Yes (C) — stubs only | REDESIGN |
| **ToolSvc** (services/toolsvc) | C | seL4 service | Internal C API | `toolsvc_init`, `toolsvc_register`, `toolsvc_unregister`, `toolsvc_dispatch`, `toolsvc_list_json` | No | Yes (C) — stubs only | REDESIGN |
| **vibe-swap** (services/vibe-swap) | **JavaScript (Node.js)** | Process over TCP | HTTP/1.1 | `POST /stage`, `POST /validate`, `POST /commit`, `POST /rollback`, `GET /status` | Yes (mjs test) | **NO — JS is forbidden** | **DELETE** |
| **EventBus PD** (kernel/event_bus.c) | C | seL4 Microkit PD | seL4 PPC (Microkit) | `MSG_EVENTBUS_INIT (0x01)`, `MSG_EVENTBUS_SUBSCRIBE (0x02)`, `MSG_EVENTBUS_UNSUBSCRIBE (0x03)`, `MSG_EVENTBUS_STATUS (0x04)`, `OP_PUBLISH_BATCH`, any label as `OP_PUBLISH` | No | Yes | KEEP |
| **CapBroker PD** (kernel/cap_broker.c) | C | seL4 Microkit PD | seL4 PPC (Microkit) | `cap_broker_init`, `cap_broker_register`, `cap_broker_grant`, `cap_broker_revoke`, `cap_broker_check`, `cap_broker_revoke_agent`, `cap_broker_attest`, `cap_broker_policy_reload`, `cap_broker_status`, `cap_broker_handle_policy_reload_ppc` | No | Yes | KEEP |
| **HTTPSvc PD** (kernel/http_svc.c) | C | seL4 Microkit PD | seL4 PPC (Microkit) | `OP_HTTP_REGISTER (0x90)`, `OP_HTTP_UNREGISTER (0x91)`, `OP_HTTP_DISPATCH (0x92)`, `OP_HTTP_LIST (0x93)`, `OP_HTTP_HEALTH (0x94)` | No | Yes | KEEP |
| **AgentFS PD** (kernel/agentfs.c) | C | seL4 Microkit PD | seL4 PPC (Microkit) | `OP_AGENTFS_PUT (0x30)`, `OP_AGENTFS_GET (0x31)`, `OP_AGENTFS_QUERY (0x32)`, `OP_AGENTFS_DELETE (0x33)`, `OP_AGENTFS_VECTOR (0x34)`, `OP_AGENTFS_STAT (0x35)`, `OP_AGENTFS_HEALTH (0x36)` | No | Yes | KEEP |
| **VibeEngine PD** (kernel/vibe_engine.c) | C | seL4 Microkit PD | seL4 PPC (Microkit) | `OP_VIBE_PROPOSE (0x40)`, `OP_VIBE_VALIDATE (0x41)`, `OP_VIBE_EXECUTE (0x42)`, `OP_VIBE_STATUS (0x43)`, `OP_VIBE_ROLLBACK (0x44)`, `OP_VIBE_HEALTH (0x45)`, `OP_VIBE_REGISTER_SERVICE (0x46)`, `OP_VIBE_LIST_SERVICES (0x47)` | No | Yes | KEEP |
| **NameServer PD** (kernel/nameserver.c) | C | seL4 Microkit PD | seL4 PPC (Microkit) | `OP_NS_REGISTER (0xD0)`, `OP_NS_LOOKUP (0xD1)`, `OP_NS_UPDATE_STATUS (0xD2)`, `OP_NS_LIST (0xD3)`, `OP_NS_DEREGISTER (0xD4)`, `OP_NS_HEALTH (0xD5)`, `OP_NS_LOOKUP_GATED (0xD6)` | No | Yes | KEEP |
| **SpawnServer PD** (kernel/spawn_server.c) | C | seL4 Microkit PD | seL4 PPC (Microkit) | `OP_SPAWN_LAUNCH (0xA0)`, `OP_SPAWN_KILL (0xA1)`, `OP_SPAWN_STATUS (0xA2)`, `OP_SPAWN_LIST (0xA3)`, `OP_SPAWN_HEALTH (0xA4)` | No | Yes | KEEP |
| **NetServer PD** (kernel/net_server.c) | C | seL4 Microkit PD | seL4 PPC (Microkit) | `OP_NET_VNIC_CREATE (0xB0)`, `OP_NET_VNIC_DESTROY (0xB1)`, `OP_NET_VNIC_SEND (0xB2)`, `OP_NET_VNIC_RECV (0xB3)`, `OP_NET_BIND (0xB4)`, `OP_NET_CONNECT (0xB5)`, `OP_NET_STATUS (0xB6)`, `OP_NET_SET_ACL (0xB7)`, `OP_NET_HEALTH (0xB8)`, `OP_NET_CONN_STATE (0xB9)`, `OP_NET_TCP_CLOSE (0xBA)`, `OP_NET_HTTP_POST (0x500)` | No | Yes | KEEP |
| **VirtIO Block PD** (include/virtio_blk.h) | C | seL4 Microkit PD | seL4 PPC (Microkit) | `OP_BLK_READ (0xF0)`, `OP_BLK_WRITE (0xF1)`, `OP_BLK_FLUSH (0xF2)`, `OP_BLK_INFO (0xF3)`, `OP_BLK_HEALTH (0xF4)` | No | Yes | KEEP |
| **AppManager PD** (include/app_manager.h) | C | seL4 Microkit PD | seL4 PPC (Microkit) | `OP_APP_LAUNCH (0xC0)`, `OP_APP_KILL (0xC1)`, `OP_APP_STATUS (0xC2)`, `OP_APP_LIST (0xC3)`, `OP_APP_HEALTH (0xC4)` | No | Yes | KEEP |
| **BootIntegrity PD** (include/boot_integrity.h) | C | seL4 Microkit PD | seL4 PPC (Microkit) | `OP_BOOT_MEASURE (0xB0)`, `OP_BOOT_SEAL (0xB1)`, `OP_BOOT_QUOTE (0xB2)`, `OP_BOOT_VERIFY (0xB3)`, `OP_BOOT_RESET (0xB4)` | No | Yes | KEEP |

---

## Per-Service Detail

### 1. capability-broker (userspace/servers/capability-broker/)

**File:** `userspace/servers/capability-broker/src/lib.rs`  
**Language:** Rust (`no_std`, `extern crate alloc`)  
**Layer:** Userspace library — no IPC dispatch, no PD

**Operations:**

Token-based API (for cap delegation between named agents):
- `allow(agent_id, kind)` — line 211: authorize grantor to issue a CapKind
- `issue(grantor, grantee, kind, rights, delegatable) -> Result<GrantToken, BrokerError>` — line 218
- `validate(token, holder, required) -> Result<&Grant, BrokerError>` — line 243
- `delegate(token, current_holder, new_grantee, rights) -> Result<GrantToken, BrokerError>` — line 261
- `revoke(token) -> Result<(), BrokerError>` — line 292
- `grant_count() -> usize` — line 298

Agent+CapKind API (for simpler grant/check):
- `grant(agent_id, cap_kind, rights, expires_at, now_ms) -> Result<(), BrokerError>` — line 311
- `check(agent_id, cap_kind, required, now_ms) -> bool` — line 338
- `check_audited(agent_id, cap_kind, required, now_ms) -> bool` — line 357
- `revoke_by_agent_cap(agent_id, cap_kind, now_ms)` — line 371

Audit:
- `audit_recent(n) -> &[AuditEntry]` — line 405
- `audit_len() -> usize` — line 411

**Status:** Well-tested (14 unit tests), clean Rust, no_std. BUT: this is a library, not a deployed PD. The real kernel-resident capability broker lives in `kernel/agentos-root-task/src/cap_broker.c`. These two implementations have diverged and have different data models.

**Disposition:** REDESIGN — consolidate into the seL4 PD. Keep the Rust lib as a simulation-layer model only, clearly labeled as such. Write a contract file.

---

### 2. event-bus (userspace/servers/event-bus/)

**File:** `userspace/servers/event-bus/src/lib.rs`  
**Language:** Rust (`no_std`, `extern crate alloc`)  
**Layer:** Userspace library — no IPC dispatch, no PD

**Operations:**
- `create_topic(topic)` — line 92
- `create_topic_owned(owner_pd, topic)` — line 98
- `subscribe(topic) -> Result<SubscriberId, BusError>` — line 106
- `unsubscribe(id)` — line 116
- `publish(topic, payload, timestamp_ns) -> Result<usize, BusError>` — line 127
- `publish_as(publisher_pd, topic, payload, timestamp_ns) -> Result<usize, BusError>` — line 141
- `drain(id) -> Result<Vec<RawEvent>, BusError>` — line 176
- `topic_count() -> usize` — line 186

Constants: `MAX_PAYLOAD_BYTES = 512`, `MAX_PENDING_PER_SUBSCRIBER = 256`

**Status:** Well-tested (14 unit tests). Same architectural problem as capability-broker — it is a library, not a PD. The real EventBus PD is at `kernel/agentos-root-task/src/event_bus.c`.

**Disposition:** REDESIGN — same strategy as capability-broker.

---

### 3. http-gateway (userspace/servers/http-gateway/)

**File:** `userspace/servers/http-gateway/src/main.rs`  
**Language:** Rust (std, Tokio, hyper, reqwest, serde_json)  
**Layer:** Standalone binary process (not a seL4 PD, not no_std)

**Operations (HTTP routes it exposes externally):**
- `GET /health` (line 238) — returns `{"status":"ok","routes":N}`
- `GET /healthz` (line 238) — alias for /health
- `GET /_gateway/health` (line 238) — alias
- `GET /_gateway/routes` (line 254) — returns route table JSON
- `ANY *` (line 267–327) — reverse proxy: looks up handler via seL4 IPC bridge, forwards request

**IPC integration:**
- `dispatch_via_http_svc(path)` (line 111) — POSTs `{"path": path}` to `GATEWAY_IPC`/dispatch, expects `{"app_id": N, "addr": "host:port"}` response. Uses opcode constant `OP_HTTP_DISPATCH = 0x92` (line 43) but the actual bridge call is plain HTTP, not seL4 IPC.
- Fallback chain: IPC → `GATEWAY_ROUTE_<APPNAME>` env → `GATEWAY_ROUTES` prefix table

**Configuration (env vars):**
- `GATEWAY_LISTEN` — bind address (default `0.0.0.0:8080`)
- `GATEWAY_IPC` — IPC bridge address (default `127.0.0.1:9090`)
- `GATEWAY_ROUTES` — prefix→upstream map
- `GATEWAY_ROUTE_<NAME>` — per-app static override

**Tested:** No tests in this crate.

**Issues:**
- Listens on `0.0.0.0:8080` by default — publicly exposed
- The "IPC bridge" is itself an HTTP call, not seL4 IPC — the name is a misnomer
- It serves 404 JSON with a "hint" message (`"hint": "register an app via OP_APP_LAUNCH..."`) — this crosses the line into serving advisory content to HTTP clients
- Tokio/hyper/reqwest are appropriate for a Rust binary, but the binary is not a seL4 protection domain
- `OP_HTTP_DISPATCH = 0x92` matches `http_svc.c`'s opcode, but the communication path goes through a TCP bridge, not through actual seL4 PPC

See §Recommendations for the detailed ruling on http-gateway.

---

### 4. model-proxy (userspace/servers/model-proxy/)

**File:** `userspace/servers/model-proxy/src/lib.rs`  
**Language:** Rust (no_std core, optional std HTTP via feature flags)  
**Layer:** Userspace library — no IPC dispatch, no deployed PD

**Operations (public API of the library):**
- `ModelRouter::register_endpoint(endpoint)` — line 342
- `ModelRouter::route(request, budget) -> Option<ModelId>` — line 347
- `ModelRouter::score_model(model, request) -> f32` — line 386 (private)
- `PromptCache::get(request) -> Option<&CachedResponse>` — line 454
- `PromptCache::put(request, response)` — line 471
- `TokenBudget::check(estimated_tokens, model_id) -> bool` — line 290
- `TokenBudget::consume(tokens)` — line 313
- `TokenBudget::reset_period(now)` — line 318

**Backend types supported:**
- `BackendType::HttpApi { endpoint_url, api_key_env, model_name }` — line 88
- `BackendType::LocalGpu { gpu_pd_channel, quantization }` — line 93
- `BackendType::PeerNode { node_id, endpoint_url }` — line 99
- `BackendType::CacheOnly` — line 104
- `BackendType::CodingCli { cli_path }` — line 107

**Tested:** No tests visible in the crate (file exceeds 10K token limit, additional tests may exist beyond offset 500).

**Disposition:** See §Recommendations.

---

### 5. tool-registry (userspace/servers/tool-registry/)

**File:** `userspace/servers/tool-registry/src/lib.rs`  
**Language:** Rust (no_std, extern crate alloc)  
**Layer:** Userspace library — no IPC dispatch, no PD

**Operations:**
- `register(tool: ToolDef) -> Result<u64, ToolStatus>` — line 195: returns badge
- `deregister(name, provider) -> Result<(), ToolStatus>` — line 220
- `unregister(name, provider) -> Result<(), ToolStatus>` — line 233 (alias)
- `grant_access(agent_badge, tool_name)` — line 240
- `invoke(call: &ToolCall, timestamp_ms) -> ToolResult` — line 265
- `list(caller_badge) -> Vec<&ToolDef>` — line 341
- `list_tools(caller_badge) -> Vec<&ToolDef>` — line 348 (alias)
- `search(query, caller_badge) -> Vec<&ToolDef>` — line 353
- `invocation_log_recent(n) -> &[InvocationEntry]` — line 369
- `invocation_log_len() -> usize` — line 374
- `stats() -> RegistryStats` — line 381

Constants: `MAX_TOOLS = 256`, `MAX_INVOCATION_LOG = 512`

**Status:** Well-tested (17 unit tests). Same library-not-PD problem.

---

### 6. vibe-engine (userspace/servers/vibe-engine/)

**Files:**
- `userspace/servers/vibe-engine/src/lib.rs` — Rust lib for proposal lifecycle, contract validation, WASM capability manifest parsing
- `userspace/servers/vibe-engine/src/wasm_validator.rs` — WASM binary validator (magic, exports, section scanning)
- `kernel/agentos-root-task/src/vibe_engine.c` — The real PD with seL4 PPC dispatch

**Kernel PD opcodes (from `vibe_engine.c`, lines 48–55):**
- `OP_VIBE_PROPOSE = 0x40` — submit WASM binary for target service (MR1=service_id, MR2=wasm_size, staging region pre-written)
- `OP_VIBE_VALIDATE = 0x41` — run validation on staged proposal (MR1=proposal_id)
- `OP_VIBE_EXECUTE = 0x42` — approve + trigger swap (MR1=proposal_id)
- `OP_VIBE_STATUS = 0x43` — query proposal or engine state (MR1=proposal_id or 0)
- `OP_VIBE_ROLLBACK = 0x44` — request rollback (MR1=service_id)
- `OP_VIBE_HEALTH = 0x45` — liveness check
- `OP_VIBE_REGISTER_SERVICE = 0x46` — register swappable service
- `OP_VIBE_LIST_SERVICES = 0x47` — list all registered services

**Result codes (vibe_engine.c, lines 59–66):**
`VIBE_OK=0`, `VIBE_ERR_FULL=1`, `VIBE_ERR_BADWASM=2`, `VIBE_ERR_TOOBIG=3`, `VIBE_ERR_NOSVC=4`, `VIBE_ERR_NOENT=5`, `VIBE_ERR_STATE=6`, `VIBE_ERR_VALFAIL=7`, `VIBE_ERR_INTERNAL=99`

**Rust lib operations (lib.rs):**
- `validate_proposal(proposal, contract) -> ValidationResult` — line 218: runs 5 checks (format, size, swappable, memory budget, WASM validity + cap manifest)
- `parse_agentos_caps_section(wasm) -> Result<Option<CapabilityManifest>, String>` — line 363
- `check_capability_policy(manifest, contract) -> CapPolicyResult` — (referenced at line 291)

**wasm_validator.rs required exports:** `init`, `handle_ppc`, `health_check`, `notified`, `memory`  
**Required custom section:** `agentos.capabilities`

**Disposition:** KEEP — this is the most fully realized service in the codebase.

---

### 7. MsgBus (services/msgbus/)

**Files:**
- `services/msgbus/msgbus.c` — implementation
- `services/msgbus/msgbus.h` — interface
- `services/msgbus/MsgBus.camkes` — CAmkES component definition
- `services/msgbus/msgbus_seL4.c` — seL4-specific stubs

**CAmkES interfaces (MsgBus.camkes):**
- `ChannelIface`: `create(name, flags)`, `delete(name)`, `subscribe(name, filter)`, `unsubscribe(name)`, `list() -> (names, count)`
- `MessageIface`: `publish(channel, msg_type, payload, payload_len)`, `send_direct(dest_badge, msg_type, payload, payload_len)`, `recv(channel, timeout_ms) -> (msg_type, payload, payload_len)`
- `RpcIface`: `call(callee_badge, request, req_len, timeout_ms) -> (reply, reply_len)`

**seL4 IPC opcodes (msgbus.h, lines 58–70):**
- `MSGBUS_OP_CREATE_CHANNEL = 0x100`
- `MSGBUS_OP_DELETE_CHANNEL = 0x101`
- `MSGBUS_OP_SUBSCRIBE = 0x102`
- `MSGBUS_OP_UNSUBSCRIBE = 0x103`
- `MSGBUS_OP_PUBLISH = 0x104`
- `MSGBUS_OP_SEND_DIRECT = 0x105`
- `MSGBUS_OP_RECV = 0x106`
- `MSGBUS_OP_LIST_CHANNELS = 0x107`
- `MSGBUS_OP_CHANNEL_INFO = 0x108`
- `MSGBUS_OP_CALL_RPC = 0x109`
- `MSGBUS_OP_REPLY_RPC = 0x10A`

**Well-known system channels:** `system.broadcast`, `system.events`, `system.log`, `system.health`, `tools.registry`, `models.registry`

**Status:** Most IPC dispatch cases in `msgbus_run()` (msgbus.c lines 341–390) are `TODO` stubs — only `MSGBUS_OP_LIST_CHANNELS` sends an actual `seL4_Reply`. The logic is architecturally sound but not yet functional.

**Disposition:** REDESIGN — the channel model and opcodes are correct. The implementation is incomplete (stub IPC). Needs completion and a formal contract file.

---

### 8. CapStore (services/capstore/capstore.c)

**Operations:**
- `capstore_init()` — line 50
- `capstore_register(owner, type, rights, label, &out_id)` — line 58
- `capstore_derive(parent_id, new_owner, rights, &out_id)` — line 80: enforces rights subset rule
- `capstore_revoke(cap_id)` — line 116: cascades to descendants
- `capstore_query_by_owner(owner, &out_ids, &out_count)` — line 154

**Capability types:** `CAP_TYPE_TOOL=0x01`, `CAP_TYPE_MODEL=0x02`, `CAP_TYPE_MEMORY=0x03`, `CAP_TYPE_MSG=0x04`, `CAP_TYPE_STORE=0x05`, `CAP_TYPE_SPAWN=0x06`, `CAP_TYPE_NET=0x07`, `CAP_TYPE_SELF=0x08`, `CAP_TYPE_SERVICE=0x09`

**Status:** Pure C, correct architecture. Internal API only (no IPC dispatch loop present). Max 4096 entries. No tests.

**Disposition:** REDESIGN — needs an IPC dispatch layer and contract file. The `capstore_derive` rights-subset enforcement is correct and should be preserved.

---

### 9. LogSvc (services/logsvc/logsvc.c)

**Operations:**
- `logsvc_init()` — line 57
- `logsvc_write(agent, level, component, message)` — line 71
- `logsvc_writef(agent, level, component, fmt, ...)` — line 92
- `logsvc_query(since_seq, filter_agent, filter_level, &out, &out_count)` — line 107
- `logsvc_entry_to_json(entry, buf, buf_size)` — line 132

**Log levels:** `TRACE=0`, `DEBUG=1`, `INFO=2`, `WARN=3`, `ERROR=4`, `FATAL=5`, `AUDIT=6`, `EVENT=7`

**Capacity:** 16,384 entries ring buffer, 512-byte messages

**Status:** C, no seL4 IPC loop. `logsvc_query` returns results in a static buffer — thread-unsafe if called concurrently. `timestamp_us` is hardcoded to 0 (TODO: seL4 timer). No tests.

**Disposition:** REDESIGN — logic is sound. Needs seL4 IPC dispatch, timer integration, and a contract file.

---

### 10. MemFS (services/memfs/memfs.c)

**Operations:**
- `memfs_init()` — line 42
- `memfs_write(path, data, len)` — line 75: create or overwrite
- `memfs_read(path, out, max_len)` — line 114
- `memfs_list(out_paths, max_entries)` — line 134
- `memfs_delete(path)` — line 150
- `memfs_tag(owner, path, tag)` — line 169
- `memfs_create(owner, path, data, size)` — line 183: legacy, fails if exists

**Limits:** 64 files max, 4KB per file, 256-char paths

**Service ABI opcodes** (from `services/abi/agentos_service_abi.h`, lines 39–45):
- `STORAGE_OP_WRITE = 0x30`
- `STORAGE_OP_READ = 0x31`
- `STORAGE_OP_DELETE = 0x32`
- `STORAGE_OP_STAT = 0x33`
- `STORAGE_OP_LIST = 0x34`
- `STORAGE_OP_STAT_SVC = 0x20`

**Status:** The reference implementation for hot-swappable storage. C, no tests, but designated as the reference. The 4KB/file and 64-file limits make it unsuitable for production but that is by design as a swap-target reference.

**Disposition:** KEEP — this is explicitly the reference implementation for vibe-swap replacement testing.

---

### 11. ModelSvc (services/modelsvc/modelsvc.c)

**Operations:**
- `modelsvc_init()` — line 42
- `modelsvc_register(model_id, endpoint_url, api_key_env, context_window, max_tokens)` — line 55
- `modelsvc_register_defaults()` — line 76: hard-codes NVIDIA NIM + OpenAI endpoints
- `modelsvc_query(requester, model_id, system_prompt, user_prompt, temperature, max_tokens, &response, &tokens_used)` — line 110
- `modelsvc_stats(model_id, &requests, &tokens_in, &tokens_out, &avg_latency_us)` — line 159

**Status:** Stub — `modelsvc_query` never makes an actual HTTP call (prints "HTTP inference not yet implemented"). The cap validation is a `TODO`. No IPC dispatch loop.

**Disposition:** REDESIGN — logic skeleton is valid. Actual HTTP calls need NetServer integration. Needs contract file.

---

### 12. ToolSvc (services/toolsvc/toolsvc.c)

**Operations:**
- `toolsvc_init()` — line 37
- `toolsvc_register(provider, name, desc, input_schema, output_schema, badge)` — line 44
- `toolsvc_unregister(provider, name)` — line 73
- `toolsvc_dispatch(caller, tool_name, input, input_len, &output, &output_len)` — line 88
- `toolsvc_list_json(requester, buf, buf_size)` — line 115: MCP-compatible JSON

**Status:** Stub — `toolsvc_dispatch` logs but doesn't route IPC. Cap validation is `TODO`. No IPC loop.

**Disposition:** REDESIGN — the JSON listing and capability badge routing concept is correct. Needs IPC completion.

---

### 13. vibe-swap (services/vibe-swap/) — VIOLATION

**Files:** `src/index.mjs`, `src/server.mjs`, `test/pipeline.test.mjs`  
**Language:** JavaScript (ES modules, Node.js)  
**Layer:** Node.js HTTP server process

**Operations exposed via HTTP:**
- `POST /stage?slot=N&name=X` — stage a WASM buffer
- `POST /validate?slot=N` — validate staged WASM (magic bytes + exports check)
- `POST /commit?slot=N` — commit staged to active
- `POST /rollback?slot=N` — clear slot
- `GET /status` — return staged/active map

**Constitutional violations:**
1. **JavaScript/Node.js is explicitly forbidden** (CLAUDE.md lines 28–34)
2. **HTTP server exposing operational state** — even if the language were allowed, running an HTTP management server for a kernel service violates the API-first/no-UI mandate
3. **Duplicate of vibe_engine.c + vibe_swap.c** — this functionality exists correctly implemented in C

**Disposition:** DELETE — no logic here that isn't already better expressed in `kernel/agentos-root-task/src/vibe_swap.c` and `vibe_engine.c`. The WASM validation logic (`validate()`) should be confirmed to be covered by `wasm_validator.rs` before deletion (it is: `wasm_validator.rs` checks magic bytes and exports).

---

### 14. EventBus PD (kernel/agentos-root-task/src/event_bus.c)

**Language:** C (Microkit PD)  
**Dispatch:** `protected(ch, msg)` — lines 131–293

**Operations:**
- `MSG_EVENTBUS_INIT (0x0001)` — init ring buffer, emit SYSTEM_READY event
- `MSG_EVENTBUS_SUBSCRIBE (0x0002)` — MR0=notify_channel, MR1=topic_mask → MR0=subscription_handle
- `MSG_EVENTBUS_UNSUBSCRIBE (0x0003)` — MR0=handle
- `MSG_EVENTBUS_STATUS (0x0004)` — MR0=head, MR1=tail, MR2=capacity, MR3=overflow_count
- `OP_PUBLISH_BATCH` — MR0=count (1–16), MR1=byte_offset into shared region; batch publish up to 16 events in one PPC
- Default (any other label) — interpreted as event kind; publishes event to ring and notifies all subscribers

**Ring buffer:** shared memory, 256KB, header + event array + 768B batch staging area  
**Capacity limits:** 64 subscribers max

**Disposition:** KEEP — correct seL4 Microkit PD, ring-buffer IPC, batch optimization documented.

---

### 15. CapBroker PD (kernel/agentos-root-task/src/cap_broker.c)

**Language:** C (Microkit PD, compiled into monitor.elf)  
**Dispatch:** Via `cap_broker_handle_policy_reload_ppc()` called from `monitor.c`

**Operations (via monitor PPC dispatch):**
- `OP_CAP_LOG` — audit log event (MR1=event_type, MR2=agent_pd, MR3=rights, MR4=slot_id)
- `OP_CAP_BROKER_RELOAD (0x15)` — hot-reload policy blob (MR1=blob_size, blob in `cap_policy_shmem_vaddr`)
  - Returns: MR0=status, MR1=grants_checked, MR2=grants_revoked, MR3=policy_version
- `OP_CAP_STATUS` — returns cap_count, policy_version, active_grants (via `cap_broker_status()`)
- `OP_CAP_ATTEST` — generate + store SHA-512 signed attestation report in AgentFS

**Policy blob format (v1 binary, little-endian):**
- `[n_rules: uint32]` + N × `[cap_type(u32), rights_mask(u32), n_agents(u32), agents[8](u32)]` (44 bytes/rule)
- Max 32 rules, max 8 agents per rule

**Attestation report format:** 32-byte header + N×20-byte cap entries + 8-byte net ACL summary + 64-byte SHA-512 signature

**Disposition:** KEEP — attestation and policy hot-reload are well-designed. Needs contract file.

---

### 16. HTTPSvc PD (kernel/agentos-root-task/src/http_svc.c)

**Language:** C (Microkit PD, priority 140)  
**Channels:** `HTTP_CH_CONTROLLER=0`, `HTTP_CH_APP_MANAGER=1`  
**Dispatch:** `protected(ch, msginfo)` — lines 261–278

**Operations:**
- `OP_HTTP_REGISTER (0x90)` — MR1=app_id, MR2=vnic_id, MR3=prefix_len, MR4–MR11=prefix_bytes → MR0=result, MR1=handler_id
- `OP_HTTP_UNREGISTER (0x91)` — MR1=handler_id → MR0=result
- `OP_HTTP_DISPATCH (0x92)` — MR1–MR8=path_bytes (8B/MR, 64B max) → MR0=result, MR1=app_id, MR2=vnic_id, MR3=handler_id
- `OP_HTTP_LIST (0x93)` — writes `http_handler_entry_t[]` to `http_req_shmem` → MR0=result, MR1=count
- `OP_HTTP_HEALTH (0x94)` — MR0=OK, MR1=active_count, MR2=version

**Capacity:** `HTTP_MAX_HANDLERS = 8`, prefix up to 64 bytes  
**Shared memory:** `http_req_shmem` (64KB), layout: `http_req_hdr_t` at offset 0, body at offset 192

**Disposition:** KEEP — this is a correct, lean seL4 IPC routing table. It contains zero HTML, serves no HTTP itself, and is the proper kernel-side counterpart to the http-gateway.

---

### 17. AgentFS PD (kernel/agentos-root-task/src/agentfs.c)

**Language:** C (Microkit PD, priority 150)  
**Channels:** `CH_CONTROLLER=0`, `CH_EVENTBUS=1`

**Operations:**
- `OP_AGENTFS_PUT (0x30)` — write object (MR1=object_kind, MR2=id_lo, MR3=byte_length)
- `OP_AGENTFS_GET (0x31)` — read object by id
- `OP_AGENTFS_QUERY (0x32)` — list/filter objects
- `OP_AGENTFS_DELETE (0x33)` — soft-delete (creates tombstone)
- `OP_AGENTFS_VECTOR (0x34)` — vector similarity query (up to 512-dim embeddings)
- `OP_AGENTFS_STAT (0x35)` — object metadata
- `OP_AGENTFS_HEALTH (0x36)` — health check

**Object model:** 32-byte object IDs (BLAKE3 or UUID), schema_type, version, size, cap_tag. Hot store: 256 objects max, 256KB. Cold tier deferred to external S3-compat via model proxy.

**Disposition:** KEEP — content-addressable store with semantic vector query is sound design.

---

### 18. Other Kernel PDs (brief)

| PD | Source | Key opcodes | Notes |
|---|---|---|---|
| VibeEngine | `vibe_engine.c` | 0x40–0x47 | See §6 above |
| NameServer | `nameserver.c` | 0xD0–0xD6 | Service discovery; shmem dump via OP_NS_LIST |
| SpawnServer | `spawn_server.c` | 0xA0–0xA4 | Loads ELF into pre-allocated app slots |
| NetServer | `net_server.c` | 0xB0–0xBA, 0x500 | vNIC management + high-level HTTP POST proxy via lwIP stub |
| VirtIO Block | `virtio_blk.c` | 0xF0–0xF4 | Block storage via virtio-mmio |
| AppManager | (header only referenced) | 0xC0–0xC4 | Launch/kill/status/list apps |
| BootIntegrity | `boot_integrity.c` | 0xB0–0xB4 | TPM-style measure/seal/quote/verify |
| GpuSched | `gpu_sched.c` | MSG_GPU_SUBMIT (0x0901)+ | GPU task queue |
| MeshAgent | `mesh_agent.c` | MSG_MESH_ANNOUNCE (0x0A01)+ | Distributed node mesh |
| TraceRecorder | `trace_recorder.c` | OP_TRACE_START (0x80)–OP_TRACE_DUMP (0x83) | IPC trace recording |
| WatchdogPD | (referenced in agentos.h) | OP_WD_REGISTER (0x50)–OP_WD_RESUME (0x55) | Heartbeat-based slot watchdog |
| QuotaPD | `quota_pd.c` | MSG_QUOTA_REVOKE (0x0B01) | Per-agent resource quota enforcement |
| FaultHandler | `fault_handler.c` | Receives seL4 fault IPC | VM fault and cap fault processing |
| CryptoIPC | `crypto_ipc.c` | (see crypto_ipc.h) | Cryptographic operations PD |
| JsRuntime | `js_runtime.c` | OP_JS_EVAL (0xC0)–OP_JS_HEALTH (0xC4) | **VIOLATION: JavaScript eval capability in kernel** — see §Violations |

---

## Gaps: Operations Consumers Need That Have No API Today

The following capabilities are architecturally required but have no formal, callable API with a contract file:

1. **Agent identity lookup.** There is no service that maps a badge value to a named agent identity, a 32-byte agent ID, or a set of granted capabilities. The capability broker tracks grants by agent_id strings but there is no IPC API to perform a badge→identity lookup from a protection domain.

2. **Timer and monotonic clock.** `logsvc.c` line 77 (`timestamp_us = 0; /* TODO: seL4 timer */`) and `capability-broker` (`expires_at` timestamps provided by caller) both require a trusted monotonic time source. No time-server PD exists; callers must supply timestamps themselves, which is unsafe for security-critical expiry.

3. **VFS contract.** `spawn_server.c` calls into `vfs_server.c` via channel CH3, but `vfs_server.c` has no published contract. `VFS.h` exists (`include/vfs.h`) but there is no `contracts/vfs/interface.h`.

4. **Agent spawn notification.** The system emits `MSG_EVENT_AGENT_SPAWNED` and `MSG_EVENT_AGENT_EXITED` on EventBus, but there is no contract specifying the payload format, the topic name, or which PD publishes these events.

5. **Capability attestation consumer API.** `cap_broker_attest()` stores a report in AgentFS, but there is no API for an external verifier to retrieve and verify it without direct AgentFS access.

6. **Peer mesh join/leave API.** `mesh_agent.c` handles `MSG_MESH_ANNOUNCE` etc., but the exact wire format for a new node joining the mesh — what goes in MR0–MR3, what the nonce format is, what authentication is required — is not published anywhere outside the source.

7. **GPU task payload format.** `MSG_GPU_SUBMIT` carries `hash_lo, hash_hi, priority, flags` but the format of the actual compute payload (presumably in shared memory) is not documented.

8. **Quota enforcement policy.** The quota PD enforces per-agent limits, but the policy configuration format (how limits are set per agent) has no contract.

---

## Violations

### Hard Violations (must be fixed before any further integration work)

#### V1 — services/vibe-swap/ (JavaScript/Node.js)
- **Files:** `services/vibe-swap/src/index.mjs`, `services/vibe-swap/src/server.mjs`, `services/vibe-swap/test/pipeline.test.mjs`, `services/vibe-swap/package.json`
- **Violation:** JavaScript is explicitly forbidden (CLAUDE.md §Non-Negotiable Language Policy). Node.js HTTP server violates the no-HTTP-servers policy for kernel services.
- **Action:** DELETE all four files and the `services/vibe-swap/` directory. The pipeline logic (stage, validate, commit, rollback) is already correctly implemented in Rust (`userspace/servers/vibe-engine/`) and C (`kernel/agentos-root-task/src/vibe_swap.c`).

#### V2 — kernel/agentos-root-task/src/js_runtime.c (JavaScript eval in kernel PD)
- **File:** `kernel/agentos-root-task/src/js_runtime.c`
- **Opcodes:** `OP_JS_EVAL (0xC0)`, `OP_JS_CALL (0xC1)`, `OP_JS_LOAD_MODULE (0xC2)`, `OP_JS_DESTROY (0xC3)`, `OP_JS_HEALTH (0xC4)`
- **Violation:** A kernel protection domain that evaluates JavaScript is a severe violation of CLAUDE.md §Non-Negotiable Language Policy. Even if the runtime itself is written in C (a JS interpreter like QuickJS), providing the capability to execute JavaScript code inside an OS kernel PD is explicitly against the project constitution and introduces an enormous attack surface.
- **Action:** EVALUATE for deletion. If the use case (dynamic scripting) is genuinely needed, the correct approach is a WASM runtime (wasm3, which is already present), not a JavaScript runtime.

#### V3 — Missing contracts/ directory
- **Violation:** CLAUDE.md §API-First Mandate requires `contracts/<service-name>/interface.h` and `contracts/<service-name>/README.md` for every service. Zero contract files exist. Every service in this codebase is uncallable by the constitution's own rules.
- **Action:** CREATE `contracts/` directory and populate it with interface files for every KEEP-disposition service before any new callers are added.

### Soft Violations (architectural debt, not hard bans)

#### V4 — Duplicate implementations of every major service
- `services/capstore/capstore.c` vs `userspace/servers/capability-broker/src/lib.rs` vs `kernel/agentos-root-task/src/cap_broker.c` — three implementations of capability management
- `services/logsvc/logsvc.c` vs (no Rust counterpart yet) vs `kernel/agentos-root-task/src/log.c`
- `services/toolsvc/toolsvc.c` vs `userspace/servers/tool-registry/src/lib.rs` — two tool registries
- `services/modelsvc/modelsvc.c` vs `userspace/servers/model-proxy/src/lib.rs` — two model inference services
- `services/msgbus/msgbus.c` vs `kernel/agentos-root-task/src/event_bus.c` — two message buses

**Recommended resolution:** The `kernel/` PDs are the canonical implementations. The `services/` C files should be REPLACED by proper seL4 IPC dispatch wrappers around the same logic, or deprecated and replaced by the kernel PDs entirely. The Rust userspace libraries are the simulation/test-layer models and should be explicitly labeled as such in their crate metadata.

#### V5 — http-gateway architectural ambiguity
See §Recommendations below.

---

## Recommendations

### On `userspace/servers/http-gateway/`

**Finding:** The http-gateway is a Rust binary (not JavaScript, not Python) that binds on TCP and proxies HTTP requests to backend apps by querying the seL4 http_svc PD via an intermediate HTTP bridge (not direct seL4 IPC). The binary itself does not serve HTML and does not have a UI.

**The tension:** The gateway does expose management endpoints (`/_gateway/health`, `/_gateway/routes`) and serves JSON to HTTP clients. The 404 response includes a "hint" field with operational guidance. These behaviors are borderline.

**Recommendation: KEEP as a thin seL4 IPC bridge, with the following constraints enforced:**

1. The gateway must NOT grow any HTML, CSS, or human-readable page content. JSON-only responses.
2. The `hint` field in 404 responses (line 341 of `main.rs`) should be removed — it is advisory content directed at human readers, not machine-readable API consumers.
3. The management endpoints (`/_gateway/routes`) should require authentication (a bearer token matching an env var) to prevent route table disclosure to unauthenticated callers.
4. The IPC bridge path (`dispatch_via_http_svc`) should be replaced with direct seL4 PPC once the build infrastructure supports it. The current HTTP-over-HTTP dispatch is an implementation convenience, not the intended architecture.
5. The gateway's Cargo.toml should add `[features] ipc_bridge = []` and the HTTP fallback dispatch should be gated behind `#[cfg(not(feature = "ipc_bridge"))]`.

**The gateway serves a real purpose:** HTTP-speaking external systems (CI, monitoring agents, human operators) need a single ingress point that the OS manages. That is a legitimate OS function. The alternative — exposing every internal app directly — is worse. Keep it, but keep it thin and explicitly restricted to opaque JSON proxying.

### On `userspace/servers/model-proxy/`

**Finding:** Model inference is explicitly called out in the model-proxy source (lines 1–40) as a system service analogous to how POSIX provides `libc` — a centralized, capability-gated, audited, budget-enforced inference layer. The argument is coherent: without OS-level inference, every agent imports its own HTTP client, manages its own API keys, and drains shared GPU capacity without coordination.

**Recommendation: KEEP, with the following constraints:**

1. The model-proxy's primary transport must be seL4 IPC (PPC), not HTTP. The `std` feature that enables reqwest/tokio should only be used in the simulation layer, never in a deployed seL4 PD.
2. The `BackendType::CodingCli { cli_path }` backend (line 107) — which references external CLI tools like `claude`, `codex`, `cursor` — is only appropriate for the development simulation. Remove it from the seL4 PD target.
3. The `BackendType::HttpApi` backend is valid but the actual HTTP calls must go through the **NetServer PD** (using `OP_NET_HTTP_POST = 0x500`), not through a directly-linked HTTP client library in the model-proxy PD itself.
4. A formal `ModelCap` capability type must be defined in `contracts/model-proxy/interface.h` and `cap_type_t` in `capstore.c` must add `CAP_TYPE_MODEL = 0x02` enforcement.

**Does an OS management system need an LLM proxy?** Yes, for the following specific reason: agentOS's defining feature is that agents can propose, validate, and hot-swap OS services (the vibe-coding loop). That loop requires inference. If inference is a per-agent uncoordinated network call, the OS cannot enforce token budgets, cannot cache identical prompts, and cannot audit which agent consumed which model capacity. Centralizing inference under OS control is architecturally correct — it is the same reason POSIX put networking in the kernel rather than having each process manage its own NIC driver.

---

## Priority Action List

In order of severity:

1. **DELETE** `services/vibe-swap/` (JavaScript violation, V1)
2. **EVALUATE/DELETE** `kernel/agentos-root-task/src/js_runtime.c` (JavaScript eval PD, V2)
3. **CREATE** `contracts/` directory; write `interface.h` + `README.md` for the six KEEP-status kernel PDs with the most callers: EventBus, CapBroker, HTTPSvc, AgentFS, VibeEngine, NameServer (V3)
4. **LABEL** the three Rust userspace libraries (capability-broker, event-bus, tool-registry) as simulation-layer models in their `Cargo.toml` descriptions and add a `[features] simulation` guard
5. **IMPLEMENT** missing IPC dispatch loops in `services/msgbus/msgbus.c`, `services/logsvc/logsvc.c`, `services/capstore/capstore.c`, `services/toolsvc/toolsvc.c`
6. **ADD** a time-server or expose seL4 kernel timestamp via a PD so that capability expiry and log timestamps work correctly (Gap #2)
7. **REMOVE** the `hint` field from http-gateway 404 responses and add route-table auth (http-gateway constraint)
8. **ADD** unit tests for the kernel C PDs (cap_broker.c, event_bus.c, http_svc.c) using the simulation layer
