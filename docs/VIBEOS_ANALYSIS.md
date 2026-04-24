# VibeOS Interface & Dynamic OS Creation — Analysis

_Auto-generated from contract + source audit, April 2026_

## What VibeOS Is

VibeOS is agentOS's top-level OS management API. It composes three subsystems into a
single IPC interface that lets any agent with `SpawnCap` create, manage, and destroy
**entire OS stacks** using only IPC messages. No human kernel engineer required.

The three subsystems:
1. **VM Manager** — guest VM lifecycle (create, boot, pause, snapshot, migrate)
2. **Guest Binding Protocol** — device PD discovery, VirtIO transport, capability tokens
3. **VibeEngine** — WASM service hot-swap (validate, propose, sandbox, commit/rollback)

## The Stack

```
         Agent (with SpawnCap)
              │
              ▼
    ┌─────────────────┐
    │    VibeOS API    │  ← vibeos_contract.h (13 opcodes, 0x24xx range)
    │  MSG_VIBEOS_*    │
    └────────┬────────┘
             │ delegates to
    ┌────────┴────────────────────────┐
    │                                 │
    ▼                                 ▼
┌──────────┐  ┌───────────┐  ┌──────────────┐
│ VM Mgr   │  │ Guest PD  │  │ VibeEngine   │
│ 0x10-1E  │  │ 0x2Axx    │  │ 0x05xx/29xx  │
│ (C PD)   │  │ (C PD)    │  │ (C+Rust)     │
└────┬─────┘  └─────┬─────┘  └──────┬───────┘
     │              │               │
     ▼              ▼               ▼
┌─────────┐  ┌───────────┐  ┌──────────────┐
│ linux_   │  │ Device PDs│  │ Swap Slots   │
│ vmm /    │  │ net, blk, │  │ 0-3 (WASM3)  │
│ freebsd_ │  │ serial,fb │  │              │
│ vmm      │  │           │  │              │
└──────────┘  └───────────┘  └──────────────┘
```

## OS Creation Sequence (MSG_VIBEOS_CREATE)

Per vibeos_contract.h, creating an OS instance:

1. **Allocate swap slot** from swap_slot_0–3 pool
2. **Configure VMM PD** — set arch (aarch64/x86_64), RAM, MCS scheduling budget
3. **Wire device PDs** — based on `device_flags` bitmask (serial, net, block, usb, fb)
4. **Execute guest binding protocol** (guest_contract.h §3.1):
   - Subscribe to EventBus for EVENT_GUEST_READY
   - Query AgentFS `/devices` namespace for available device PD endpoints
   - Send MSG_<DEVICE>_OPEN to each device PD (badge-validated)
   - Register resource requirements with QuotaPD
   - Publish EVENT_GUEST_READY to EventBus
5. **Hot-load WASM service** if `wasm_hash != 0` (via vibe_swap)
6. **Publish EVENT_VIBEOS_READY** to EventBus
7. **Return vibeos_handle** to caller

## Supported Guest Types

| OS Type | Arch | VMM PD | System File |
|---------|------|--------|-------------|
| Linux | aarch64 | linux_vmm | agentos-linux-x86.system (manifest) |
| FreeBSD | x86_64 | freebsd_vmm | agentos-freebsd.system (manifest) |

## Device PD Abstraction

VibeOS enforces a **non-reinvention principle**: guest OSes MAY NOT implement device
classes for which a generic device PD already exists. Instead they get a VirtIO
transport backed by the system's device PDs:

| Device Flag | Func Class | PD | Notes |
|-------------|------------|-----|-------|
| SERIAL (bit 0) | 0x01 | term_server | UART emulation |
| NET (bit 1) | 0x02 | net_server | virtio-net backend |
| BLOCK (bit 2) | 0x03 | block_pd → virtio_blk | Storage |
| USB (bit 3) | 0x04 | usb_pd | USB passthrough |
| FB (bit 4) | 0x05 | framebuffer_pd | Display output |

The `MSG_VIBEOS_CHECK_SERVICE_EXISTS` opcode lets agents query whether a ring-0
service already handles a function class before attempting to provide one.

## VibeEngine: Hot-Swap Protocol

The core innovation. Agents can propose new service implementations:

```
Agent → ModelProxy: "Generate a better network driver"
Agent → VibeEngine::validate(wasm_binary_hash)
      ← ok + capability requirements bitmask
Agent → VibeEngine::propose(service_id, flags)
      ← proposal_id
      [VibeEngine sandboxes + tests the module]
Agent → VibeEngine::commit(proposal_id)
      ← ok
      [Controller → swap_slot: atomic service switch]
      [VibeEngine monitors health → auto-rollback on failure]
```

Key invariants:
- VALIDATE must succeed before PROPOSE
- At most one proposal per swap slot in-flight at a time
- HOTRELOAD is a fast path when layout/caps are compatible
- HOTRELOAD_FALLBACK on mismatch (caller tears down slot)
- REGISTRY_STATUS/QUERY are read-only

### Swap Slot Internals

Each `swap_slot_N` PD:
- Links against `wasm3_host.c` (WASM3 interpreter)
- Loads WASM bytes from a shared memory code region
- Supports `init`, `health_check`, and `ppc` (protected procedure call) entry points
- Has 4 states: EMPTY → LOADING → LIVE (or FAILED) → STANDBY
- Health checks run post-swap; failure triggers auto-rollback

### Module Types

| Type | Value | Runtime |
|------|-------|---------|
| WASM | 1 | wasm3 interpreter in swap_slot |
| ELF | 2 | native PD (future — requires PD lifecycle management) |

## VM Manager Operations

Full VM lifecycle via vm_manager_contract.h:

| Opcode | Name | Phase |
|--------|------|-------|
| 0x10 | CREATE | Allocate VM slot, kernel + initrd from AgentFS |
| 0x11 | DESTROY | Reclaim all capabilities |
| 0x12 | START | Boot vCPUs |
| 0x13 | STOP | ACPI shutdown |
| 0x14 | PAUSE | Halt vCPUs, preserve state |
| 0x15 | RESUME | Resume paused vCPUs |
| 0x16 | CONSOLE | Attach/detach serial console ring |
| 0x17 | INFO | Query VM state + resource usage |
| 0x18 | LIST | List all VM instances |
| 0x19 | SNAPSHOT | Checkpoint to AgentFS |
| 0x1A | RESTORE | Restore from AgentFS snapshot |
| 0x1B | ATTACH | Attach device service |
| 0x1C | DETACH | Detach device service |
| 0x1D | MIGRATE | Live migration to another domain |
| 0x1E | CONFIGURE | Modify VM params without destroy |

## Rust Userspace (Host-Side)

The Rust vibe-engine server (1,903 LOC) is a **simulation model**, not deployed on seL4:
- `lib.rs` (1,267 LOC): Full hot-swap protocol with proposal management
- `wasm_validator.rs` (636 LOC): WASM binary validation (capability safety, bounds, interface)

The model-proxy server (1,807 LOC) provides capability-gated LLM inference:
- Routes to OpenAI-compatible, NIM, Anthropic, local GPU, peer agents, or cache
- Enforces per-agent token budgets via capability periods
- Content-addressable caching via AgentFS

## Assessment

### What's Solid
1. **Contract-first design** — All 3 subsystems have detailed IPC contracts with clear invariants
2. **Layered architecture** — VibeOS composes VM Manager + Guest Protocol + VibeEngine cleanly
3. **Non-reinvention enforcement** — `CHECK_SERVICE_EXISTS` prevents duplicate device drivers
4. **Rollback safety** — WASM sandbox + health checks + auto-rollback is well-designed
5. **Capability gating** — SpawnCap, ModelCap, cap_token per device — proper least-privilege
6. **Live migration** — MSG_VIBEOS_MIGRATE with target_node for multi-node mesh

### What's Missing / Needs Work
1. **No RISC-V guest support** — Only aarch64 + x86_64 defined. The default build target is riscv64 but vibeOS can't create riscv64 guests
2. **4 swap slots hardcoded** — swap_slot_0–3. May need dynamic allocation for serious workloads
3. **ELF module type defined but not implemented** — MODULE_TYPE_ELF=2 exists in contract, no runtime support
4. **wasm3 is a slow interpreter** — No JIT. Service performance ceiling is low. Consider WAMR or wasmtime for production
5. **freebsd_vmm has contract but no source** — Only linux_vmm.c exists
6. **watchdog contract exists with no implementation** — Critical for production reliability
7. **RISCV64 build excludes VM PDs** — The default system file has no VMM PDs at all; vibeOS OS creation would fail on riscv64
8. **Rust sim models drift risk** — Rust vibe-engine and C vibe_engine could diverge since they're separate implementations
