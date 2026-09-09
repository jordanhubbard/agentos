# agentOS Architecture

## System Layers

```
┌─────────────────────────────────────────────────────────────────────────┐
│  Native agents / applications (EL0 clients of virtualizers)             │
│                                                                         │
│   Agent binary (WASM or ELF)         agentctl CLI                      │
│   libagent.c (seL4_Call wrappers)    xtask / build tools               │
└────────────────────────────┬────────────────────────────────────────────┘
                             │ seL4 IPC (capability-gated)
┌────────────────────────────▼────────────────────────────────────────────┐
│  VMM PDs (EL0) — vCPU, vGIC, emulated virtio                            │
│                                                                         │
│   linux_vmm.c          freebsd_vmm.c      (future guest VMMs)          │
│   ┌──────────────┐     ┌─────────────┐                                 │
│   │ Linux guest  │     │ FreeBSD     │  virtio devices via seL4 shmem  │
│   │ (EL1)        │     │ guest (EL1) │                                 │
│   └──────────────┘     └─────────────┘                                 │
│   gpu_shmem.c (approved custom channel — DEFECT-001)                   │
└────────────────────────────┬────────────────────────────────────────────┘
                             │ seL4 IPC
┌────────────────────────────▼────────────────────────────────────────────┐
│  Driver + virtualizer PDs (EL0) — own hardware, mux I/O                 │
│                                                                         │
│  VibeOS lifecycle        Hot-swap pipeline     OS management            │
│  ┌─────────────────┐    ┌────────────────┐    ┌──────────────────┐    │
│  │  vibe_engine.c  │    │  vibe_swap.c   │    │  vm_manager.c    │    │
│  │  MSG_VIBEOS_*   │───▶│  swap slots    │    │  OP_VM_CREATE    │    │
│  │  MSG_VIBE_*     │    │  wasm3 runtime │    │  OP_VM_START     │    │
│  └────────┬────────┘    └────────────────┘    │  OP_VM_CONFIGURE │    │
│           │ OP_VM_*                            └──────────────────┘    │
│           └────────────────────────────────────────────┘               │
│                                                                         │
│  Devices / I/O            Security              Networking              │
│  ┌──────────────┐    ┌──────────────────┐    ┌────────────────────┐   │
│  │ serial_pd.c  │    │  cap_broker.c    │    │  net_server.c      │   │
│  │ block_pd.c   │    │  (grant/revoke/  │    │  net_pd.c          │   │
│  │ framebuf_pd.c│    │   cascade)       │    │  wg_net.c          │   │
│  │ virtio_blk.c │    └──────────────────┘    └────────────────────┘   │
│  └──────────────┘                                                       │
│                                                                         │
│  Messaging / Storage      Scheduling             Observability          │
│  ┌──────────────┐    ┌──────────────────┐    ┌────────────────────┐   │
│  │ event_bus.c  │    │  gpu_sched.c     │    │  log_drain.c       │   │
│  │ nameserver.c │    │  time_partition  │    │  mem_profiler.c    │   │
│  │ agentfs      │    │  quota_pd        │    │  perf_counters.c   │   │
│  └──────────────┘    └──────────────────┘    │  trace_recorder.c  │   │
│                                              └────────────────────┘   │
└────────────────────────────┬────────────────────────────────────────────┘
                             │ seL4 system calls only
┌────────────────────────────▼────────────────────────────────────────────┐
│  Root task (EL0) — untyped, spawn, initial caps                         │
│                                                                         │
│   init_agent.c     controller.c     vibe_engine init                   │
│   Distributes initial capabilities. No policy enforcement.              │
│   Spawns TCB PDs. Never modified after boot.                            │
└────────────────────────────┬────────────────────────────────────────────┘
                             │ seL4 kernel API (seL4_Call, seL4_Reply …)
┌────────────────────────────▼────────────────────────────────────────────┐
│  seL4 (EL2) — formally verified, never modified                         │
│                                                                         │
│   Capability system   IPC endpoints   TCB / scheduling   Memory objects │
└─────────────────────────────────────────────────────────────────────────┘
```

## Primary API Surfaces

agentOS is API-first. Every Protection Domain exposes one IPC contract, and
host-side tools reach the running QEMU instance through the CC-PD Unix socket
at `build/cc_pd.sock`. `agentctl` and `../agentos_gui` are reference external
consumers of that API; no UI code belongs in this repository.

