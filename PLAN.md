# agentOS API-First Architecture Refactoring Plan

**Author**: Jordan Hubbard  
**Status**: Active  
**Priority**: P0 — blocking all other feature work

---

## Motivation

agentOS was designed from day one as an OS for agents, not humans. The console/ directory
(a WebSocket bridge and HTML dashboard) is a contradiction of that principle. It treats the OS
as a thing humans watch, rather than a thing agents drive. It must be deleted.

More broadly, the project has accumulated UI scaffolding (console_mux, dev_shell) that encodes
human-facing semantics into kernel-layer protection domains. This is architecturally wrong. The
kernel layer must expose pure, machine-readable IPC contracts. If someone wants a UI, they build
one on top of those contracts as an external project.

This plan transforms agentOS into a properly API-first system: every capability exercisable
via a documented IPC contract, every contract tested, and no UI anywhere in the repository.

---

## Guiding Principles

1. **No UI in this repository.** No HTML, no JavaScript, no ncurses runtime interfaces.
   agentctl is a build/launch tool and is the only exception (see Phase 0).

2. **Every PD is an API endpoint.** Every Protection Domain exposes exactly one IPC contract.
   That contract is the only way to interact with the PD.

3. **API contracts precede implementation.** New message opcodes are defined in `agentos.h`
   before any implementation is written.

4. **seL4 is the only Ring 0 code.** All OS services run as Microkit Protection Domains.
   No service may claim kernel privilege.

5. **Generic before specific.** No guest OS may implement its own device driver until it
   demonstrates in writing (filed as an issue) that the generic device PD cannot serve
   its needs.

6. **Pure assembly, C, and Rust.** No interpreted languages in the core. Python and Node.js
   are external tooling only and must not appear in kernel/, services/, libs/, or userspace/.

7. **Tests are the source of truth.** The test suite in tests/ is the authoritative
   specification of API behavior. A contract not tested is not a contract.

---

## Current State Inventory

### Files that must be deleted (Phase 0)

| Path | Reason |
|------|--------|
| `console/dashboard.html` | HTML UI — violates API-first mandate |
| `console/agentos_console.mjs` | Node.js WebSocket bridge — interpreted language in core |
| `console/package.json` | npm package metadata — no npm in this repo |
| `services/vibe-swap/src/wasm-validator.mjs` | Node.js WASM validator — replace with C/Rust |
| `services/vibe-swap/package.json` | npm package metadata |
| `tools/trace_replay.mjs` | Node.js trace replay — replace with C or Python (tools only) |

### Files that must be refactored (Phase 0)

| Path | Current Role | Target Role |
|------|-------------|-------------|
| `kernel/agentos-root-task/src/console_mux.c` | "tmux for agentOS" with UI semantics | Pure log drain PD: structured ring buffer drain, no attach/detach/scroll/mode semantics |
| `kernel/agentos-root-task/src/dev_shell.c` | Interactive REPL | IPC test harness: exercises PD APIs programmatically, no interactive input |

### Makefile targets to remove (Phase 0)

- `make dashboard` — starts the console bridge
- Console-related QEMU setup in main `make` target
- `CONSOLE_DIR` variable and all references to it

### IPC message opcodes to remove from agentos.h (Phase 0)

The following `agentos_msg_tag_t` values encode UI semantics and must be removed or replaced
with log-drain-appropriate equivalents:

```c
MSG_CONSOLE_ATTACH    // UI concept
MSG_CONSOLE_DETACH    // UI concept
MSG_CONSOLE_LIST      // UI concept
MSG_CONSOLE_MODE      // UI concept: broadcast/exclusive/monitor
MSG_CONSOLE_INJECT    // UI concept: keystroke injection
MSG_CONSOLE_SCROLL    // UI concept
```

Retained (these describe structured data operations, not UI):
```c
MSG_CONSOLE_STATUS    // renamed → MSG_LOG_STATUS
MSG_CONSOLE_WRITE     // renamed → MSG_LOG_WRITE
```

---

## Phase 0: UI Removal

**Goal**: Delete all UI code from the repository. Refactor console_mux and dev_shell to
serve purely programmatic roles.

### 0.1 — Delete the console/ directory

```
git rm -r console/
```

Remove all Makefile references to CONSOLE_DIR, `make dashboard`, and the console
startup sequence in the default `make` target.

