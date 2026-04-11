# agentOS Boot Guide

This guide covers prerequisites, build steps, the QEMU boot command, connecting
the dashboard, and expected first-boot output.

## Prerequisites

### macOS (Homebrew)

```bash
brew install qemu llvm lld cmake ninja python3 dtc coreutils
# Rust toolchain
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
rustup target add wasm32-unknown-unknown
```

Minimum versions tested:
- QEMU 9.x or later (`brew install qemu`)
- LLVM/clang 18+ (`brew install llvm`)
- lld 18+ (`brew install lld` or `brew install lld@20`)
- CMake 3.18+
- Rust 1.80+

### Linux (Debian/Ubuntu)

```bash
sudo apt-get install -y \
    qemu-system-misc qemu-system-arm qemu-system-x86 \
    clang lld cmake ninja-build python3 device-tree-compiler \
    curl xz-utils
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
rustup target add wasm32-unknown-unknown
```

### Microkit SDK

The Microkit SDK (seL4 Microkit 2.1.0) is downloaded automatically by
`make deps`. It is extracted to `microkit-sdk-2.1.0/` in the project root.
No manual download is required.

If you need to download it manually:

```
https://github.com/seL4/microkit/releases/download/2.1.0/microkit-sdk-2.1.0-<platform>.tar.gz
```

Where `<platform>` is one of:
- `macos-aarch64` — Apple Silicon Mac
- `macos-x86-64` — Intel Mac
- `linux-aarch64` — Linux/AArch64
- `linux-x86-64` — Linux/x86-64

## Build Steps

```bash
# 1. Install all dependencies (downloads Microkit SDK automatically)
make deps

# 2. Build and launch (native arch, hardware-accelerated QEMU + dashboard)
make
```

`make` is equivalent to `make console` which:
1. Builds agentOS for the host's native CPU architecture
2. Launches it headlessly in QEMU (HVF on Intel Mac, KVM on Linux, TCG fallback on Apple Silicon)
3. Starts the agentOS console server
4. Opens `http://localhost:8080` in the default browser

To build a specific architecture without launching:

```bash
make build BOARD=qemu_virt_riscv64    # RISC-V 64-bit
make build BOARD=qemu_virt_aarch64    # AArch64
```

To run the CI boot test:

```bash
make test
```

## Target Architectures

| `config.yaml` `target_arch` | `BOARD` | QEMU binary |
|---|---|---|
| `riscv64` (default) | `qemu_virt_riscv64` | `qemu-system-riscv64` |
| `aarch64` | `qemu_virt_aarch64` | `qemu-system-aarch64` |
| `x86_64` | `x86_64_generic` | `qemu-system-x86_64` |

Override via `config.yaml` or on the command line:
```bash
make build TARGET_ARCH=aarch64
```

## QEMU Invocation

### AArch64 (native on Apple Silicon / Linux AArch64)

Used by `make console` when the host is AArch64:

```bash
qemu-system-aarch64 \
    -machine virt,virtualization=on,highmem=off,secure=off \
    -cpu cortex-a53 \
    -m 2G \
    -display none -monitor none \
    -chardev socket,id=char0,path=/tmp/agentos-serial.sock,server=on,wait=off \
    -serial chardev:char0 \
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:8789-:8789 \
    -device virtio-net-device,netdev=net0 \
    -device loader,file=build/qemu_virt_aarch64/agentos.img,addr=0x70000000,cpu-num=0
```

Note: Apple Silicon uses TCG (software emulation) rather than HVF because
seL4's AArch64 memory-access patterns trigger an assertion failure in QEMU's
HVF backend (`hvf_vcpu_exec` isv assertion in `hvf.c`). This is a known
upstream QEMU issue.

### RISC-V 64 (CI test / cross-build from macOS or Linux)

```bash
qemu-system-riscv64 \
    -machine virt \
    -cpu rv64 \
    -m 2G \
    -nographic \
    -bios /usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin \
    -kernel build/qemu_virt_riscv64/agentos.img
```

On macOS the OpenSBI firmware is at:
```
$(brew --prefix)/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin
```

### x86-64 (Intel Mac, Linux x86)

```bash
qemu-system-x86_64 \
    -machine q35 \
    -cpu host \
    -m 2G \
    -display none -monitor none \
    -serial unix:/tmp/agentos-serial.sock \
    -enable-kvm \
    -netdev user,id=net0,hostfwd=tcp:127.0.0.1:8789-:8789 \
    -device e1000,netdev=net0 \
    -kernel build/x86_64_generic/agentos.img
```

## Connecting the Dashboard

The agentOS console (dashboard + serial bridge) runs at:
```
http://localhost:8080
```

It connects to the agentOS HTTP API at `http://127.0.0.1:8789`.

To start only the dashboard (when agentOS is already running on hardware or
in a separate terminal):

```bash
make dashboard
```

## Expected First-Boot Output

When agentOS boots successfully in QEMU you should see the following on the
serial console (in order):

```
agentOS v0.1.0-alpha
[event_bus] Initializing...
[event_bus] READY
[controller] Waking EventBus via PPC...
[controller] EventBus: READY
[init_agent] Subscribing to EventBus...
[init_agent] EventBus subscription: OK
[controller] *** agentOS controller boot complete ***
[controller] Ready for agents.
[init_agent] Entering event loop. agentOS is ALIVE.
```