The main runtime surfaces are:

```
Host tool / GUI
        │
        │  Unix socket bridge: build/cc_pd.sock
        ▼
  cc_pd.c
        │
        ├─ MSG_CC_LIST_GUESTS / MSG_CC_GUEST_STATUS
        ├─ MSG_CC_CREATE_GUEST
        ├─ MSG_CC_CONSOLE_DRAIN / MSG_CC_SEND_INPUT
        ├─ MSG_CC_SNAPSHOT / MSG_CC_RESTORE
        └─ display/device relay calls

agent / controller PD
        │
        │  seL4 IPC
        ▼
  guest/vmm/vibeOS contracts
        │
        ├─ MSG_GUEST_*       guest lifecycle and console I/O
        ├─ MSG_VM_*          generic VMM operations
        ├─ MSG_VIBEOS_*      higher-level OS lifecycle
        └─ serial/net/block/framebuffer contracts for device binding
```

## Hot-Swap Pipeline

```
Agent writes WASM to vibe_staging (4MB shmem)
        │
        │  OP_VIBE_PROPOSE
        ▼
  vibe_engine.c
        │  validate: WASM magic, size, required exports
        │  (wasm_validator.rs in sim; C path in kernel)
        │
        │  OP_VIBE_EXECUTE (on approval)
        ▼
  controller.c
        │  vibe_swap_begin()
        ▼
  swap_slot.c  ──▶  wasm3 runtime loads WASM binary
        │            health_check() called
        │            on pass: service goes live
        │            on fail: rollback to prior version
        ▼
  EVENT_SWAP_COMPLETE / EVENT_SWAP_FAILED ──▶ event_bus.c
```

## IPC Contract Locations

Every service must have a contract before it may be called (`AGENTS.md`
API-first rule):

| Service         | Contract                              | Status   |
|-----------------|---------------------------------------|----------|
| CC-PD           | `kernel/agentos-root-task/include/contracts/cc_contract.h` | ✓ |
| guest lifecycle | `kernel/agentos-root-task/include/contracts/guest_contract.h` | ✓ |
| VMM             | `kernel/agentos-root-task/include/contracts/vmm_contract.h` | ✓ |
| vibeOS          | `kernel/agentos-root-task/include/contracts/vibeos_contract.h` | ✓ |
| vibe-engine     | `kernel/agentos-root-task/include/contracts/vibe_engine_contract.h` | ✓ |
| event-bus       | `kernel/agentos-root-task/include/contracts/eventbus_contract.h` | ✓ |
| cap-broker      | `kernel/agentos-root-task/include/contracts/cap_broker_contract.h` | ✓ |
| agentfs         | `kernel/agentos-root-task/include/contracts/agentfs_contract.h` | ✓ |
| serial          | `kernel/agentos-root-task/include/contracts/serial_contract.h` | ✓ |
| net             | `kernel/agentos-root-task/include/contracts/net_contract.h` | ✓ |
| block           | `kernel/agentos-root-task/include/contracts/block_contract.h` | ✓ |
| nameserver      | `kernel/agentos-root-task/include/contracts/nameserver_contract.h` | ✓ |

## Key Invariants

- **seL4 is the only kernel-mode (EL2) component.** Never modified.
- **Capabilities are monotonically decreasing** as they are delegated. No PD
  may escalate its own privileges.
- **Root task distributes, never enforces policy.** Policy is the cap-broker's job.
- **No UI code** in this repository. GUI clients live outside the repo and
  consume CC-PD or IPC contracts.
- **Every API must have a contract** under
  `kernel/agentos-root-task/include/contracts/` before anything may call it.
- **Generic device rule:** serial, net, block, USB, timer, entropy each have exactly
  one canonical PD in `services/`. Custom implementations require an approved defect.

## See the live architecture

Run `make demo` to boot Ubuntu and FreeBSD concurrently on one agentOS image.
The command requires authenticated, key-only SSH to both guests before it
prints manual login commands and leaves them running. This exercises the path
from QEMU hardware, through agentOS-owned driver and virtualizer PDs, into each
guest's emulated virtio devices.

Use `make demo-test` for the same non-interactive acceptance gate or
`make demo-smoke` for host-only logic checks. The complete walkthrough and
proof boundary are in [`../demo.md`](../demo.md).
