# agentOS Architecture

## System Layers

```
┌─────────────────────────────────────────────────────────────────────────┐
│  Ring 5 — Agents / Applications                                         │
│                                                                         │
│   Agent binary (WASM or ELF)         agentctl CLI                      │
│   libagent.c (seL4_Call wrappers)    xtask / build tools               │
└────────────────────────────┬────────────────────────────────────────────┘
                             │ seL4 IPC (capability-gated)
┌────────────────────────────▼────────────────────────────────────────────┐
│  Ring 3–4 — Guest OS VMMs                                               │
│                                                                         │
│   linux_vmm.c          freebsd_vmm.c      (future guest VMMs)          │
│   ┌──────────────┐     ┌─────────────┐                                 │
│   │ Linux guest  │     │ FreeBSD     │  virtio devices via seL4 shmem  │
│   │ (EL0 / Ring4)│     │ guest (EL0) │                                 │
│   └──────────────┘     └─────────────┘                                 │
│   gpu_shmem.c (approved custom channel — DEFECT-001)                   │
└────────────────────────────┬────────────────────────────────────────────┘
                             │ seL4 IPC
┌────────────────────────────▼────────────────────────────────────────────┐
│  Ring 2 — System Services (Microkit Protection Domains)                 │
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
│  Ring 1 — agentOS Root Task / Init                                      │
│                                                                         │
│   init_agent.c     controller.c     vibe_engine init                   │
│   Distributes initial capabilities. No policy enforcement.              │
│   Spawns all Ring-2 PDs. Never modified after boot.                    │
└────────────────────────────┬────────────────────────────────────────────┘
                             │ seL4 kernel API (seL4_Call, seL4_Reply …)
┌────────────────────────────▼────────────────────────────────────────────┐
│  Ring 0 — seL4 Microkernel (formally verified, never modified)          │
│                                                                         │
│   Capability system   IPC endpoints   TCB / scheduling   Memory objects │
└─────────────────────────────────────────────────────────────────────────┘
```

## Primary API: vibeOS Lifecycle

The `vibeOS` interface is the main external-facing API. Callers use seL4 IPC
on `CH_VIBEENGINE` with `MSG_VIBEOS_*` opcodes. A C FFI wrapper is available
via `libagent`.

```
Caller (agent / agentctl)
        │
        │  seL4 PPC on CH_VIBEENGINE
        ▼
  vibe_engine.c (Ring 2)
        │
        ├─▶ MSG_VIBEOS_CREATE    → OP_VM_CREATE  ──▶ vm_manager.c
        │                         OP_VM_START    ──▶ vm_manager.c
        │                         MSG_SERIAL_OPEN ─▶ serial_pd.c
        │                         MSG_NET_OPEN  ──▶ net_pd.c
        │                         MSG_BLOCK_OPEN ─▶ block_pd.c
        │                         EVENT_VIBEOS_READY ─▶ event_bus.c
        │
        ├─▶ MSG_VIBEOS_DESTROY   → OP_VM_STOP / OP_VM_DESTROY ─▶ vm_manager.c
        ├─▶ MSG_VIBEOS_STATUS    → OP_VM_INFO ──▶ vm_manager.c
        ├─▶ MSG_VIBEOS_LIST      → local s_vos[] table
        ├─▶ MSG_VIBEOS_BOOT      → OP_VM_START ──▶ vm_manager.c
        ├─▶ MSG_VIBEOS_CONFIGURE → OP_VM_CONFIGURE ─▶ vm_manager.c
        ├─▶ MSG_VIBEOS_SNAPSHOT  → OP_VM_SNAPSHOT ─▶ vm_manager.c
        ├─▶ MSG_VIBEOS_RESTORE   → OP_VM_RESTORE ─▶ vm_manager.c
        ├─▶ MSG_VIBEOS_MIGRATE   → snapshot + destroy + restore
        ├─▶ MSG_VIBEOS_BIND_DEVICE / UNBIND_DEVICE
        └─▶ MSG_VIBEOS_LOAD_MODULE → vibe_swap hot-swap pipeline
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

Every service must have a contract before it may be called (CLAUDE.md §API-First):

| Service         | Contract                              | Status   |
|-----------------|---------------------------------------|----------|
| vibeOS          | `contracts/vibeos/interface.h`        | ✓        |
| vibe-engine     | `contracts/vibe-engine/interface.h`   | ✓        |
| event-bus       | `contracts/event-bus/interface.h`     | ✓        |
| cap-broker      | `contracts/cap-broker/interface.h`    | ✓        |
| agentfs         | `contracts/agentfs/`                  | ✓        |
| serial-mux      | `contracts/serial-mux/`               | ✓        |
| net-service     | `contracts/net-service/`              | ✓        |
| block-service   | `contracts/block-service/`            | ✓        |
| nameserver      | `contracts/nameserver/`               | ✗ todo   |
| http-svc        | `contracts/http-svc/`                 | ✗ todo   |

## Key Invariants

- **seL4 is the only Ring 0 component.** Never modified.
- **Capabilities are monotonically decreasing** as they are delegated down the
  ring hierarchy. No PD may escalate its own privileges.
- **Root task distributes, never enforces policy.** Policy is the cap-broker's job.
- **No UI code, no JavaScript, no Python** anywhere in this repository.
- **Every API must have a contract** in `contracts/<name>/interface.h` before
  anything may call it.
- **Generic device rule:** serial, net, block, USB, timer, entropy each have exactly
  one canonical PD in `services/`. Custom implementations require an approved defect.