The CI test (`make test` / `scripts/ci-test.sh`) checks for these strings and
exits 0 on success or 1 on failure.

## Protection Domain Layout

The default RISC-V build (`agentos.system`) boots the following protection
domains:

| PD | Priority | Role |
|---|---|---|
| `controller` | 50 | System coordinator + capability broker |
| `event_bus` | 200 | Passive pub/sub server (ring buffer) |
| `init_agent` | 100 | Agent ecosystem bootstrapper |
| `worker_0..7` | 80 | Agent pool workers (8 total) |
| `vibe_engine` | 140 | WASM hot-swap lifecycle engine |
| `agentfs` | 150 | Content-addressed object store |
| `swap_slot_0..3` | 75 | WASM hot-swap execution slots |
| `console_mux` | 160 | Session multiplexer (serial/ring) |
| `mem_profiler` | 108 | Heap allocation tracker |
| `net_isolator` | 160 | Per-agent network firewall |
| `nameserver` | 130 | PD name → channel ID lookup |
| `virtio_blk` | 120 | VirtIO block device driver |
| `vfs_server` | 115 | Virtual filesystem server |
| `spawn_server` | 110 | Dynamic PD spawn manager |
| `app_slot` | 85 | Application execution slot |
| `net_server` | 155 | TCP/IP stack |
| `app_manager` | 90 | Installed-app lifecycle |
| `http_svc` | 145 | HTTP service endpoint |
| `trace_recorder` | 128 | Inter-PD dispatch event logger |
| `perf_counters` | 105 | Hardware performance counter reader |
| `time_partition` | 170 | MCS time-partition scheduler |

## Known Limitations / Work in Progress

- **Apple Silicon QEMU HVF**: seL4's AArch64 memory patterns trigger an
  assertion failure in QEMU's HVF backend. The Makefile automatically uses
  TCG on Apple Silicon instead. Performance is roughly 10-20x slower than
  native.

- **riscv64 QEMU on macOS**: MacPorts ships `qemu-system-riscv64` but
  Homebrew's QEMU formula (if installed via `make deps`) also includes it.
  The Makefile auto-detects via `$(BREW_PREFIX)/bin/qemu-system-riscv64`.
  If riscv64 QEMU is not found after `make deps`, check that Homebrew qemu
  is fully linked: `brew link qemu`.

- **Linux VMM (AArch64 only)**: The `linux_vmm.elf` and `freebsd_vmm.elf`
  protection domains are only built for `BOARD=qemu_virt_aarch64`. The
  x86-64 board includes a stub `linux_vmm.elf` for compatibility with the
  system description file.

- **WASM agent execution**: `swap_slot` PDs load and execute WASM binaries
  via the embedded wasm3 interpreter. Binaries must be signed with
  `tools/sign-wasm` before deployment.

- **Network stack**: The `net_server` PD provides a TCP/IP stack stub. Full
  network functionality requires a VirtIO NIC (provided by the QEMU `-netdev`
  + `-device virtio-net-device` flags above).

- **Guest OS VMM**: `make GUEST_OS=linux` (AArch64 only) builds the Linux VMM
  via `libvmm`. `make GUEST_OS=freebsd` builds the FreeBSD VMM. Run
  `make fetch-guest GUEST_OS=linux` or `make fetch-guest GUEST_OS=freebsd`
  first to download the guest images.

## Agent Signing

agentOS verifies the capability manifest of every WASM agent before granting
capabilities.  The verification path is implemented in
`kernel/agentos-root-task/src/verify.c` (`verify_capabilities_manifest`) and
called from `monitor.c` before each `vibe_swap_begin` invocation.

### WASM binary layout

A deployable WASM agent must contain three custom sections (WASM section
type `0x00`):

| Section name              | Size     | Content |
|---------------------------|----------|---------|
| `agentos.capabilities`    | variable | Declared capability bitmask and metadata (agent-defined) |
| `agentos.cap_signature`   | 32 bytes | SHA-256 digest of the `agentos.capabilities` section body |
| `agentos.signature`       | 128 bytes | Ed25519 pubkey (32B) + signature (64B) + SHA-256 of WASM body (32B) |

### Signing flow

1. Compile your WASM agent normally.
2. Append an `agentos.capabilities` custom section declaring the required
   capability bitmask (see `AGENTOS_CAP_*` constants in `agentos.h`).
3. Compute `SHA-256(agentos.capabilities section bytes)` and embed the
   32-byte digest as the `agentos.cap_signature` custom section.
4. Sign the WASM body (excluding the `agentos.signature` section itself)
   with your Ed25519 issuer key and embed the 128-byte payload as the
   `agentos.signature` custom section.

Steps 2-4 will be automated by a `sign-agent` tool (planned for a future
release).  Until then, the reference implementation in `verify.c` documents
the exact byte layout expected by the kernel verifier.

### Verification modes

| `VIBE_VERIFY_MODE` | Missing manifest | Hash mismatch |
|--------------------|-----------------|---------------|
| `0` (dev, default) | warn, load with minimal defaults | warn, allow load |
| `1` (production)   | reject load     | reject load   |

Set `-DVIBE_VERIFY_MODE=1` in the kernel build flags for production
deployments.
