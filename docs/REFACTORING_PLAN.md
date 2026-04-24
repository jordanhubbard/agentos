# agentOS Comprehensive Refactoring Plan

_Generated from full codebase audit + architecture mapping + vibeOS analysis, April 2026_

**Companion docs:** `ARCHITECTURE.md` (PD map), `VIBEOS_ANALYSIS.md` (vibeOS deep dive),
`consolidation-plan.md` (services/ cleanup, partially executed)

---

## Current State Summary

| Metric | Value |
|--------|-------|
| Kernel C PDs | 70 source files, 32K LOC |
| Rust userspace | 6 servers + SDK + sim, 16K LOC |
| Contracts | 55 headers |
| System PDs | 48 (riscv64 default) |
| Build targets | riscv64 (QEMU), aarch64 (rpi5), x86_64, FreeBSD, Linux |
| Open issues | 28 (8 epics, 12 stories, 8 feature requests) |
| Test files | 53 (C tests, Rust tests, shell scripts) |

The codebase is architecturally sound but has accumulated three layers of implementation
per service (kernel PD, Rust sim model, services/ prototype) plus a major in-progress
migration from Microkit abstractions to raw seL4 IPC.

---

## Priority 1: Hygiene & Contract Completeness

**Goal:** Every PD has a contract. Every contract has a source. No zombies.

### 1.1 Add Missing Contracts (17 sources without contracts)

These PDs compile and ship but have no formal IPC interface specification:

| Source | Priority | Notes |
|--------|----------|-------|
| `block_pd` | HIGH | Storage stack needs formal contract |
| `ext2fs` | HIGH | Same — filesystem contract needed |
| `framebuffer_pd` | HIGH | GPU/display contracts essential for vibeOS device binding |
| `cc_pd` | HIGH | Compute context — tied to GPU pipeline |
| `cap_policy` | MED | Security-critical, needs formal interface |
| `cap_audit` | MED | Audit trail needs contract for compliance |
| `cap_accounting` | MED | Resource accounting |
| `crypto_ipc` | MED | Crypto operations |
| `boot_integrity` | MED | Secure boot chain |
| `irq_pd` | LOW | Hardware IRQ — internal, may not need external contract |
| `timer_pd` | LOW | Same |
| `usb_pd` | LOW | USB not wired on riscv64 yet |
| `core_affinity` | LOW | CPU pinning — internal |
| `fault_inject` | LOW | Test-only |
| `gpu_scheduler` | LOW | Internal to gpu_sched |
| `cap_policy_hotreload` | LOW | Internal to cap_policy |
| `monitor` | LOW | System monitor |

### 1.2 Implement Missing Sources (critical contracts without source)

| Contract | Priority | Notes |
|----------|----------|-------|
| `watchdog` | HIGH | System reliability — contract exists, no implementation |
| `freebsd_vmm` | MED | FreeBSD guest support — contract + system file exist |
| `vibeos` | — | Already implemented via vibe_engine PD (contract is the API layer) |

### 1.3 Remove Deprecated Code

| Item | Action |
|------|--------|
| `http_svc.c` + `http_svc_contract.h` | **DELETE** — removed from system file Phase 0.5, "API-first violation" |
| `http-gateway` Rust server | **KEEP** — host-side HTTP bridge, not a PD violation |
| `services/vibe-swap/` | Already deleted (consolidation-plan) |

---

## Priority 2: seL4 Native Migration (in-progress, tracked by GitHub epics)

The major architectural evolution: Microkit abstractions → raw seL4 IPC.

### Epics (from GitHub issues)

| Epic | # | Status | Stories |
|------|---|--------|---------|
| E1: seL4 Native Root Task & Bootstrap | #47 | Open | S5: libmicrokit_compat shim (#59) |
| E2: seL4 Endpoint IPC Framework | #48 | Open | S6: IPC benchmark (#66) |
| E3: Capability Registry & Nameserver | #49 | Open | S3: OP_NS_REVOKE (#69) |
| E4: System Description & Cap Init Tool | #50 | Open | S2: gen-caps (#72), S3: gen-image (#73), S6: remove microkit dep (#76) |
| E5: PD Service Migration | #51 | Open | S4: net stack (#80), S6: vibe stack (#81), S7: GPU stack (#84) |
| E7: VMM Hardening | #53 | Open | S1: libvmm audit (#89), S2: VCPU port (#91) |
| E8: Test Infrastructure | #54 | Open | S2: IPC test suite (#96) |

### Migration Order (recommended)