**Acceptance criteria**:
- `console/` does not exist in the repository.
- `make` builds and runs agentOS without attempting to start any Node.js process.
- `make dashboard` target is gone (running it must produce a "no such target" error).

### 0.2 — Delete services/vibe-swap JavaScript

```
git rm services/vibe-swap/src/wasm-validator.mjs
git rm services/vibe-swap/package.json
```

WASM validation must be performed in C or Rust. The replacement is tracked in Phase 1
(cap_policy/verify.c already contains Ed25519 + SHA-256; WASM structural validation
must be added there or in a new `wasm_validate.c` PD source).

**Acceptance criteria**:
- No `.mjs` or `.js` files remain anywhere in the repository tree.
- No `package.json` files remain anywhere in the repository tree.
- `grep -r '\.mjs\|\.js\b\|package\.json' --include='*.json' --include='*.mjs' --include='*.js'` returns nothing.

### 0.3 — Delete tools/trace_replay.mjs

```
git rm tools/trace_replay.mjs
```

Trace replay is a debugging tool. If a replay tool is needed it must be written in C
(linking against the trace_recorder PD's output format) or Python (tools/ only, clearly
marked as external tooling). The existing `trace_recorder.c` already records traces in a
binary ring buffer — a C replay tool consuming that format is the correct approach.

**Acceptance criteria**:
- `tools/trace_replay.mjs` does not exist.
- If a replacement trace_replay tool exists, it is a C binary or a Python script.

### 0.4 — Refactor console_mux.c → log_drain.c

`console_mux.c` must be reduced to a pure log drain. Semantics to remove:
- `MSG_CONSOLE_ATTACH` / `MSG_CONSOLE_DETACH` — UI session management
- `MSG_CONSOLE_MODE` — broadcast/exclusive/monitor modes
- `MSG_CONSOLE_INJECT` — keystroke injection
- `MSG_CONSOLE_SCROLL` — scroll position tracking
- `MSG_CONSOLE_LIST` — list of attached sessions

Semantics to retain (renamed):
- `MSG_LOG_WRITE` (was MSG_CONSOLE_WRITE) — a PD writes a structured log record
- `MSG_LOG_STATUS` (was MSG_CONSOLE_STATUS) — query drain status and buffer fill level

The log drain PD:
- Accepts `MSG_LOG_WRITE` messages from any PD via its designated channel.
- Writes log records into a persistent ring buffer in a shared memory region.
- Exposes `MSG_LOG_STATUS` for buffer introspection.
- Does NOT model sessions, modes, or terminals.

**Rename**: `console_mux.c` → `log_drain.c`. Update `agentos.h` channel ID constants
from `CONSOLE_MUX_CH_*` to `LOG_DRAIN_CH_*`. Update all `MSG_CONSOLE_*` opcodes to
`MSG_LOG_*` (only WRITE and STATUS survive; all others are deleted).

The `console_rings_vaddr` extern in `agentos.h` is renamed to `log_drain_rings_vaddr`.
The ring header magic `0xC0DE4D55` remains valid (binary compatibility preserved for
any existing binary that logs; only the C symbol name changes).

**Acceptance criteria**:
- `console_mux.c` does not exist; `log_drain.c` exists in its place.
- `agentos.h` contains no `MSG_CONSOLE_ATTACH/DETACH/LIST/MODE/INJECT/SCROLL` opcodes.
- `agentos.h` contains `MSG_LOG_WRITE` and `MSG_LOG_STATUS`.
- Every PD that previously called `console_log()` still compiles with the renamed symbol.
- The log_drain PD boots and accepts MSG_LOG_WRITE messages.

### 0.5 — Refactor dev_shell.c → ipc_harness.c

`dev_shell.c` must be converted from an interactive REPL to a programmatic IPC test
harness. It:
- Is NOT a runtime PD in production builds.
- IS compiled into debug/test builds.
- On boot, executes a static sequence of IPC calls against configured target PDs.
- Reports pass/fail results via the log drain (MSG_LOG_WRITE).
- Exits cleanly after completing its test sequence.

**Rename**: `dev_shell.c` → `ipc_harness.c`. It must not accept terminal input. It must
not render any output except via MSG_LOG_WRITE.

The ipc_harness is compiled in when `CONFIG_IPC_HARNESS=1` is set in the build. It is
excluded from production images.

