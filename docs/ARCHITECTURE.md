# agentOS Architecture Map

_Auto-generated from codebase audit, April 2026_

## Overview

agentOS is a seL4 Microkit-based OS for AI agents. ~48K LOC total (32K C kernel, 16K Rust userspace).
48 PDs in the system file, 70 C source files, 55 contracts, 6 Rust simulation servers, 7 tools.

The Rust userspace servers are **NOT deployed on seL4** — they are simulation models
(`capability-broker`, `event-bus`) or host-side services (`http-gateway`, `model-proxy`,
`tool-registry`, `vibe-engine`). The real PD implementations are all in C under
`kernel/agentos-root-task/src/`.

## PD Architecture by Layer

Legend: `[SYS]` = in system file | `[SRC]` = has source | `[CON]` = has contract

### Core / Orchestration
| PD | SYS | SRC | CON | Prio | Notes |
|----|-----|-----|-----|------|-------|
| controller | ✓ | ✓ (main.c) | — | 50→201 | Root coordinator, raised to 201 to prevent priority inversion |
| event_bus | ✓ | ✓ (main.c) | ✓ | 200 | Pub/sub IPC hub, passive |
| init_agent | ✓ | ✓ | ✓ | 100 | Bootstrap agent, first to run |
| worker_0–7 | ✓ | ✓ | ✓ | 80 | 8 worker slots for agent tasks |

### Agent Runtime (vibeOS)
| PD | SYS | SRC | CON | Prio | Notes |
|----|-----|-----|-----|------|-------|
| vibe_engine | ✓ | ✓ | ✓ | 140 | Service hot-swap protocol, passive |
| vibe_swap | — | ✓ | ✓ | — | Swap logic (linked into swap_slots) |
| swap_slot_0–3 | ✓ | ✓ | ✓ | 75 | Dynamic code swap targets |
| agent_pool | — | ✓ | ✓ | — | Agent lifecycle management |

### Storage
| PD | SYS | SRC | CON | Prio | Notes |
|----|-----|-----|-----|------|-------|
| agentfs | ✓ | — | ✓ | 150 | Agent filesystem, passive (no separate .c — part of main?) |
| vfs_server | ✓ | ✓ | ✓ | 138 | Virtual filesystem |
| ext2fs | ✓ | ✓ | — | 132 | ext2 filesystem driver (needs contract) |
| block_pd | ✓ | ✓ | — | 120 | Block device abstraction (needs contract) |
| virtio_blk | ✓ | ✓ | ✓ | 125 | VirtIO block driver |

### Process / Execution
| PD | SYS | SRC | CON | Prio | Notes |
|----|-----|-----|-----|------|-------|
| spawn_server | ✓ | ✓ | ✓ | 170 | Process creation |
| exec_server | ✓ | ✓ | ✓ | 135 | Binary loading/execution |
| proc_server | ✓ | ✓ | ✓ | 145 | Process table management |
| app_manager | ✓ | ✓ | ✓ | 130 | App lifecycle, manifest handling |
| app_slot_0–3 | ✓ | ✓ | ✓ | 110 | Dynamic app execution slots |

### Networking
| PD | SYS | SRC | CON | Prio | Notes |
|----|-----|-----|-----|------|-------|
| net_server | ✓ | — | — | 155 | Network stack (lwip-based, source in lwip_*.c helpers) |
| net_pd | ✓ | — | — | 118 | Low-level network driver |
| net_isolator | ✓ | ✓ | ✓ | 108 | Network namespace isolation |
| wg_net | ✓ | ✓ | ✓ | 105 | WireGuard tunnel PD |
| pflocal_server | ✓ | ✓ | ✓ | 115 | Unix-domain socket emulation |

### Security / Capability
| PD | SYS | SRC | CON | Prio | Notes |
|----|-----|-----|-----|------|-------|
| auth_server | ✓ | ✓ | ✓ | 180 | Authentication |
| cap_broker | — | ✓ | ✓ | — | Capability brokering (linked into controller?) |
| cap_policy | — | ✓ | — | — | Policy engine (needs contract) |
| cap_policy_hotreload | — | ✓ | — | — | Live policy reload |
| cap_audit | — | ✓ | — | — | Audit trail |
| cap_audit_log | — | ✓ | ✓ | — | Persistent audit log |
| cap_accounting | — | ✓ | — | — | Resource accounting |
| crypto_ipc | — | ✓ | — | — | Crypto operations over IPC |
| boot_integrity | — | ✓ | — | — | Secure boot verification |

### GPU / Display
| PD | SYS | SRC | CON | Prio | Notes |
|----|-----|-----|-----|------|-------|
| gpu_sched | ✓ | ✓ | ✓ | 142 | GPU task scheduler |
| gpu_scheduler | — | ✓ | — | — | Scheduler impl (linked into gpu_sched?) |
| gpu_shmem | — | ✓ | ✓ | — | GPU shared memory management |
| framebuffer_pd | ✓ | ✓ | — | 90 | Framebuffer output (needs contract) |
| cc_pd | ✓ | ✓ | — | 85 | Compute context (needs contract) |