```
Phase A — Foundation (E1, E2, E4)
  ├── Write root task bootstrap (replaces Microkit loader)
  ├── Implement raw seL4 endpoint IPC framework
  ├── gen-caps + gen-image xtask tools
  └── libmicrokit_compat shim for incremental migration

Phase B — Core Services (E3, E5 partial)
  ├── NameServer raw IPC (capability revocation)
  ├── EventBus raw IPC
  ├── SpawnServer raw IPC
  └── Absorb msgbus endpoint-pool into NameServer

Phase C — Device & Runtime (E5, E7)
  ├── Net stack (net_server, net_pd, wg_net)
  ├── Vibe stack (vibe_engine, vibe_swap, swap_slots)
  ├── GPU stack (gpu_sched, framebuffer_pd, serial_pd)
  └── VMM layer (libvmm audit → raw VCPU)

Phase D — Validation (E8)
  ├── IPC correctness + cap delegation tests
  ├── Benchmark suite
  └── Remove microkit-sdk dependency (#76)
```

---

## Priority 3: Consolidation (from consolidation-plan.md)

### 3.1 Port Security Logic from services/ to Kernel PDs

| Source | Target | What to Port |
|--------|--------|-------------|
| `services/capstore/capstore.c` | `cap_broker.c` | Cascading revocation, rights-subset derivation |
| `services/msgbus/msgbus_seL4.c` | `nameserver.c` | Endpoint pool allocator, per-agent registry, RPC path |

### 3.2 Rust Sim Model Alignment

The Rust `userspace/servers/` crates model service behavior for testing but:
- `sim/` has its own `SimCapStore`/`SimEventBus` that DON'T import the server crates
- Risk of C PD and Rust model diverging silently

**Action:** Either (a) make sim import the server crates as the single source of truth,
or (b) generate Rust models from C contract headers to keep them in sync.

---

## Priority 4: Feature Gaps (from VIBEOS_ANALYSIS.md)

| Gap | Severity | Action |
|-----|----------|--------|
| No RISC-V guest support | HIGH | Add VIBEOS_ARCH_RISCV64 + riscv64 VMM PD |
| 4 hardcoded swap slots | MED | Dynamic swap slot allocation via spawn_server |
| wasm3 interpreter only | MED | Evaluate WAMR/wasmtime for better performance |
| ELF module type not implemented | MED | MODULE_TYPE_ELF=2 defined, no runtime |
| watchdog contract, no source | MED | Implement watchdog PD |
| freebsd_vmm contract, no source | LOW | FreeBSD guest support (contract + system file ready) |

---

## Priority 5: Build & CI

| Issue | Action |
|-------|--------|
| #1: `-Wall -Werror` clean build | Fix remaining warnings, enable globally |
| Cargo workspace: console/dashboard commented out | Already cleaned (aos-2) |
| `--no-recurse-submodules` required | Fix or remove libvmm submodule |
| No CI pipeline | Set up GitHub Actions: build riscv64/aarch64, run tests, cargo check |

---

## Priority 6: Standalone Feature Requests (GitHub)

| # | Feature | Notes |
|---|---------|-------|
| #15 | GPU passthrough (virtio-gpu / VFIO) | Requires IOMMU support in PD |
| #13 | Tailscale/WireGuard integration | wg_net PD exists, needs Tailscale control plane |
| #12 | IPC bridge: native agentOS ↔ Linux VM | Requires shared-memory ring between host PD and guest |
| #11 | Rust runtime for native PDs | `no_std` Rust PDs — needs seL4 Rust bindings |
| #10 | Node.js runtime (V8/QuickJS) | ⚠️ AGENTS.md says no JS in core — QuickJS as WASM module? |
| #9 | Linux VM per-agent isolation | Multi-guest VMM with per-agent network namespaces |
| #35 | Generic device mandate waiver process | Policy for exceptions to non-reinvention rule |

---

## Execution Summary

```
NOW (Sprint 1-2):
  ├── Delete http_svc.c + contract (dead code)
  ├── Add contracts for block_pd, ext2fs, framebuffer_pd, cc_pd
  ├── Implement watchdog PD
  ├── Port capstore cascading revocation → cap_broker.c
  └── -Wall -Werror clean build

NEXT (Sprint 3-4):
  ├── E1: Root task bootstrap
  ├── E4: gen-caps + gen-image tools
  ├── Port msgbus endpoint-pool → nameserver
  └── GitHub Actions CI

LATER (Sprint 5+):
  ├── E2-E3: Raw IPC framework + nameserver upgrade
  ├── E5: PD migration waves (net → vibe → GPU)
  ├── E7: VMM hardening
  └── E8: Test infrastructure
```

---

_This plan should be reviewed and updated as epics close. The GitHub issues are the
authoritative backlog; this document provides strategic context and ordering rationale._