**Acceptance criteria**:
- `dev_shell.c` does not exist; `ipc_harness.c` exists in its place.
- `ipc_harness.c` contains no `getchar()`, `fgets()`, `readline()`, or similar input calls.
- `ipc_harness.c` produces output exclusively via `MSG_LOG_WRITE` to the log drain.
- Building with `CONFIG_IPC_HARNESS=0` (the default) excludes it from the image.

---

## Phase 1: API Contract Definition

**Goal**: Every PD in `kernel/agentos-root-task/src/` and `userspace/servers/` has a
documented, machine-verifiable IPC contract. All contracts are defined in `agentos.h`
(opcode layer) and in per-PD contract headers.

### 1.1 — Contract header layout

Each PD gains a contract header at:

```
kernel/agentos-root-task/include/contracts/<pd_name>_contract.h
```

Each contract header defines exactly:
1. Channel IDs used by this PD (cross-referenced to `agentos.h` constants).
2. Message opcodes (cross-referenced to `agentos_msg_tag_t`).
3. Request layout: `struct <pd>_req_<opcode>` for each opcode.
4. Reply layout: `struct <pd>_reply_<opcode>` for each opcode.
5. Error codes: `enum <pd>_error` (PD-local error codes; `0` is always success).
6. Invariants: comments describing preconditions, postconditions, and ordering constraints.

The contract header is the source of truth. The implementation in `<pd>.c` must match it
exactly. The test in `tests/<pd>_test.c` must exercise every opcode.

### 1.2 — PD contract inventory

The following table lists every PD that requires a contract header. "Existing opcodes"
refers to message tags already in `agentos.h`. PDs marked "needs new opcodes" require
additions to `agentos_msg_tag_t`.

| PD | Source File | Existing Opcodes | Needs New Opcodes |
|----|------------|-----------------|-------------------|
| EventBus | event_bus.c | INIT, SUBSCRIBE, UNSUBSCRIBE, PUBLISH_BATCH, STATUS | QUERY_SUBSCRIBERS |
| InitAgent | init_agent.c | START, SHUTDOWN, READY, STATUS | AGENT_LIST |
| VibeEngine | vibe_engine.c | REPLAY, HOTRELOAD, REGISTRY_STATUS, REGISTRY_QUERY | VALIDATE, PROPOSE, COMMIT, ROLLBACK |
| AgentFS | agentfs.c | (none in agentos.h) | READ, WRITE, STAT, LIST, DELETE, SEARCH |
| AgentPool | agent_pool.c | (none) | ALLOC_WORKER, FREE_WORKER, STATUS |
| Worker | worker.c | RETRIEVE, RETRIEVE_REPLY | ASSIGN, REVOKE, STATUS |
| GPUSched | gpu_sched.c | GPU_SUBMIT, GPU_STATUS, GPU_CANCEL, GPU_COMPLETE, GPU_FAILED | (complete) |
| GPUShmem | gpu_shmem.c | (none) | MAP, UNMAP, FENCE, STATUS |
| LinuxVMM | linux_vmm.c | (none in agentos.h) | VM_CREATE, VM_DESTROY, VM_SWITCH, VM_STATUS, VM_LIST |
| FreeBSDVMM | (implicit) | VM_CREATE, VM_DESTROY, VM_SWITCH, VM_STATUS, VM_LIST | (complete) |
| LogDrain | log_drain.c | MSG_LOG_WRITE, MSG_LOG_STATUS | (Phase 0 result) |
| VibeSwap | vibe_swap.c | SWAP_BEGIN, SWAP_ACTIVATE, SWAP_ROLLBACK, SWAP_HEALTH, SWAP_STATUS | (complete) |
| SwapSlot | swap_slot.c | SLOT_READY, SLOT_FAILED, SLOT_HEALTHY | (complete) |
| MeshAgent | mesh_agent.c | MESH_ANNOUNCE, MESH_STATUS, MESH_REMOTE_SPAWN, MESH_HEARTBEAT, MESH_PEER_DOWN | (complete) |
| CapAuditLog | cap_audit_log.c | CAPLOG_LOG, CAPLOG_STATUS, CAPLOG_DUMP, CAPLOG_ATTEST | (complete) |
| QuotaPD | quota_pd.c | QUOTA_REGISTER, QUOTA_TICK, QUOTA_STATUS, QUOTA_SET, CAP_REVOKE | (complete) |
| SnapshotSched | snapshot_sched.c | SNAP_STATUS, SNAP_SET_POLICY, SNAP_FORCE, SNAP_HISTORY | (complete) |
| DebugBridge | debug_bridge.c | (none) | DBG_ATTACH, DBG_DETACH, DBG_BREAKPOINT, DBG_STEP, DBG_READ_MEM |
| FaultHandler | fault_handler.c | (none) | FAULT_NOTIFY, FAULT_QUERY, FAULT_HISTORY |
| MemProfiler | mem_profiler.c | MEMPROF_LEAK_ALERT | MEMPROF_STATUS, MEMPROF_DUMP |
| PerfCounters | perf_counters.c | (none) | PERF_STATUS, PERF_RESET, PERF_DUMP |
| NetIsolator | net_isolator.c | (none) | NET_ALLOW, NET_DENY, NET_STATUS, NET_ACL_DUMP |
| CapBroker | cap_broker.c | (none) | CAP_GRANT, CAP_REVOKE, CAP_STATUS, CAP_LIST |
| Watchdog | watchdog.c | WD_REGISTER, WD_HEARTBEAT, WD_STATUS, WD_UNREGISTER, WD_FREEZE, WD_RESUME | (complete) |
| TraceRecorder | trace_recorder.c | TRACE_START, TRACE_STOP, TRACE_QUERY, TRACE_DUMP | (complete) |
| OOMKiller | oom_killer.c | (none) | OOM_STATUS, OOM_POLICY_SET, OOM_NOTIFY |
| TimePart | time_partition.c | (none) | TPART_ALLOC, TPART_FREE, TPART_STATUS |
| PowerMgr | power_mgr.c | (none) | PWR_STATUS, PWR_DVFS_SET, PWR_SLEEP |
| IPCHarness | ipc_harness.c | (test-only PD) | TEST_RUN, TEST_STATUS, TEST_RESULT |
| Verify | verify.c | (library, not a PD) | N/A — inline API only |

