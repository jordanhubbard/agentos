# agentOS — Agent Development Guidelines

**Mandatory.** Violations are rejected. Full constitution: `CLAUDE.md`. TCB: `docs/TCB.md`. Plan: `PLAN.md`.

---

## What This Project Is

agentOS is a seL4 OS that **owns devices in user mode** and hosts Linux and
FreeBSD as virtio guests. QEMU pretends to be a board. The real target is
bare metal.

There is no POSIX userland. There is no "ring 1–5". seL4 is EL2. Our PDs are
EL0. Guest kernels are EL1 and are hostile.

If you do not understand seL4 IPC and sDDF queues, read the seL4 Microkit
manual and `libvmm/dep/sddf/docs/network/network.md` before writing I/O code.

---

## The Rules

### 1. TCB first

Only PDs in `docs/TCB.md` may own a device frame or IRQ. Museum PDs listed
there must not be extended.

### 2. No human UI here

No HTML/JS served by a process in this repo. Skill helpers may **print** HTML
for LLM agents.

### 3. On-target: C / Rust / ASM. Host skills: Python.

Python in `skills/` and `tools/` only. Never in `kernel/` live PDs.

### 4. Guests speak virtio that **we** emulate

Use `libvmm` virtio device backends (`virtio_mmio_net_init`, blk, console).
Do not map QEMU virtio-mmio into the guest. Host virtio is for the driver PD.

### 5. Contracts before callers

Queue layout in `platform/include/platform/`. IPC in
`kernel/agentos-root-task/include/contracts/`. Tests must assert outputs.

### 6. Make is the interface

If a Makefile rule exists, use it (`make build`, `make test`, `make gate`).

---

## How to Add I/O (the only new "PD" path that matters)

1. Driver PD owns the physical (or QEMU-stand-in) device.
2. Virtualizer PD muxes sDDF queues to N clients.
3. VMM calls libvmm `virtio_mmio_*_init` at a **guest IPA that faults**.
4. Guest DTB describes that emulated device, not the host MMIO.
5. Host test for the pump/mux; QEMU test that the guest sees the device.

Do not add `MSG_*_OPEN` opcode tables as a substitute for queues.

---

## How to Add a Skill

```
skills/<block>/SKILL.md          # small: invariants, forbidden moves, helper
skills/<block>/scripts/*.py      # compute; print HTML to stdout
```

Blocks that should exist: `sel4-platform`, `sddf-net`, `sddf-blk`,
`sddf-serial`, `virtio-device`, `linux-guest`, `freebsd-guest`, `compose`.

---

## Commit messages

```
platform: <short description>

<why>

Refs: agentos-<id>
```

---

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
