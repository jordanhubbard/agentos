# agentOS — Project Constitution for AI Agents

This document is **binding**. Every agent working in this repository must read and follow these rules before writing, deleting, or proposing any code. These rules exist because the humans running this project have learned what belongs here and what does not. Do not make exceptions. Do not ask whether a rule applies — if it plausibly applies, it does.

---

## What agentOS Is

agentOS is a bootable, capability-secured operating system built on the seL4 formally-verified microkernel. It hosts entire operating system stacks (Linux, FreeBSD, and future guests) as a cooperating set of isolated protection domains. Its primary interface is an API — not a UI. Its primary value is OS lifecycle management (create, configure, destroy, migrate, snapshot operating systems on demand), strict capability-based isolation, and the vibeOS interface for on-demand OS instantiation.

**agentOS is not a framework. It is not a web app. It is not a dashboard. It boots on bare metal.**

---

## Non-Negotiable Language Policy

agentOS is a **pure C + Rust + Assembly** project. Period.

### Allowed
- **C** — kernel root task, CAmkES components, seL4 services, device drivers, low-level runtime
- **Rust** — userspace agents, SDK, build tools, the simulator (no_std where appropriate)
- **Assembly** — boot code, low-level stubs, architecture-specific routines
- **CAmkES / CDL** — system topology and IPC interface descriptions
- **CMake / Make** — build orchestration only (no logic, no code generation)
- **WASM (as a guest binary format only)** — agents may emit WASM for the vibe-engine to validate and hot-swap; WASM runtime code is in C (wasm3) or Rust. WASM is never used for system services themselves.

### Forbidden — No Exceptions
- **JavaScript / TypeScript / Node.js** — none, ever, for any purpose
- **Python** — none, ever, for any purpose
- **Go, Zig, or any other language** — not unless approved by the project owner in writing
- **Shell scripts as primary logic** — shell is only for CI glue and one-liner build wrappers
- **HTML, CSS, React, Vue, Svelte, or any browser technology** — see UI Policy below

If you find an existing file in a forbidden language, **do not use it or extend it — file a task to delete it or rewrite it in C or Rust**.

---

## UI Policy — No User Interface Code

agentOS has **no user interface** and must never have one.

- No web dashboards, admin panels, or browser frontends
- No HTML, CSS, or JavaScript served by any process in this repo
- No WebSocket servers that serve terminal emulators to browsers
- No React, xterm.js, WASM frontends, or similar

**Rationale:** If a human or system wants to observe or control agentOS, they use the API. They write their own UI on top of the API contracts exposed by agentOS services. agentOS has no responsibility for how that UI looks or works. Mixing UI concerns into an OS kernel project pollutes scope, introduces forbidden languages, and creates maintenance debt with no upside.

**If you find console/ or similar UI directories:** delete them and remove their Cargo workspace entries.

---

## Ring Model — seL4 Is Ring 0, Nothing Else

The seL4 microkernel is the **only** component that runs in kernel mode (Ring 0 / EL1 on ARM). Everything else runs in user mode as a protection domain (PD).

### The Protection Domain Hierarchy

```
Ring 0:  seL4 microkernel (formally verified, never modified)
Ring 1:  agentOS root task / init (resource distributor, no policy)
Ring 2:  System services (net, disk, serial, memory, capability broker)
Ring 3:  Guest OS VMMs (linux_vmm, freebsd_vmm, future guest VMMs)
Ring 4:  Guest OS userspace (runs inside the VMM's address space)
Ring 5:  Agents / applications
```

Rules:
1. **No code in seL4 itself** — the kernel source is an external submodule. Never modify it.
2. **Root task does not enforce policy** — it distributes initial capabilities. Policy is the capability broker's job.
3. **System services do not trust each other by default** — they communicate via seL4 IPC with capability verification.
4. **VMMs run in Ring 3** — they have no direct hardware access except via capabilities explicitly granted by the root task or an authorized service.
5. **No PD may escalate privileges** — capabilities are monotonically decreasing in rights as they are delegated down the hierarchy.

---

## API-First Mandate

**Every seL4 service must expose a formal, documented interface contract before it may be used by anything.**

### What a Contract Must Include

1. **Service name and protection domain** — which PD implements this service
2. **IPC endpoint description** — badge value (or discovery mechanism), message size, reply size
3. **Operation table** — every supported opcode: name, input structure (C typedef), output structure, error codes
4. **Capability requirements** — what capabilities the caller must hold to invoke each operation
5. **Version** — a monotonically increasing integer; increment when the wire format changes
6. **Stored location** — `contracts/<service-name>/interface.h` (C header with structs + opcodes) and `contracts/<service-name>/README.md` (prose description)

### Rules

