# agentOS — Project Constitution for AI Agents

This document is **binding**. Read `docs/TCB.md` first. Do not make exceptions.

---

## What agentOS Is

agentOS is a bootable, capability-secured operating system on the seL4
microkernel. It is the **I/O and isolation platform** for:

- Linux and FreeBSD as **guests** (in-tree virtio drivers only)
- Native purpose-driven PDs / agents that never run a legacy OS

QEMU is a **hardware emulator** for fast prototyping. The same driver PDs that
own QEMU virtio-mmio as a stand-in NIC/disk will own real devices on bare metal.

agentOS is not a thin hypervisor slave to QEMU. It is not a web app. It is not
a dashboard. It boots on bare metal.

---

## Trusted Computing Base

See `docs/TCB.md`. Short form:

- **seL4** is the only kernel-mode code. Never modify it.
- **Root task** distributes untyped memory, CSpace, VSpace, and initial caps.
  It does not enforce policy after spawn.
- **Driver PDs** own exactly one device class (frame + IRQ).
- **Virtualizer PDs** mux that class over sDDF-shaped shared-memory queues.
- **VMM PDs** run vCPUs/vGIC and **emulate virtio** for guests.
- Everyone else is a client or is quarantined (`docs/TCB.md` museum list).

There is no "Ring 1–5". Those numbers mixed Intel rings with ARM ELs with
seL4 PDs and were wrong. Guest userspace runs in the **guest** VSpace.

---

## Language Policy

### On-target (PDs, libvmm backends, root task)

**C, Rust, Assembly only.** Freestanding. No Python runtime in a PD.
No JavaScript. No Go, Zig, or other languages unless the project owner
approves in writing.

### Host composition (`skills/`, `tools/`)

**Python is required** for skill helpers and generators. Helpers must stay
small: they compute (graphs, DTB/ELF/virtio inspection, topology) and print
**HTML** (or another structured dump) for LLM agents. That HTML is not a
product UI and must not be served by any PD.

CMake / Make orchestrate builds. Shell is CI glue and one-line wrappers.

WASM is a guest/agent binary format only. WASM runtime is C or Rust. WASM is
not a NIC, disk, or UART.

---

## UI Policy

No human UI in this repository: no dashboards, no HTML/JS served by a process
in this repo, no WebSocket terminal emulators.

Exceptions:

- `agentctl` — CLI, structured stdout, exits
- `skills/*/scripts/*.py` — HTML **to the model**, not to a browser session
- `../agentos_gui` — external consumer of CC-PD / contracts

---

## I/O Policy — sDDF + virtio

Before any guest, VMM, or agent implements a device class:

| Class | Owner | Guest ABI |
|-------|--------|-----------|
| Serial | serial driver + virt | virtio-console (or one UART emu backed by serial queues) |
| Network | nic driver + net_virt | virtio-net, **emulated** in the VMM |
| Block | blk driver + blk_virt | virtio-blk, **emulated** in the VMM |

libvmm `src/virtio/{net,block,console}.c` **is** the guest-facing virtio
device. Use it. Do not passthrough QEMU virtio-mmio into the guest.

Host virtio (QEMU) is the **physical device** for the driver PD only.

Do not add a guest-specific driver for a class that has a virtualizer.

---

## API-First (on-target)

Every live TCB PD exposes one IPC or queue contract **before** callers.

- Queue protocols for bulk I/O (sDDF)
- seL4 IPC for control (create, bind, status)
- Packed C structs, explicit opcodes, version field
- Stored under `kernel/agentos-root-task/include/contracts/` for IPC
- Stored under `platform/include/platform/` for queue layouts

A museum PD with a contract and no boot-time caller is not an API.

---

## Tests

No untested TCB path. Host-only tests (`-DAGENTOS_TEST_HOST`) are a pre-filter
and **cannot** be cited as proof of I/O or IPC. `make gate` is the OS-claim
gate. Device-class claims also need a guest I/O assertion through the
virtualizer.

---

## What Goes in This Repository

1. seL4 root task
2. TCB PDs (drivers, virtualizers, VMM)
3. Queue/IPC contracts for those PDs
4. Guest payloads and FDT (Linux, FreeBSD) — no custom guest drivers
5. Optional native-agent **clients** of virtualizers
6. Simulator / host tests
7. `skills/` and `tools/` (Python helpers, generators)
8. Documentation (`docs/TCB.md`, `DESIGN.md`, `PLAN.md`)

## What Must Not Be Added

- UI served to humans
- JavaScript
- Python inside PDs
- Guest drivers for a class that already has a virtualizer
- New museum PDs (`oom_killer`, POSIX spawn/vfs, vibe-swap as networking, …)
- QEMU virtio passthrough as architecture
- Cloud / LLM SDKs in PDs

If you find a forbidden file: do not extend it. File a beads issue.

---

## Checklist Before Merging

- [ ] On-target files are C, Rust, or Assembly
- [ ] Skills/tools Python does not run in a PD
- [ ] No new museum PDs
- [ ] New I/O uses sDDF queues + emulated virtio for guests
- [ ] No QEMU device passthrough for a class that has a virtualizer
- [ ] Tests exist; host-only is not claimed as boot-proven
- [ ] `docs/TCB.md` still accurate

---

## Beads

Use `bd` for all task tracking. See the beads block below.

<!-- BEGIN BEADS INTEGRATION v:1 profile:minimal hash:ca08a54f -->
## Beads Issue Tracker

This project uses **bd (beads)** for issue tracking. Run `bd prime` to see full workflow context and commands.

### Quick Reference

```bash
bd ready # Find available work
bd show <id> # View issue details
bd update <id> --claim # Claim work
bd close <id> # Complete work
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
 git status # MUST show "up to date with origin"
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
