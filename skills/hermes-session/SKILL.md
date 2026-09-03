# Hermes operator session (agentOS)

This file is the session context an operator agent (Hermes or any other LLM
CLI) loads when attached to agentOS. It is not a UI. It is not a PD.

## What agentOS is

agentOS is a capability-secured OS on seL4. Guests (Linux, FreeBSD) and
native agents consume **virtualizers** (serial, net, blk). seL4 is the only
kernel-mode code. Native agents are not TCB.

## What this session may do (now)

**Read and report.** Call the inspect snapshot ABI:

- Header: `platform/include/platform/inspect.h`
- Fill: `aos_inspect_fill`
- Structured text: `aos_inspect_format` (`key=value` lines)

The snapshot covers:

- **Memory:** untyped pool total/used, guest RAM size, PD count
- **Threads:** PD index, name, priority, state (running/blocked/idle/unknown)
- **Hardware:** arch, UART PA, GIC distributor PA, emulated virtio-net IPA/IRQ

Host tests: `tests/platform/test_inspect_snapshot.c` (via `make test-host`).
Those tests are a pre-filter. They do not prove a live seL4 query.

## What this session must not do

- No HTML/JS/TUI in this repository. Hermes-the-product is a **client**.
- No JavaScript Protection Domain. No Node in `kernel/`, `services/`, or
  `userspace/servers/`.
- Do not extend `term_server`. It is museum. Interactive bytes later ride
  `serial_virt` (generic serial), same as other native clients.
- Do not store API keys or provider secrets in this tree. The user supplies
  them to the client at attach time.
- Do not compose, spawn, or mutate services in this slice. Inspect is
  read-only (`AOS_INSPECT_OP_SNAPSHOT` and friends). Compose is a later
  contract with its own tests.
- Do not claim host tests as guest-boot or live-IPC proof.

## Architecture (target)

```
user + API key + provider URL
        │
        ▼
Hermes (or other LLM CLI)     ← guest userspace or external process
        │  SKILL.md (this file) + inspect report
        ▼
serial_virt client (PTY-shaped session is a serial client)
        │
        ▼
inspect snapshot (C structs)  ← filled from root-task observations
```

Slice order: inspect report (this skill + ABI) → serial_virt line protocol →
LLM client with user key. Do not skip to compose.

## Hardware facts the report should know

- Emulated virtio-net guest IPA: `0x0A010000` (`AOS_VIRTIO_NET_GUEST_IPA`)
- That IPA must fault to the VMM. QEMU virtio-mmio at `0x0A000000` is a
  kill-dated crutch.
- One UART owner. Guests see virtio-console, not the UART.

## Tracker

mac project `agentos` (dispatch paused — do not activate).

- Epic: `task_72e781c303084d638b732e48d0e9132d`
- Slice 1 (inspect): `task_a80ae509b10540c7932b03d95df2e74b`
- Slice 2 (serial_virt session): `task_0981068853cc4881886a6483f1583733`
- Slice 3 (Hermes client): `task_1ab2cbb61c374bd99b43bbfbebf05bdc`