- A service with no contract in `contracts/` **must not be called by anything**. Add a contract before adding a caller.
- Contracts are defined using **C structs and `#define` constants**, not JSON, YAML, or prose.
- All operation structs must be `__attribute__((packed))` and have explicit `uint32_t` opcode fields.
- No implicit contracts. No "see the source code" contracts. No verbal contracts.
- Breaking a contract requires a version bump and migration path documented in `contracts/<service-name>/CHANGELOG.md`.

---

## Generic Device Rule — Prove Uniqueness Before Specializing

**Before any guest OS, VMM, or agent may implement its own version of a standard device type, it must formally prove that no generic implementation exists or can be enhanced to meet its requirements.**

### Standard Generic Device Types

The following device types MUST have exactly one canonical implementation in `services/`:

| Device Type   | Canonical Service PD   | Contract Location              |
|---------------|------------------------|-------------------------------|
| Serial / UART | `serial-mux`           | `contracts/serial-mux/`       |
| Network       | `net-service`          | `contracts/net-service/`      |
| Block / Disk  | `block-service`        | `contracts/block-service/`    |
| USB           | `usb-service`          | `contracts/usb-service/`      |
| Timer         | `timer-service`        | `contracts/timer-service/`    |
| Entropy / RNG | `entropy-service`      | `contracts/entropy-service/`  |

### Process for Custom Implementations

If a guest OS needs a device type not listed above, or claims the generic service is inadequate:

1. **File a task** titled "DEFECT: `<guest-os>` cannot use generic `<device-type>` because `<reason>`"
2. **Document in the task** exactly what the generic service cannot do and why enhancement is impossible
3. **Get written approval** from the project owner (Jordan Hubbard) before implementing a custom version
4. **Place the custom implementation** in `services/<guest-os>/<device-type>/` with a clear comment in its header: `/* Custom implementation. See defect: <task-id>. Generic service is inadequate because: <reason>. */`

Any custom device implementation without a corresponding approved defect task is **a bug** and must be deleted or replaced.

---

## vibeOS Interface — The Primary External API

The `vibeOS` interface is the **primary way any external consumer interacts with agentOS**. It provides OS lifecycle management via seL4 IPC, callable from any language via a C FFI.

### Required Operations

Every operation listed here MUST exist, be documented in `contracts/vibeos/interface.h`, and have a test in `tests/`:

| Operation        | Description                                             |
|------------------|---------------------------------------------------------|
| `VOS_CREATE`     | Instantiate a new guest OS from a spec struct           |
| `VOS_DESTROY`    | Tear down a guest OS and reclaim all capabilities       |
| `VOS_STATUS`     | Query state, bound services, resource usage             |
| `VOS_LIST`       | Enumerate all running OS instances                      |
| `VOS_ATTACH`     | Bind a running OS to a generic device service           |
| `VOS_DETACH`     | Unbind from a generic device service                    |
| `VOS_SNAPSHOT`   | Checkpoint OS state to storage                          |
| `VOS_RESTORE`    | Restore a previously snapshotted OS                     |
| `VOS_MIGRATE`    | Move an OS instance between capability domains          |
| `VOS_CONFIGURE`  | Modify OS parameters without destroying/recreating      |

The API is pure C structs over seL4 IPC. No HTTP. No JSON. No YAML. A `vos_spec_t` C struct describes the OS to create. The caller fills it in, calls the IPC endpoint, gets back a `vos_handle_t` or an error code.

### vibeOS and the Vibe-Engine

The vibe-engine (`userspace/servers/vibe-engine/` and `kernel/agentos-root-task/src/vibe_engine.c`) handles **service-level hot-swap** — an agent proposes a WASM component, the engine validates and installs it. This is distinct from vibeOS (OS lifecycle). Both must coexist. The vibe-engine's hot-swap protocol must itself expose a contract in `contracts/vibe-engine/`.

---

## Test Policy — No Untested APIs

**No seL4 service may be merged without corresponding tests. No vibeOS operation may be merged without an integration test.**

### Test Infrastructure

- **Unit tests**: `tests/*.c` — C test suites for individual services
- **Integration tests**: `tests/integration/` — end-to-end tests using the Rust simulator (`userspace/sim/`)
- **API coverage tests**: `tests/api/` — one test file per service contract, one test function per opcode
- **Simulator tests** run without seL4 hardware and must pass in CI
- **Hardware tests** require a QEMU or physical board; marked with `#[cfg(feature = "hardware")]` or `HARDWARE_TEST` guard

### Rules

- Every opcode in every contract must have at least one test
- Tests must produce machine-readable output (`TAP` format preferred)
- The `xtask test` command must run all simulator tests and report pass/fail per API endpoint
- A test that exercises a code path but does not assert on the return value or output is **not a test** — it is a fire drill

---

## What Goes in This Repository

This repository is exclusively for:

1. **seL4 root task** — `kernel/agentos-root-task/`
2. **CAmkES system services** — `services/` — each a seL4 PD implementing a specific system function
3. **Interface contracts** — `contracts/` — C headers + README per service
4. **Generic device services** — `services/serial-mux/`, `services/net-service/`, `services/block-service/`, etc.
5. **Guest OS VMMs** — `kernel/agentos-root-task/src/linux_vmm.c`, `freebsd-vmm/`, future VMMs
6. **vibeOS lifecycle API and implementation** — wherever it lives, it must have a contract
7. **Vibe-engine (WASM hot-swap)** — `userspace/servers/vibe-engine/`
8. **agentOS SDK** — `userspace/sdk/`, `libs/` — the C and Rust libraries agents use
9. **Build and code-generation tools** — `tools/` — written in Rust or C
10. **Simulator** — `userspace/sim/` — in-memory seL4 simulation for testing
11. **Test suite** — `tests/` — all tests, written in C or Rust
12. **Documentation** — `docs/`, `DESIGN.md`, `contracts/*/README.md`

## What Must Never Be in This Repository

- **Any UI code** — see UI Policy above
- **Any JavaScript or TypeScript** — see Language Policy above
- **Any Python** — see Language Policy above
- **Any Node.js project** — this includes `package.json`, `node_modules/`, `.nvmrc`
- **Any cloud provider SDK** — no AWS SDK, no GCP SDK, no Azure SDK
- **Any LLM provider SDK used directly** — if agentOS needs model inference, it goes through `ModelSvc` (a CAmkES PD), not a Rust crate that calls the OpenAI API
- **Any web framework** — no axum serving HTML, no actix-web, no warp serving browser content
- **Any mobile code** — agentOS runs on servers and embedded hardware, not phones
- **Monitoring agents or observability SaaS integrations** — agentOS has its own audit log (`LogSvc`)
- **Any mocking of seL4 IPC for tests** — use the simulator (`userspace/sim/`) instead

---

## Working With Existing Code That Violates These Rules

If you open a file and discover it violates these rules:

1. **Do not extend the violation** — don't add more JS to an existing JS file, don't add more HTML to an existing HTML template
2. **File a task** — "Remove violation: `<path>` is `<language>` which violates language policy"
3. **If the file is in `console/`** — delete it; the whole directory is slated for removal
4. **If the file is a system service implemented in the wrong language** — the service logic must be rewritten in C or Rust before the file is deleted; the new implementation must have a contract
5. **If the file is test infrastructure in a forbidden language** — rewrite in C or Rust; the test suite is not exempt from language policy

---

## Checklist Before Merging Any Code

- [ ] All new/modified files are C, Rust, or Assembly
- [ ] No UI code added or modified
- [ ] Any new seL4 service has a contract in `contracts/<service-name>/`
- [ ] Any new vibeOS operation is added to `contracts/vibeos/interface.h`
- [ ] Any new generic device service is in `services/` and registered in the generic device table above
- [ ] All new API opcodes have tests in `tests/`
- [ ] `xtask test` passes with no failures
- [ ] No custom device implementation without an approved defect task

---

*This document supersedes any prior verbal or written instructions about what belongs in this repository. When in doubt: no UI, no JavaScript, no untested APIs, and seL4 is Ring 0.*


<!-- BEGIN BEADS INTEGRATION v:1 profile:minimal hash:ca08a54f -->
## Beads Issue Tracker

This project uses **bd (beads)** for issue tracking. Run `bd prime` to see full workflow context and commands.

### Quick Reference

```bash
bd ready              # Find available work
bd show <id>          # View issue details
bd update <id> --claim  # Claim work
bd close <id>         # Complete work
```

### Rules

- Use `bd` for ALL task tracking — do NOT use TodoWrite, TaskCreate, or markdown TODO lists
- Run `bd prime` for detailed command reference and session close protocol
- Use `bd remember` for persistent knowledge — do NOT use MEMORY.md files

## Session Completion

**When ending a work session**, you MUST complete ALL steps below. Work is NOT complete until `git push` succeeds.

**MANDATORY WORKFLOW:**

1. **File issues for remaining work** - Create issues for anything that needs follow-up
2. **Run quality gates** (if code changed) - Tests, linters, builds
3. **Update issue status** - Close finished work, update in-progress items
4. **PUSH TO REMOTE** - This is MANDATORY:
   ```bash
   git pull --rebase
   bd dolt push
   git push
   git status  # MUST show "up to date with origin"
   ```
5. **Clean up** - Clear stashes, prune remote branches
6. **Verify** - All changes committed AND pushed
7. **Hand off** - Provide context for next session

**CRITICAL RULES:**
- Work is NOT complete until `git push` succeeds
- NEVER stop before pushing - that leaves work stranded locally
- NEVER say "ready to push when you are" - YOU must push
- If push fails, resolve and retry until it succeeds
<!-- END BEADS INTEGRATION -->