**Acceptance criteria for Phase 1**:
- Every PD in the table above has a `_contract.h` file in `include/contracts/`.
- Every new opcode is added to `agentos_msg_tag_t` in `agentos.h`.
- Every request/reply struct is defined in the contract header, not in the PD source.
- `grep -r 'microkit_msginfo_new\|microkit_call\|microkit_reply' kernel/` finds no opcode
  literal integers — all opcodes are referenced by their named constants from `agentos.h`.

### 1.3 — Remove the console contract

The following opcodes are removed from `agentos_msg_tag_t` as part of this phase
(continuation from Phase 0.4):

```c
MSG_CONSOLE_ATTACH
MSG_CONSOLE_DETACH
MSG_CONSOLE_LIST
MSG_CONSOLE_MODE
MSG_CONSOLE_INJECT
MSG_CONSOLE_SCROLL
```

Any `switch` statement in any PD source that references these opcodes must be updated.

---

## Phase 2: Generic Device Layer

**Goal**: Establish OS-neutral Protection Domains for every hardware device class.
Guest OSes must bind to these PDs. No guest OS may implement its own device driver
until it proves the generic PD cannot serve its needs.

### Device PD Taxonomy

| Device Class | PD Name | Source File | IPC Contract |
|-------------|---------|-------------|-------------|
| Serial I/O | `serial_pd` | `serial_pd.c` | `contracts/serial_contract.h` |
| Network | `net_pd` | `net_pd.c` | `contracts/net_contract.h` |
| Block/Disk | `block_pd` | `block_pd.c` | `contracts/block_contract.h` |
| USB | `usb_pd` | `usb_pd.c` | `contracts/usb_contract.h` |
| Timer/RTC | `timer_pd` | `timer_pd.c` | `contracts/timer_contract.h` |
| Interrupt controller | `irq_pd` | `irq_pd.c` | `contracts/irq_contract.h` |

Each device PD:
- Owns the hardware resource exclusively (seL4 device frame capability).
- Exposes an OS-neutral API via IPC (no guest OS specifics leak into the API).
- Serves multiple client PDs simultaneously via a capability-gated client table.
- Is NOT a VirtIO device (VirtIO is a guest-facing transport; the generic PD is the backend).

