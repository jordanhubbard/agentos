# agentOS — Agent Development Guidelines

**This file is mandatory reading for any AI agent or developer working on this repository.**
**Violations of these rules will result in rejected PRs.**

---

## What This Project Is

agentOS is a **seL4 Microkit-based operating system for AI agents**. It runs on bare metal.
There is no libc. There is no POSIX. There is no userland in the traditional sense.
The entire OS is a set of cooperating **Protection Domains (PDs)** communicating via
seL4 IPC. Only seL4 itself runs in Ring 0. Everything else — every OS service, every
device driver, every guest OS manager — runs in outer rings as Microkit PDs.

This is not a Linux distro. This is not a container runtime. If you don't understand
seL4 Microkit IPC, read the [seL4 Microkit manual](https://trustworthy.systems/projects/microkit/manual.html)
before writing any code.

---

## The Rules

### 1. NO UI IN THIS REPOSITORY

There is no HTML. No JavaScript. No CSS. No ncurses. No interactive terminal UI.
No dashboard. No web interface. No WebSocket bridges. **Nothing that a human looks at.**

agentOS is for agents. Agents use APIs. If someone wants a UI, they build it as an
**external project** that consumes agentOS IPC contracts. That external project is not
this repository.

The **only** exception is `agentctl` — a CLI build/launch/management tool that prints
structured output (JSON or tab-separated) to stdout and exits. It is a tool, not a UI.

**What this means in practice:**
- Do not add `.html`, `.css`, `.js`, `.mjs`, `.jsx`, `.tsx`, `.vue`, `.svelte` files. Ever.
- Do not add `package.json`, `node_modules`, `yarn.lock`, `bun.lockb`. Ever.
- Do not add ncurses, readline, or any interactive input handling to PD code.
- Do not add "pretty printing", progress bars, or human-readable formatting to PD output.
- PD output goes through `MSG_LOG_WRITE` to the log drain. Period.

### 2. PURE ASSEMBLY + C + RUST

The core of agentOS is written in:
- **Assembly** (seL4 bootstrap, architecture-specific stubs)
- **C** (kernel PDs, device drivers, core services) — freestanding, no libc
- **Rust** (userspace servers, SDK, higher-level services) — `no_std` where applicable

**No interpreted languages in core.** Python and Node.js are permitted ONLY in:
- `tools/` — external build/debug tooling (clearly marked as host-side)
- `sdk/python/` — Python SDK for external consumers
- CI scripts

They must **never** appear in `kernel/`, `services/`, `libs/`, or `userspace/servers/`.

### 3. seL4 IS THE ONLY RING 0 CODE

No PD may claim kernel privilege. No PD may bypass seL4 IPC. No PD may directly
manipulate hardware without holding the appropriate seL4 device frame capability
granted by the root task.

If you find yourself writing code that "needs kernel access" — you're doing it wrong.
Redesign as an IPC contract with the appropriate PD.

### 4. API CONTRACTS PRECEDE IMPLEMENTATION

Before writing **any** new PD code:

1. Define the IPC contract in a header: `include/contracts/<pd_name>_contract.h`
2. Add opcodes to `agentos_msg_tag_t` in `agentos.h`
3. Define request/reply structs in the contract header
4. Write a failing test in `tests/contracts/<pd_name>_test.c`
5. **Then** implement the PD

A PR that adds implementation without a contract header and test will be rejected.

### 5. EVERY PD IS AN API ENDPOINT

Every Protection Domain exposes exactly **one** IPC contract. That contract — defined
in its `_contract.h` header — is the **only** way to interact with the PD.

A contract header defines:
- Channel IDs (cross-referenced to `agentos.h` constants)
- Message opcodes (cross-referenced to `agentos_msg_tag_t`)
- Request structs: `struct <pd>_req_<opcode>`
- Reply structs: `struct <pd>_reply_<opcode>`
- Error codes: `enum <pd>_error` (0 is always success)
- Invariants: preconditions, postconditions, ordering constraints

### 6. GENERIC BEFORE SPECIFIC

agentOS provides **OS-neutral generic device PDs** for all standard device classes:
- `serial_pd` — serial I/O
- `net_pd` — networking
- `block_pd` — block storage
- `usb_pd` — USB devices
- `timer_pd` — timers/RTC
- `irq_pd` — interrupt routing

**No guest OS may implement its own driver for any device class that has a generic PD.**

If a guest OS needs device functionality the generic PD cannot provide:
1. File a GitHub issue: `[device-waiver] <guest_os> requires <device_class>`
2. Document: what the generic PD can't do, what extension would fix it, why the
   extension can't be made
3. Get approval from the project owner **before writing any code**

PRs with guest-specific drivers and no approved waiver issue: **rejected**.

### 7. TESTS ARE THE SOURCE OF TRUTH

The test suite in `tests/` is the authoritative specification of API behavior.
**A contract not tested is not a contract.**

- Every IPC opcode must have at least one test
- Every PR that adds/modifies an opcode must add/update tests
- PRs that reduce passing test count: **rejected**
- `make test` must pass on both `TARGET_ARCH=aarch64` and `TARGET_ARCH=x86_64`

---

## Repository Structure

```
agentOS/
├── kernel/agentos-root-task/
│   ├── include/
│   │   ├── agentos.h                    # Master header: opcodes, types, constants
│   │   └── contracts/                    # Per-PD IPC contract headers
│   │       ├── eventbus_contract.h
│   │       ├── serial_contract.h
│   │       ├── net_contract.h
│   │       ├── guest_contract.h
│   │       ├── vmm_contract.h
│   │       ├── vibeos_contract.h
│   │       └── ...
│   └── src/                              # PD implementations (C, freestanding)
│       ├── event_bus.c
│       ├── log_drain.c                   # was console_mux.c
│       ├── ipc_harness.c                 # was dev_shell.c (test builds only)
│       ├── serial_pd.c
│       ├── net_pd.c
│       ├── block_pd.c
│       └── ...
├── userspace/
│   ├── sdk/                              # Rust SDK for userspace PDs
│   └── servers/                          # Rust-based service PDs
├── services/                             # Higher-level services
├── tools/
│   └── agentctl/                         # CLI build/launch tool (C)
├── sdk/python/                           # External Python SDK
├── tests/
│   ├── contracts/                        # Per-PD contract tests
│   ├── integration/                      # Cross-PD integration tests
│   └── harness/                          # Test framework and runner
├── PLAN.md                               # Active refactoring plan
├── DESIGN.md                             # Architecture design document
└── AGENTS.md                             # THIS FILE
```

---

## The Architecture

```
┌─────────────────────────────────────────────────────┐
│                    Ring 0: seL4                       │
│              (ONLY kernel-privilege code)             │
└──────────────────────┬──────────────────────────────┘
                       │ IPC (microkit_call / microkit_reply)
┌──────────────────────┴──────────────────────────────┐
│              Outer Rings: Microkit PDs                │
│                                                       │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐             │
│  │ EventBus │ │ LogDrain │ │ QuotaPD  │  Core Svcs  │
│  └──────────┘ └──────────┘ └──────────┘             │
│                                                       │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐             │
│  │serial_pd │ │  net_pd  │ │ block_pd │  Device PDs │
│  └──────────┘ └──────────┘ └──────────┘             │
│                                                       │
│  ┌──────────────┐ ┌──────────────┐                   │
│  │  linux_vmm   │ │ freebsd_vmm  │   Guest OS VMMs  │
│  │  (VirtIO ←───┤─┤── generic PDs)                   │
│  └──────────────┘ └──────────────┘                   │
│                                                       │
│  ┌──────────────┐ ┌──────────────┐                   │
│  │  VibeEngine  │ │  VibeSwap    │   OS Management   │
│  └──────────────┘ └──────────────┘                   │
└──────────────────────────────────────────────────────┘
```

**Key invariants:**
- All arrows are IPC. No shared memory without explicit mapping via seL4 capabilities.
- Guest VMMs bind to generic device PDs, not hardware.
- VibeEngine orchestrates full OS lifecycle via IPC composition.

---

## How to Add a New PD

1. **Create contract header**: `include/contracts/<name>_contract.h`
   - Define channel IDs, opcodes, request/reply structs, error codes
   - Add opcodes to `agentos_msg_tag_t` in `agentos.h`

2. **Write failing test**: `tests/contracts/<name>_test.c`
   - Use `test_framework.h` macros
   - Test every opcode: success path + error paths

3. **Implement PD**: `kernel/agentos-root-task/src/<name>.c`
   - Include your contract header
   - Handle every opcode in your `notified()` / `protected()` handler
   - Report boot via `MSG_LOG_WRITE` to log drain
   - All output via `MSG_LOG_WRITE` — no `dbg_puts` except pre-boot

4. **Register in build system**: Add to `CMakeLists.txt` / Microkit system description

5. **Update CI**: Ensure `make test` exercises your PD

6. **PR requirements**:
   - Contract header ✓
   - Test file ✓
   - Implementation ✓
   - All existing tests still pass ✓
   - No UI code ✓
   - No interpreted languages in core ✓

---

## How to Add a New Guest OS

1. Create VMM PD: `kernel/agentos-root-task/src/<os>_vmm.c`
2. Include `contracts/guest_contract.h` and `contracts/vmm_contract.h`
3. Implement the guest binding protocol (§3.1 of PLAN.md):
   - Subscribe to EventBus
   - Query AgentFS `/devices` for available device PDs
   - Open device handles (capability-badge validated)
   - Register with QuotaPD
   - Publish EVENT_GUEST_READY
4. Expose VirtIO transport backed by generic device PDs
5. Do NOT implement any device class that has a generic PD (see Rule 6)
6. Test in `tests/integration/<os>_binding_test.c`

---

## Commit and PR Guidelines

### Commit messages
```
<phase>: <short description>

<optional body explaining WHY, not WHAT>

Refs: #<issue_number>
```

Examples:
```
phase-0: delete console/ directory and Makefile references

The console UI contradicts the API-first mandate. All human-facing
interfaces must be external projects consuming IPC contracts.

Refs: #23
```

### PR checklist
Every PR must satisfy ALL of the following:
- [ ] No `.html`, `.css`, `.js`, `.mjs`, `.jsx`, `.tsx` files added
- [ ] No `package.json`, `node_modules` added
- [ ] No interpreted languages in `kernel/`, `services/`, `libs/`, `userspace/servers/`
- [ ] Every new IPC opcode has a contract header entry
- [ ] Every new IPC opcode has a test
- [ ] `make test TARGET_ARCH=aarch64` passes
- [ ] `make test TARGET_ARCH=x86_64` passes
- [ ] No guest-specific device drivers without approved waiver issue
- [ ] No UI code of any kind

### Branch naming
```
phase-N/description    (e.g., phase-0/delete-console)
fix/description        (e.g., fix/log-drain-overflow)
test/description       (e.g., test/eventbus-contract)
```

---

## What Does NOT Belong in This Repository

| Category | Example | Where It Goes |
|----------|---------|---------------|
| Web dashboards | React/Vue/HTML status pages | External project |
| Interactive TUIs | ncurses monitoring tools | External project |
| WebSocket bridges | Real-time event streaming to browsers | External project |
| Chat interfaces | Slack/Discord bots for OS management | External project |
| Jupyter notebooks | Data analysis of OS metrics | External project |
| npm/yarn/bun anything | Node.js package ecosystem | Nowhere near here |

If you're unsure whether something belongs here, apply this test:
> **Does it run as a seL4 Microkit PD, or is it a host-side build/test tool?**
> If neither, it doesn't belong here.

---

## Current Active Plan

See `PLAN.md` for the comprehensive 6-phase refactoring plan (Phases 0–5).
All work must reference the corresponding phase and GitHub issue.

**Phase execution order:**
```
Phase 0 (no deps)     → UI removal, file cleanup
Phase 1 (needs Ph.0)  → API contract headers for all PDs
Phase 2 (needs Ph.1)  → Generic device PDs (serial, net, block, USB)
Phase 3 (needs Ph.2)  → Guest OS binding protocol
Phase 4 (needs Ph.3)  → VibeOS lifecycle API
Phase 5 (parallel)    → Test suite (stubs written alongside Phase 1)
```

Do not skip phases. Do not start Phase N+1 before Phase N is complete.
Phase 5 test stubs are the exception — write failing tests first, then implement.

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
