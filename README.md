# agentOS

**The world's first operating system designed for agents, not humans.**

[![License](https://img.shields.io/badge/license-BSD--2--Clause-blue.svg)](LICENSE)
[![Kernel](https://img.shields.io/badge/kernel-seL4-green.svg)](https://sel4.systems)
[![Status](https://img.shields.io/badge/status-alpha-orange.svg)]()

---

## What is agentOS?

agentOS is a bootable operating system built on the [seL4 microkernel](https://sel4.systems/) — the world's only formally verified, capability-secured microkernel. It's designed from the ground up for AI agents running autonomous workloads.

Every other "agent OS" is a Python framework running on Linux. They borrow a human OS, bolt some agent abstractions on top, and call it done. **agentOS is different.**

agentOS boots bare metal. The root task, the seL4/Microkit boot path, and Linux
and FreeBSD guest boot are **boot-proven** under QEMU (see the status table
below). The capability model — agents running in isolated address spaces with
hardware-enforced boundaries so an agent cannot touch memory, a tool, a model, or
a storage namespace it doesn't hold a capability for — is the design seL4
provides; in this repository the higher-level agent services are still mostly
host-tested scaffolding, not yet exercised on the booted target.

A core design goal is that **agents can redesign their own environment**: the
vibe-coding layer is intended to let agents generate new system services
(filesystems, message buses, tool registries), have them validated, and hot-swap
them in without rebooting. Today the hot-swap pipeline is **host-tested** — its
read/probe paths are validated, but proposing and swapping a real WASM component
requires a mapped staging region and has not been proven on target.

## Why seL4?

- **Formally verified** — mathematical proof that the implementation matches the spec
- **Capability-based security** — fine-grained, unforgeable access control
- **No heap in the kernel** — deterministic, no memory surprises
- **World-class IPC** — ~100 cycle synchronous IPC on ARM
- **Policy freedom** — kernel provides mechanisms; agents define policies

## Architecture

QEMU is a hardware emulator for prototyping. agentOS is the platform that
runs on that hardware (and later on a real board): user-mode drivers,
sDDF virtualizers, and a VMM that presents **emulated virtio** to Linux and
FreeBSD. Native agents use the same virtualizers without a guest OS.

See `docs/TCB.md` and `PLAN.md`.

```
 Hardware / QEMU
      │
      ▼
 seL4 (EL2)
      ├── root task
      ├── driver PDs + virtualizers (user mode)
      └── VMM (emulated virtio-net/blk/console)
            ├── Linux guest
            └── FreeBSD guest
```

## Core Concepts

### Agent Identity
Every agent has an Ed25519 keypair. Their badge on seL4 endpoints is derived from their identity. Message senders are verified at the kernel level — unforgeable.

### Capabilities
Everything an agent can do requires a capability:
- `ToolCap` — invoke a tool
- `ModelCap` — query an LLM
- `MemCap` — read/write memory regions
- `MsgCap` — send/receive on a channel
- `StoreCap` — access storage namespaces
- `SpawnCap` — create new agents
- `NetCap` — use network resources

Capabilities are delegatable but never escalatable. An agent can grant a subset of what it holds.

### The Vibe-Coding Layer

This is the key design idea (proof level: **host-tested** — see status table).
Every system service has a defined interface. The intended flow is that agents:

1. Analyze the reference implementation
2. Generate a better one (using ModelSvc)
3. Propose it as a replacement via `aos_service_propose()`
4. After validation, activate it via `aos_service_swap()`

The system is designed to evolve; the reference implementations are starting
points, not permanent fixtures. As of today the hot-swap pipeline's read/probe
paths are validated on the host, but a full propose+validate+swap of a real WASM
component has not been proven on a booted target.

## System Services

These agent-facing services have IPC contracts in `contracts/` and host-side
implementations/tests. Unless noted in the status table above, treat them as
**host-tested** — their contracts and logic are validated on the host, not yet
proven against a booted seL4 target.

| Service | Description | Proof level |
|---------|-------------|-------------|
| `CapStore` | Capability database — tracks all cap derivations and grants | host-tested |
| `MsgBus` | Inter-agent communication — channels, pub/sub, direct messaging, RPC | host-tested |
| `MemFS` | Virtual filesystem — per-agent namespaces, capability-gated access | host-tested |
| `ToolSvc` | Tool registry — agents register and invoke tools (MCP-compatible) | host-tested |
| `ModelSvc` | Inference proxy — capability-gated LLM access, pluggable backends | host-tested |
| `NetStack` | TCP/IP networking — lwIP-based, capability-gated per-endpoint | host-tested |
| `BlobSvc` | Object storage — large binary objects, S3-compatible API | host-tested |
| `LogSvc` | Audit logging — structured, queryable, every cap op recorded | host-tested |

The system is implemented as seL4 Microkit Protection Domains with explicit IPC
contracts. External tools consume the contracts; UI code lives in separate
repositories such as `../agentos_gui`.

## agentOS SDK (libagent)

```c
// Initialize agent
aos_init(&config);

// Send a message to another agent
aos_msg_send(dest_id, message);

// Publish to a channel
aos_msg_publish(channel, message);

// Call a tool
aos_tool_call(tool_cap, "web_search", args, args_len, &result, &result_len);

// Query a model
aos_inference_t resp = aos_model_query(model_cap, prompt, &params);

// Read/write storage
aos_store_t f = aos_store_open(cap, "/path/to/file", AOS_STORE_RDWR);
aos_store_write(f, data, len);

// Propose a service replacement (vibe-coding)
aos_service_propose("storage.v1", component_image, image_len, &proposal_id);
aos_service_swap(proposal_id);
```

## Getting Started

### Prerequisites

- macOS with Homebrew, or Ubuntu 22.04+
- 8GB RAM, 20GB disk
- QEMU for simulation (no hardware needed to start)
- Rust, LLVM/Clang, LLD, and the seL4 Microkit 2.1.0 SDK
- Optional ISO cache at `/Volumes/ISOs`; staged guest images live under `build/guest-images`
- **FreeBSD hosts**: cross-compile from Linux/macOS (FreeBSD LLVM cross-compilation support is limited)

### Dual-guest demo

```bash
make setup    # install dependencies, SDK, and validate the host
make demo     # boot Ubuntu + FreeBSD, prove SSH, retain both for login
```

The demo runs one agentOS image with both guest VMMs, proves key-only SSH to
Ubuntu on port 12222 and FreeBSD on port 12223, then prints the exact commands
for opening both sessions. Press Enter in the original terminal to stop QEMU.
See [`docs/demo.md`](docs/demo.md) for the complete walkthrough, expected
runtime, proof boundary, and troubleshooting.

```bash
make demo-test   # same authenticated-SSH acceptance gate; exit afterward
make demo-smoke  # fast host-only checks; no QEMU and not a boot proof
make help        # lower-level build, run, and device-proof targets
```

### Lower-level build and run examples

```bash
make run GUEST_OS=ubuntu                         # foreground Ubuntu run
make run GUEST_OS=freebsd                        # foreground FreeBSD run
make test-guest-login                            # CC-PD console proof
make build TARGET_ARCH=aarch64 GUEST_OS=ubuntu    # AArch64 + Ubuntu 26.04
make build TARGET_ARCH=aarch64 GUEST_OS=freebsd   # AArch64 + FreeBSD 15.0
make build TARGET_ARCH=aarch64 GUEST_OS=both      # Package both guest VMM PDs
make build TARGET_ARCH=x86_64 GUEST_OS=none       # x86_64 root-task smoke image
make fetch-guest GUEST_OS=ubuntu                  # stage Ubuntu assets only
make fetch-guest GUEST_OS=freebsd                 # stage FreeBSD assets only
make fetch-guest GUEST_OS=both                    # stage both guest OS assets
```

Guest images and temporary build artifacts stay under `build/`. Use
`AGENTOS_IMAGES=/path/to/cache` only when intentionally overriding the default.
The dual-guest demo automatically uses 3 GB of outer QEMU RAM. Both guests see
the conventional `0x40000000` GPA window while their VMMs map disjoint host
virtual-address windows. `make run-dual-ssh` remains as the lower-level alias
used by `make demo`.

### FreeBSD host

```bash
# Install build tools (LLVM, dtc, etc.)
make install

# Build with xtask gen-image — no external SDK download needed.
make build
```

### Post-boot CC-PD client

Once agentOS is running in QEMU, use `agentctl` to inspect and control guests
via the CC-PD socket:

```bash
make -C tools/agentctl

# List running guest OS instances
./tools/agentctl/agentctl --batch list-guests

# Query a specific guest
./tools/agentctl/agentctl --batch guest-status <handle>

# Snapshot a guest
./tools/agentctl/agentctl --batch snapshot <handle>
```

Run `./tools/agentctl/agentctl --help` for the full command reference.

## Project Structure

```
agentos/
├── kernel/agentos-root-task/  # seL4 root task, PD code, IPC contracts
├── kernel/freebsd-vmm/        # FreeBSD VMM support code
├── services/                  # host-side service models and prototypes
├── libs/                      # shared C/Rust support libraries
├── userspace/                 # Rust SDK and userspace service crates
├── tools/                     # host tools such as agentctl and image helpers
├── xtask/                     # Rust automation for build/test/fetch flows
├── tests/                     # host, contract, integration, and E2E tests
├── docs/                      # documentation
├── build/                     # generated images, sockets, logs, temp artifacts
├── Makefile                   # top-level build/run/test entry point
└── AGENTS.md                  # repository rules for agents and developers
```

## Development Status

agentOS is **alpha**. Many subsystems exist as host-validated scaffolding rather
than proven bare-metal behavior. To avoid overstating maturity, every subsystem
below is labeled by **proof level**, not by "done / not done".

### Proof-level legend

| Level | Meaning |
|-------|---------|
| **boot-proven** | Exercised on a booted seL4 target under QEMU (or hardware) and asserted by an automated E2E/boot test. |
| **target-tested** | Built into the seL4 target image and validated by a test that runs against the target, but not full end-to-end boot of the feature. |
| **host-tested** | Validated only by host-compiled tests (`-DAGENTOS_TEST_HOST`) where seL4/Microkit IPC is stubbed. Proves contract/logic shape, **not** production IPC or hardware behavior. |
| **stubbed** | Code links and returns a defined value, but the real behavior is a placeholder / `not implemented` / no-op. |
| **planned** | Described in design docs; little or no implementation yet. |

> Per the project's proof policy (`PLAN.md`): host-only mocks may exist, but they
> **cannot** be cited as proof of production IPC or bare-metal behavior. When a
> level below is uncertain, the more conservative label is used.

### Subsystem status

| Subsystem | Proof level | Evidence / notes |
|-----------|-------------|------------------|
| Top-level Makefile (`setup`/`demo`/`build`/`test`/E2E) | host-tested | Build/run orchestration; not a runtime subsystem |
| Raw seL4/Microkit boot (AArch64, x86_64) | boot-proven | `xtask qemu-test` and `tests/end_to_end_boot_test.sh` wait for boot markers; RISC-V build path exists but is not regularly boot-asserted |
| Linux/Ubuntu guest boot | boot-proven | `tests/e2e/run_dual_os_e2e.sh` boots Ubuntu under QEMU and proves it via SSH; `make test-guest-login` waits for the login prompt via CC-PD |
| FreeBSD 15.0 guest boot | boot-proven | Same dual-OS E2E boots FreeBSD and SSHes in; commit `3f5365a` "prove dual linux freebsd lifecycle" |
| Guest VMM multiplexer (slot create/switch/list/status) | target-tested | VMM contract + slot lifecycle exercised; per-guest VMM slots coordinated (`470679f`) |
| Guest snapshot / restore | stubbed | `vm_manager.c`: `SNAPSHOT/RESTORE: not implemented (Phase 1)`; `cc_pd.c` boot-guest snapshot returns `CC_ERR_RELAY_FAULT` |
| Guest live-migrate | planned | `MSG_VIBEOS_MIGRATE` defined in contract; no target-validated implementation |
| Guest virtual IRQ injection | stubbed | `vm_manager.c` `vmm_inject_irq` logs `(stub)`; real `virq_inject` via libvmm is a TODO |
| Dynamic guest CREATE/LIST/DESTROY via vibe_engine | host-tested | On AArch64 the build links `vmm_mux_stub.c`; vibe_engine surfaces dynamic guests as "phantom" `RUNNING` because no real VM boots (`e70d955`). Lifecycle UX works end-to-end through CC-PD against stubbed VM backing only. |
| serial-mux / serial PD | boot-proven | Guest console login flows through CC-PD over the serial path (`make test-guest-login`) |
| Emulated virtio-net / net-service | target-tested | `make test-guest-net` boots Buildroot, reaches `DRIVER_OK`, and requires a guest packet through the agentOS virtualizer; host contract tests provide additional logic coverage |
| Emulated virtio-blk / block-service | target-tested | `make test-guest-blk` boots Buildroot and requires a guest block request through the agentOS virtualizer; host contract tests provide additional logic coverage |
| usb-service | stubbed | `usb_pd.c` runs in "stub mode" (simulated HID device) unless built with `AGENTOS_USB_PD` and real MMIO is wired |
| timer-service | host-tested | Host contract tests (`tests/contracts/timer_test.c`) |
| entropy-service | host-tested | `services/entropy-service/entropy_svc.c` + contract; not target-validated |
| CC-PD host API (list/status/console) | boot-proven | Unix socket bridge at `build/cc_pd.sock`; list/status/console-drain proven by guest-login E2E |
| CC-PD snapshot relay | stubbed | Returns `CC_ERR_RELAY_FAULT` for the boot guest (snapshot not implemented) |
| VibeOS lifecycle API (`VOS_*`) | host-tested | Contract tests build with `-DAGENTOS_TEST_HOST` (`make test-vibeos-contract`, `tests/api/test_vibeos*.c`); create/destroy/list/status logic proven on host, not on target |
| vibe-engine WASM hot-swap | host-tested | `tests/integration/vibe_hotswap_test.c` exercises read/probe paths only; actual WASM propose+swap needs a mapped staging region (hardware-dependent) |
| Tracing (trace_recorder PD) | host-tested | 512-entry ring with START/STOP/QUERY/DUMP; covered by host contract tests, not boot-asserted |
| Host integration/contract test suite | host-tested | `make test-integration` runs host-compiled tests with stubbed IPC |
| Build artifact hygiene | host-tested | Images, sockets, logs, temp files under `build/` |
| External GUI | Separate project | Lives in `../agentos_gui`; not part of this repo |

## Philosophy

agentOS is built on a few core beliefs:

1. **Agents deserve their own OS.** Running on Linux is running on someone else's OS, designed for someone else's needs.

2. **Security is not optional.** Formal verification, capability-based isolation, and hardware-enforced boundaries are the minimum bar for a system where autonomous agents operate.

3. **Agents should design their environment.** The hardest part of building agent infrastructure is that humans are guessing at what agents need. Let agents figure it out themselves.

4. **Boot it or it doesn't count.** An "agent OS" that's a Python package is an agent library. agentOS boots.

## CUDA Compute Offload

> **Proof level: host-tested / planned.** The PTX-in-WASM extraction and slot
> bookkeeping are exercised on the host. On QEMU/RISC-V (no GPU) the
> gpu_scheduler is bookkeeping only; real `nvrtc` JIT + CUDA-context binding on
> Sparky GB10 (Blackwell) is **planned** and not boot-proven here.

agentOS is designed to support GPU kernel offload via CUDA PTX embedded in WASM modules.

### How it works

1. **Embed PTX in WASM**: Add a custom section named `agentos.cuda` to any WASM module. The section payload is a raw PTX source file (must begin with `.version`).

2. **Submit via VibeEngine**: When an agent submits a WASM module with this section, VibeEngine automatically extracts and validates the PTX during `OP_VIBE_VALIDATE`.

3. **gpu_scheduler PD**: On `OP_VIBE_EXECUTE`, VibeEngine notifies the `gpu_scheduler` protection domain (priority 160, passive). The scheduler claims one of 4 static GPU slots.

4. **On Sparky GB10 (Blackwell)**: The gpu_scheduler would JIT-compile the PTX via `nvrtc` and bind it to a CUDA context on the slot. On QEMU/RISC-V (no GPU), it's bookkeeping only.

### Rust SDK

```rust
use agentos_sdk::cuda::CudaKernel;

let ptx = b".version 7.5\n.target sm_90\n.address_size 64\n\
            .visible .entry matmul(.param .u64 A, .param .u64 B, .param .u64 C) {\n\
                ret;\n}\n".to_vec();

let kernel = CudaKernel::new(ptx, "matmul".to_string());
kernel.validate()?;   // Check PTX before submitting
kernel.submit(0)?;    // Submit to GPU slot 0
// ... kernel runs on GPU ...
CudaKernel::complete(0)?; // Release slot
```

### Channel topology

```
vibe_engine (CH_GPU=2) ──notify──► gpu_scheduler (CH_VIBE=0)
controller  (CH=51)    ──ppcall──► gpu_scheduler (CH_CTRL=1)
```

---

## FreeBSD VM Guest

agentOS stages and boots **FreeBSD 15.0 AArch64** as a virtual machine guest
under the seL4 hypervisor path (proof level: **boot-proven** — `run_dual_os_e2e.sh`
boots it under QEMU and proves it over SSH). The same CC-PD API surface used by
Ubuntu enumerates the guest, drains serial output, and injects console input.

seL4 runs at **EL2** (ARM hypervisor mode) — it IS the hypervisor. No separate hypervisor layer needed.

### Boot sequence

```
seL4 (EL2)
  └─► freebsd_vmm PD (libvmm)
        └─► EDK2 UEFI firmware @ guest phys 0x00000000
              └─► bootaa64.efi → loader.efi
                    └─► FreeBSD kernel (EL1)
```

### VM Multiplexer

The generic VMM contract models up to 4 guest slots. The FreeBSD path uses the
same slot lifecycle vocabulary as the Ubuntu path. CREATE/SWITCH/STATUS/LIST and
single-guest boot are exercised by `make test-guest-login` and the E2E targets
(**boot-proven** / **target-tested**). Note that `SNAPSHOT`/`RESTORE` and virtual
IRQ injection in `vm_manager.c` are **stubbed** ("not implemented (Phase 1)"),
and on AArch64 dynamic CREATE via vibe_engine currently links a VMM stub
(dynamic guests appear as "phantom" `RUNNING`). The opcode table below describes
the contract, not a fully boot-proven implementation of every opcode:

| Opcode | Operation | Args | Returns |
|--------|-----------|------|---------|
| `0x10` | `OP_VM_CREATE` | — | `slot_id` (0-3) or `0xFF` |
| `0x11` | `OP_VM_DESTROY` | `mr[0]=slot_id` | `0` ok / `1` error |
| `0x12` | `OP_VM_SWITCH` | `mr[0]=slot_id` | `0` ok / `1` error |
| `0x13` | `OP_VM_STATUS` | — | `mr[0..3]` = state per slot |
| `0x14` | `OP_VM_LIST` | — | count + `(slot_id<<8\|state)` per slot |

**Slot states:** `FREE(0)` -> `BOOTING(1)` -> `RUNNING(2)` <->
`SUSPENDED(3)` -> `HALTED(4)` / `ERROR(5)`

Console focus follows the selected guest handle through CC-PD.

### Quick start

```bash
# Install build dependencies and the shared Microkit SDK
make setup

# Stage FreeBSD 15.0 assets under build/guest-images
make fetch-guest GUEST_OS=freebsd

# Build and boot the FreeBSD guest path
make run GUEST_OS=freebsd
```

### Inspecting the Guest

Once agentOS is running, inspect the FreeBSD guest through the CC-PD reference
consumer:

```bash
make -C tools/agentctl
./tools/agentctl/agentctl --batch list-guests
./tools/agentctl/agentctl --batch guest-status 0
```

### Memory layout

Each VMM has an isolated guest VSpace. In the dual demonstration both guests
see the conventional RAM GPA at `0x40000000`, while their VMM aliases are
disjoint:

```
Guest view:
  FreeBSD GPA: 0x40000000 - 0x4fffffff  (256 MB)
  Linux GPA:   0x40000000 - 0x7fffffff  (1 GB)

VMM aliases:
  FreeBSD HVA: 0x80000000 - 0x8fffffff
  Linux HVA:   0xc0000000 - 0xffffffff
```

The repeated GPA does not overlap because each guest has its own VSpace.

### Why FreeBSD?

- BSD license aligns with seL4's formal verification story
- **Jails** map naturally to seL4 capability domains (Phase 3 roadmap)
- `pf` firewall ruleset = capability policy layer
- ZFS + GEOM: a principled storage stack for agentOS's BlobSvc
- bhyve inside FreeBSD = agents can *nest* hypervisors within agentOS

See [`docs/freebsd-vm-guest.md`](docs/freebsd-vm-guest.md) for the full design doc.

---

## Contributing

agentOS is in early (alpha) development. The design is stable; the implementation
is growing and much of it is still host-tested scaffolding (see the proof-level
status table above). Contributions welcome in:

- seL4/Microkit integration and IPC contract coverage
- libagent SDK implementation
- Generic device PD and VMM integration
- CC-PD API and E2E tests that prove guest OS behavior
- Documentation and tutorials

## License

BSD 2-Clause. See [LICENSE](LICENSE).

## Acknowledgments

Built on [seL4](https://sel4.systems/) by the Trustworthy Systems group at CSIRO's Data61. The formally verified foundation that makes this possible.

---

*"Hey Rocky, watch me pull an operating system out of my hat!"*  
*"That trick never works!"*  
*"This time for sure!"*  
*[boots successfully]* 🫎