### 2.1 — Serial device PD (serial_pd)

The current system uses raw `dbg_puts` for output, bypassing all IPC. `serial_pd` replaces
this with a proper IPC-based serial service.

Opcodes:
```c
MSG_SERIAL_OPEN       // claim a serial port (returns client slot)
MSG_SERIAL_CLOSE      // release slot
MSG_SERIAL_WRITE      // write bytes (up to 256 bytes per message)
MSG_SERIAL_READ       // read bytes (returns available count)
MSG_SERIAL_STATUS     // baud rate, rx/tx counts, error flags
MSG_SERIAL_CONFIGURE  // set baud rate, parity, flow control
```

`dbg_puts` is only used before serial_pd is initialized (early boot). After `serial_pd`
reports ready via EventBus (MSG_TYPE_SYSTEM_READY), all serial output goes through IPC.

### 2.2 — Network device PD (net_pd)

The current `lwIP-based NetStack` is a direct-access model. `net_pd` provides the
OS-neutral abstraction.

Opcodes:
```c
MSG_NET_OPEN          // claim a network interface (returns handle)
MSG_NET_CLOSE         // release handle
MSG_NET_SEND          // transmit a packet (up to 1514 bytes via shared MR)
MSG_NET_RECV          // poll for received packet (returns length, zero-copy)
MSG_NET_STATUS        // link status, MAC address, rx/tx statistics
MSG_NET_CONFIGURE     // set promiscuous mode, MTU, etc.
MSG_NET_FILTER_ADD    // add packet filter rule
MSG_NET_FILTER_REMOVE // remove packet filter rule
```

Guest OSes receive packets via a dedicated shared memory ring (one ring per guest).
The net_pd DMA-maps the ring into the guest VMM's address space; the guest maps it
into the guest OS via the VMM's VirtIO-net backend.

### 2.3 — Block device PD (block_pd)

Opcodes:
```c
MSG_BLOCK_OPEN        // claim a block device partition
MSG_BLOCK_CLOSE       // release
MSG_BLOCK_READ        // read N sectors starting at LBA (via shared MR)
MSG_BLOCK_WRITE       // write N sectors (via shared MR)
MSG_BLOCK_FLUSH       // flush write-back cache
MSG_BLOCK_STATUS      // sector count, sector size, read-only flag
MSG_BLOCK_TRIM        // TRIM/UNMAP for SSD lifetime
```

### 2.4 — USB device PD (usb_pd)

Opcodes:
```c
MSG_USB_ENUMERATE     // trigger device enumeration
MSG_USB_LIST          // list attached devices (class, VID, PID)
MSG_USB_OPEN          // open device by index (returns handle)
MSG_USB_CLOSE         // release handle
MSG_USB_CONTROL       // submit CONTROL transfer
MSG_USB_BULK_IN       // submit BULK IN transfer (via shared MR)
MSG_USB_BULK_OUT      // submit BULK OUT transfer (via shared MR)
MSG_USB_STATUS        // transfer status, error codes
```

### 2.5 — Generic device mandate enforcement

Before any guest OS PD may implement its own driver for any device class listed in §2,
the proposer must:

1. File a GitHub issue titled: `[device-waiver] <guest_os> requires <device_class>`.
2. The issue must document: (a) what the generic PD cannot do, (b) what API extension
   would make the generic PD sufficient, (c) why the extension cannot be made to the
   generic PD.
3. The issue must be approved by the project owner before any guest-specific driver code
   is written.

Pull requests that introduce guest-specific drivers without an approved waiver issue will
be rejected.

**Acceptance criteria for Phase 2**:
- All four device PD source files exist with their contract headers.
- All device opcodes are in `agentos_msg_tag_t`.
- The FreeBSD VMM and Linux VMM bind to these device PDs (not direct hardware).
- `grep -r 'dbg_puts' kernel/` finds only pre-serial_pd boot code in `monitor.c` and
  `init_agent.c` (boot banner only).

---

## Phase 3: Guest OS Interface Contract

**Goal**: Define the formal contract between agentOS and any guest OS running as a VMM PD.
This contract governs how guest OSes discover, bind to, and use generic device PDs.

### 3.1 — Guest OS binding protocol

A guest OS PD must perform the following sequence at boot:

1. **Announce to EventBus**: Send `MSG_EVENTBUS_SUBSCRIBE` for `EVENT_GUEST_READY`.
2. **Query device PDs**: Send `MSG_AGENTFS_STAT` for the `/devices` namespace to discover
   available device PD endpoints. (AgentFS serves as the service registry.)
3. **Open device handles**: For each required device, send the appropriate `MSG_<DEVICE>_OPEN`
   to the device PD. The device PD validates the guest's capability badge before granting a handle.
4. **Register with QuotaPD**: Send `MSG_QUOTA_REGISTER` with the guest OS's PD ID and
   resource requirements (CPU budget, memory ceiling, network bandwidth).
5. **Publish ready event**: Send `MSG_EVENTBUS_PUBLISH_BATCH` with `EVENT_GUEST_READY`
   containing the guest OS ID and bound device handles.

### 3.2 — Guest OS contract header

```
kernel/agentos-root-task/include/contracts/guest_contract.h
```

Defines:
- `struct guest_capabilities` — the full set of device handles a guest OS may hold.
- `struct guest_resource_limits` — CPU, memory, net, disk quotas.
- `enum guest_error` — error codes for guest binding failures.
- Invariant: A guest OS may not submit IPC to a device PD for which it does not hold
  a valid handle (enforced by the device PD's client table).

### 3.3 — VMM PD requirements

All VMM PDs (`linux_vmm.c`, `freebsd_vmm.c`, future VMMs) must:
- Include `contracts/guest_contract.h`.
- Call `agentos_log_boot("vmm_<os_name>")` on init.
- Complete the binding protocol in §3.1 before running any guest code.
- Expose a VirtIO transport layer *backed by* the generic device PDs.
- Not implement any device class for which a generic device PD exists (see Phase 2 mandate).

### 3.4 — Guest OS creation/teardown via IPC

Guest OS lifecycle must be fully API-driven. A caller with the `SpawnCap` capability can:

```c
MSG_VM_CREATE   // create a new guest OS slot (returns VM handle)
MSG_VM_DESTROY  // destroy guest OS slot and release all resources
MSG_VM_SWITCH   // switch active console output to this guest
MSG_VM_STATUS   // query guest state (CREATING, RUNNING, PAUSED, DEAD)
MSG_VM_LIST     // enumerate all guest slots
```

These opcodes already exist in `agentos.h` for the FreeBSD VMM. They must be made generic
and moved to a shared `contracts/vmm_contract.h` that all VMM PDs include.

**Acceptance criteria for Phase 3**:
- `contracts/guest_contract.h` exists and defines the complete binding protocol types.
- `contracts/vmm_contract.h` exists and defines the generic VM lifecycle opcodes.
- `linux_vmm.c` and any FreeBSD VMM source include `contracts/guest_contract.h`.
- A guest OS that skips any step of the binding protocol fails with a documented error code.
- The binding protocol is tested in `tests/guest_binding_test.c`.

---

## Phase 4: VibeOS API

**Goal**: Creating, destroying, and managing entire OS stacks must be possible entirely
via API. A caller with appropriate capabilities must be able to go from zero to a running
OS with devices, network, and storage using only IPC messages.

### 4.1 — VibeOS lifecycle API

The VibeOS API is the top-level OS management API. It composes Phase 2 (device PDs),
Phase 3 (guest binding), and the existing VibeEngine hot-swap mechanism.

New opcodes to add to `agentos_msg_tag_t`:

```c
// VibeOS lifecycle (top-level OS management)
MSG_VIBEOS_CREATE         // create a new OS stack (returns vibeos_handle)
MSG_VIBEOS_DESTROY        // destroy OS stack and all PDs
MSG_VIBEOS_STATUS         // query OS stack state
MSG_VIBEOS_LIST           // list all OS stacks
MSG_VIBEOS_BIND_DEVICE    // attach a device PD to this OS stack
MSG_VIBEOS_UNBIND_DEVICE  // detach a device PD
MSG_VIBEOS_SNAPSHOT       // checkpoint OS stack state
MSG_VIBEOS_RESTORE        // restore from checkpoint
MSG_VIBEOS_MIGRATE        // live-migrate OS stack to another node (mesh)
```

A `vibeos_handle` is a 32-bit opaque identifier scoped to the current agentOS instance.

### 4.2 — VibeOS creation flow

`MSG_VIBEOS_CREATE` takes a `struct vibeos_create_req`:

```c
struct vibeos_create_req {
    uint8_t  os_type;           // VIBEOS_TYPE_FREEBSD, VIBEOS_TYPE_LINUX, etc.
    uint8_t  arch;              // VIBEOS_ARCH_AARCH64, VIBEOS_ARCH_X86_64
    uint32_t ram_mb;            // guest RAM in megabytes
    uint32_t cpu_budget_us;     // MCS scheduling budget per period
    uint32_t cpu_period_us;     // MCS scheduling period
    uint32_t device_flags;      // bitmask: VIBEOS_DEV_NET | VIBEOS_DEV_BLOCK | etc.
    uint8_t  wasm_hash[32];     // optional: initial vibe-coded service to hot-load
};
```

The VibeEngine PD executes this sequence:
1. Allocate a swap slot from the swap slot pool.
2. Configure the slot's VMM PD with the requested arch/RAM.
3. Wire device PD handles based on `device_flags`.
4. Execute the guest OS binding protocol (Phase 3).
5. If `wasm_hash` is non-zero, hot-load the specified WASM service via the vibe-swap mechanism.
6. Publish `EVENT_VIBEOS_READY` to the EventBus.
7. Return the `vibeos_handle` to the caller.

### 4.3 — VibeOS contract header

```
kernel/agentos-root-task/include/contracts/vibeos_contract.h
```

Defines:
- `struct vibeos_create_req` / `struct vibeos_create_reply`
- `struct vibeos_status_req` / `struct vibeos_status_reply`
- `struct vibeos_device_bind_req`
- `struct vibeos_snapshot_req` / `struct vibeos_snapshot_reply`
- `enum vibeos_type` — OS type identifiers
- `enum vibeos_state` — CREATING, BOOTING, RUNNING, PAUSED, DEAD, MIGRATING
- `enum vibeos_error` — error codes

### 4.4 — agentctl VibeOS integration

`tools/agentctl/agentctl.c` is a build and launch tool. It must NOT be extended with
a runtime management UI. However, it may be extended with command-line subcommands that
drive the VibeOS API:

```
agentctl vibeos create --type freebsd --arch aarch64 --ram 512
agentctl vibeos list
agentctl vibeos status <handle>
agentctl vibeos destroy <handle>
```

These subcommands translate to IPC messages via a thin agentctl client library. They
are CLI tools, not a UI — they print structured output (JSON or tab-separated) to stdout
and exit. No curses, no interactive menus for VibeOS management.

**Acceptance criteria for Phase 4**:
- `contracts/vibeos_contract.h` exists with all types defined.
- `MSG_VIBEOS_*` opcodes are in `agentos_msg_tag_t`.
- `vibe_engine.c` implements `MSG_VIBEOS_CREATE` end-to-end.
- A FreeBSD guest OS can be created, run, and destroyed using only API calls.
- `agentctl vibeos create` successfully creates a guest OS (verified in CI).
- The full lifecycle is tested in `tests/vibeos_lifecycle_test.c`.

---

## Phase 5: Test Suite

**Goal**: Every API contract is tested. Every IPC opcode is exercised. The test suite
runs in CI on every commit and is the authoritative specification of correct behavior.

### 5.1 — Test directory structure

```
tests/
  contracts/
    eventbus_test.c
    init_agent_test.c
    vibe_engine_test.c
    agent_fs_test.c
    agent_pool_test.c
    worker_test.c
    gpu_sched_test.c
    log_drain_test.c
    vibe_swap_test.c
    mesh_agent_test.c
    cap_audit_log_test.c
    quota_pd_test.c
    snapshot_sched_test.c
    debug_bridge_test.c
    fault_handler_test.c
    mem_profiler_test.c
    perf_counters_test.c
    net_isolator_test.c
    cap_broker_test.c
    watchdog_test.c
    trace_recorder_test.c
    oom_killer_test.c
    serial_pd_test.c
    net_pd_test.c
    block_pd_test.c
    usb_pd_test.c
  integration/
    guest_binding_test.c    // Phase 3 binding protocol
    vibeos_lifecycle_test.c // Phase 4 OS creation/destroy
    vibe_hotswap_test.c     // end-to-end hot-swap
    mesh_spawn_test.c       // remote spawn via mesh
  harness/
    test_runner.c           // boots ipc_harness.c, collects results via log_drain
    test_framework.h        // ASSERT_IPC_OK(), ASSERT_REPLY_EQ(), etc.
```

### 5.2 — Test framework

`tests/harness/test_framework.h` provides macros that map to seL4 Microkit IPC calls:

```c
#define ASSERT_IPC_OK(ch, msg)         // send msg on ch, assert reply tag == SUCCESS
#define ASSERT_IPC_ERR(ch, msg, err)   // send msg on ch, assert reply tag == err
#define ASSERT_REPLY_U32(ch, msg, val) // send msg, assert reply mr0 == val
#define TEST_BEGIN(name)               // emit MSG_LOG_WRITE "TEST BEGIN <name>"
#define TEST_PASS(name)                // emit MSG_LOG_WRITE "TEST PASS <name>"
#define TEST_FAIL(name, reason)        // emit MSG_LOG_WRITE "TEST FAIL <name>: <reason>"
```

All test output goes through MSG_LOG_WRITE to the log drain. The test runner
(`test_runner.c`) reads the log drain ring buffer and counts PASS/FAIL entries.

### 5.3 — CI integration

`make test` must:
1. Build agentOS with `CONFIG_IPC_HARNESS=1`.
2. Boot in QEMU.
3. Wait for `EVENT_SYSTEM_READY` on the log drain (timeout: 30 seconds).
4. Run all contract tests via the ipc_harness PD.
5. Parse test results from the log drain ring buffer.
6. Exit 0 if all tests pass, non-zero otherwise.
7. Print a human-readable summary to stdout.

CI runs `make test` on both `TARGET_ARCH=aarch64` and `TARGET_ARCH=x86_64`.

### 5.4 — Test coverage requirements

A pull request may not be merged if it:
- Adds a new IPC opcode without a corresponding test in `tests/contracts/`.
- Modifies an existing IPC contract without updating the corresponding test.
- Reduces the number of passing tests.

**Acceptance criteria for Phase 5**:
- `make test TARGET_ARCH=aarch64` exits 0 with all tests passing.
- `make test TARGET_ARCH=x86_64` exits 0 with all tests passing.
- Every opcode in `agentos_msg_tag_t` is exercised by at least one test.
- A PR that adds an untested opcode fails CI.

---

## Implementation Order and Dependencies

```
Phase 0 (no deps)     → can begin immediately
Phase 1 (needs Ph.0)  → contract headers require opcode cleanup
Phase 2 (needs Ph.1)  → device PDs need contract framework
Phase 3 (needs Ph.2)  → guest binding uses device PD contracts
Phase 4 (needs Ph.3)  → VibeOS composes guest + device APIs
Phase 5 (runs all)    → tests require all contracts to be defined
```

Phase 5 test stubs should be written in parallel with Phase 1 (write failing tests first,
then implement the contracts). This is the required development order for all new PD work
going forward.

---

## Files Touched Summary

### Deleted
- `console/dashboard.html`
- `console/agentos_console.mjs`
- `console/package.json`
- `services/vibe-swap/src/wasm-validator.mjs`
- `services/vibe-swap/package.json`
- `tools/trace_replay.mjs`

### Renamed
- `kernel/agentos-root-task/src/console_mux.c` → `log_drain.c`
- `kernel/agentos-root-task/src/dev_shell.c` → `ipc_harness.c`

### Modified
- `kernel/agentos-root-task/include/agentos.h` — opcode additions/removals (see §1.2, §0.4)
- `Makefile` — remove dashboard target and CONSOLE_DIR
- `CMakeLists.txt` — add new PD source files, remove console_mux/dev_shell references

### Created
- `kernel/agentos-root-task/include/contracts/<pd>_contract.h` (one per PD, ~30 files)
- `kernel/agentos-root-task/src/serial_pd.c`
- `kernel/agentos-root-task/src/net_pd.c`
- `kernel/agentos-root-task/src/block_pd.c`
- `kernel/agentos-root-task/src/usb_pd.c`
- `tests/contracts/<pd>_test.c` (one per PD)
- `tests/integration/guest_binding_test.c`
- `tests/integration/vibeos_lifecycle_test.c`
- `tests/harness/test_runner.c`
- `tests/harness/test_framework.h`
