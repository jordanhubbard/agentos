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

Refs: <mac-task-id>
```

---

## Issue tracking — mac, not beads

This project uses the **MAC hub task ledger**. Do not run `bd`. Beads/dolt
is retired (the Dolt remote could not merge). Canonical store: `mac task`.

Project name: `agentos`. Dispatch is **paused** — do not activate it so a
fleet agent rewrites seL4 code unsupervised. Interactive sessions file,
claim, and close tasks themselves.

```bash
mac task list --project agentos
mac task ready --project agentos --limit 10
mac task show <id>
mac task create "title" --project agentos --description-file=desc.txt --no-dispatch
mac task close <id> --reason="..."
mac admin memory remember <key> "<content>" --project=agentos
```

- `--no-dispatch` stages work: it is hidden from `task ready` until
  `mac task release <id>`. Keep it while dispatch is paused.
- Multi-line / shell-hostile text: `--description-file` (or `-` for stdin).
- `.tickets/` is a local mirror. Do not commit it.
- Do not use TodoWrite, TaskCreate, or markdown TODO lists as the tracker.

## Session Completion

Work is not complete until `git push` succeeds.

1. File follow-up via `mac task create --project agentos`
2. Quality gates if code changed (`make test-host`; OS claims need `make gate`)
3. `mac task close <id> --reason="..."`
4. ```
   git pull --rebase
   git push
   git status   # up to date with origin
   ```
5. Clean stashes / leftover branches
6. Hand off: next ready task is `mac task ready --project agentos`