### Observability
| PD | SYS | SRC | CON | Prio | Notes |
|----|-----|-----|-----|------|-------|
| log_drain | ✓ | ✓ | ✓ | 160 | Centralized logging, passive |
| trace_recorder | ✓ | ✓ | ✓ | 128 | Distributed tracing |
| mem_profiler | ✓ | ✓ | ✓ | 108 | Memory profiling, passive |
| perf_counters | ✓ | ✓ | ✓ | 95 | Performance counters, passive |
| debug_bridge | — | ✓ | ✓ | — | Debug access (not in system file) |
| monitor | — | ✓ | — | — | System monitor (needs contract) |

### VM Management
| PD | SYS | SRC | CON | Prio | Notes |
|----|-----|-----|-----|------|-------|
| vm_manager | ✓ | ✓ | ✓ | 190 | Guest VM lifecycle |
| vm_snapshot | ✓ | ✓ | ✓ | 100 | VM state snapshots |
| linux_vmm | — | ✓ | ✓ | — | Linux VMM (arch-specific, not in riscv64 system) |
| snapshot_sched | — | ✓ | ✓ | — | Snapshot scheduler |

### System Services
| PD | SYS | SRC | CON | Prio | Notes |
|----|-----|-----|-----|------|-------|
| nameserver | ✓ | ✓ | ✓ | 192 | Service name resolution |
| time_partition | ✓ | ✓ | ✓ | 250 | Time partitioning (highest prio), passive |
| term_server | ✓ | ✓ | ✓ | 100 | Terminal emulation |
| timer_pd | — | ✓ | — | — | Hardware timer (needs contract) |
| irq_pd | — | ✓ | — | — | IRQ dispatch (needs contract) |
| usb_pd | — | ✓ | — | — | USB driver (needs contract) |
| power_mgr | — | ✓ | ✓ | — | Power management |
| oom_killer | — | ✓ | ✓ | — | Out-of-memory handler |

### Resilience
| PD | SYS | SRC | CON | Prio | Notes |
|----|-----|-----|-----|------|-------|
| fault_handler | — | ✓ | ✓ | — | Fault recovery |
| fault_inject | — | ✓ | — | — | Testing: fault injection |
| core_affinity | — | ✓ | — | — | CPU affinity management |
| quota_pd | — | ✓ | ✓ | — | Resource quotas |
| mesh_agent | — | ✓ | ✓ | — | Multi-node mesh |
| watchdog | — | — | ✓ | — | Watchdog (contract only, no source yet) |

### Removed/Deprecated
| PD | SYS | SRC | CON | Notes |
|----|-----|-----|-----|-------|
| http_svc | — | ✓ | ✓ | Removed Phase 0.5 — "API-first violation, superseded by IPC contracts" |

## Gaps

### Sources without contracts (17)
These PDs have implementations but no formal IPC contract header:
`block_pd`, `boot_integrity`, `cap_accounting`, `cap_audit`, `cap_policy`,
`cap_policy_hotreload`, `cc_pd`, `core_affinity`, `crypto_ipc`, `ext2fs`,
`fault_inject`, `framebuffer_pd`, `gpu_scheduler`, `irq_pd`, `monitor`,
`timer_pd`, `usb_pd`

### Contracts without source (16)
These are interface definitions awaiting implementation, or use generic names:
`agentfs` (may be in main.c), `block`, `cc`, `eventbus` (event_bus uses main.c),
`framebuffer`, `freebsd_vmm`, `guest`, `irq`, `net`, `serial`, `timer`, `usb`,
`vibe_engine` (Rust server), `vibeos`, `vmm`, `watchdog`

## Rust Userspace (NOT on seL4)

| Crate | LOC | Purpose |
|-------|-----|---------|
| capability-broker | 692 | Simulation model of CapBroker PD |
| event-bus | 417 | Simulation model of EventBus PD |
| http-gateway | 392 | Host-side HTTP server → agentOS apps |
| model-proxy | 1807 | LLM inference proxy with capability gating |
| tool-registry | 815 | Tool registration and dispatch |
| vibe-engine | 1903 | Vibe coding hot-swap + WASM validator |
| sim | 1065 | Host-side simulation runner |
| sdk | ~800 | Rust SDK (agent_context, capability, event, etc.) |

## IPC Message Space (from agentos.h)

| Range | Domain |
|-------|--------|
| 0x01xx | EventBus |
| 0x02xx–03xx | InitAgent |
| 0x04xx | Events |
| 0x05xx–06xx | VibeSwap |
| 0x07xx | Worker |
| 0x08xx | Spawn |
| 0x09xx | GPU |
| 0x0Axx | Mesh |
| 0x0Bxx | Quota |
| 0x0Cxx | MemProfiler |

## Build Targets

| Arch | System File | Board Configs |
|------|-------------|---------------|
| riscv64 | agentos.system (default) | qemu_virt_riscv64 |
| aarch64 | agentos-aarch64.system | rpi5 |
| x86_64 | agentos-x86_64.system | — |
| FreeBSD | manifests/agentos-freebsd.system | — |
| Linux | manifests/agentos-linux-x86.system | — |
