# agentOS Boot Guide

This guide covers prerequisites, build steps, the QEMU boot command, external
tool connections, and expected first-boot output.

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

### Microkit SDK and Artifacts

The build uses one external seL4 Microkit 2.1.0 SDK, shared by every agentOS
worktree. `SEL4_SDK` defaults to
`$HOME/.cache/agentos/microkit-sdk-2.1.0`; set it explicitly to use another
installation:

```bash
export SEL4_SDK=/absolute/path/to/microkit-sdk-2.1.0
make build
```

Generated images, sockets, QEMU logs, guest images, and temporary files belong
under `build/`; do not place binary artifacts in the repository root.

## Build Steps

```bash
# Show maintained top-level targets and current defaults.
make help

# Install host dependencies with brew, apt, or pkg.
make install

# Build and launch the native board in QEMU.
make run
```

`make run` builds the host-native board, stages the selected guest image under
`build/guest-images`, starts QEMU, exposes the CC-PD Unix socket at
`build/cc_pd.sock`, and prints the matching command for the external GUI:

```bash
cd ../agentos_gui && make run
```

To build a specific architecture without launching:

```bash
make build TARGET_ARCH=aarch64 GUEST_OS=ubuntu
make build TARGET_ARCH=aarch64 GUEST_OS=freebsd
make build TARGET_ARCH=x86_64 GUEST_OS=none
make build TARGET_ARCH=riscv64 GUEST_OS=none
```

Guest image staging:

```bash
make fetch-guest GUEST_OS=ubuntu      # Ubuntu 26.04 AArch64
make fetch-guest GUEST_OS=freebsd     # FreeBSD 15.0 AArch64
make bootstrap-guest OS=ubuntu-arm64  # build image from ISO cache
```

To run the QEMU smoke test:

```bash
make test
```

To run seL4-target smoke tests, build the dedicated test image and parse its
TAP sentinel:

```bash
make sel4-test-image
make run-tests
```

`make run-tests` launches the current board in QEMU and waits for
`TAP_DONE:<code>` on the serial log. `TAP_DONE:0` is success; any non-zero code
or a timeout is a failure.

To prove the host API reaches complete guest console boot and supports input:

```bash
make test-guest-login
```

This runs Ubuntu and FreeBSD QEMU boots, drains each serial console via CC-PD,
matches the login or maintenance prompt, injects raw input, and verifies that
the guest echoes it.

## Target Architectures

| `TARGET_ARCH` | Default `BOARD_NAME` | Default `BOARD` | QEMU binary | Scope |
|---|---|---|---|---|
| `aarch64` | `qemu-aarch64` | `qemu_virt_aarch64` | `qemu-system-aarch64` | Full guest path |
| `x86_64` | `qemu-x86_64` | `x86_64_generic` | `qemu-system-x86_64` | root-task smoke path |
| `riscv64` | `qemu-riscv64` | `qemu_virt_riscv64` | `qemu-system-riscv64` | cross-build/test path |

Override via `config.yaml` or on the command line:
```bash
make build TARGET_ARCH=aarch64
```

## QEMU Invocation

Use `make run` as the maintained QEMU entry point. It computes the QEMU
binary, board image, guest block device, CC-PD socket, and port forwards from
the selected target. The important runtime interfaces are:

| Interface | Default |
|---|---|
| agentOS foreground serial | QEMU stdio |
| CC-PD Unix socket | `build/cc_pd.sock` |
| host API forward | `127.0.0.1:8789` |
| Ubuntu SSH forward | `localhost:2222` |
| FreeBSD SSH forward | `localhost:2223` |
| NixOS SSH forward | `localhost:2224` |

Exit foreground QEMU with `Ctrl-A X`.

Apple Silicon uses TCG for AArch64 QEMU because the HVF backend still trips on
seL4's AArch64 memory-access patterns. Linux uses KVM when `/dev/kvm` exists.

## Connecting External Tools

agentOS does not ship an in-repository dashboard. External tools connect to
the exported IPC/API contracts. The default QEMU run exposes `build/cc_pd.sock`
for the host-side CC-PD protocol and forwards the host API port at
`127.0.0.1:8789`.

Reference consumers:

```bash
make -C tools/agentctl
./tools/agentctl/agentctl --batch list-guests
cd ../agentos_gui && make run
```

## Expected First-Boot Output

When agentOS boots successfully in QEMU, exact logs vary by target and guest.
The stable success markers are:

```
[controller] *** agentOS controller boot complete ***
[controller] Ready for agents.
[cc_pd] VirtIO serial ready
```

For guest tests, `make test-guest-login` waits through the CC-PD console API
until Ubuntu 26.04 or FreeBSD 15.0 reaches a login or maintenance prompt, then
injects input and verifies that the guest echoes it. The x86_64 smoke path
checks the root-task marker `[rt] boot complete`.

On x86-64 the maintained scope is intentionally narrower than AArch64: the
root task boots a reduced topology with no service PDs. This avoids running the
AArch64 service endpoint/MMIO layout on QEMU q35 while x86 service mappings are
rebuilt. The x86 gate is therefore root-task boot plus no `[rt] FAULT` endpoint
reports after the marker.

## Protection Domain Layout

The AArch64 guest-capable build boots the root task plus Microkit Protection
Domains for orchestration, storage, device abstraction, VMM management,
observability, and CC-PD host access. The architecture map in
`docs/ARCHITECTURE.md` is the detailed inventory.

Important runtime contracts for external users:

| API surface | Contract/Header |
|---|---|
| CC-PD host bridge | `kernel/agentos-root-task/include/contracts/cc_contract.h` |
| Guest lifecycle | `kernel/agentos-root-task/include/contracts/guest_contract.h` |
| Generic VMM operations | `kernel/agentos-root-task/include/contracts/vmm_contract.h` |
| Serial device API | `kernel/agentos-root-task/include/contracts/serial_contract.h` |
| Framebuffer API | `kernel/agentos-root-task/include/contracts/framebuffer_contract.h` |
| EventBus | `kernel/agentos-root-task/include/contracts/eventbus_contract.h` |

## Known Limitations / Work in Progress

- **Apple Silicon QEMU HVF**: seL4's AArch64 memory patterns trigger an
  assertion failure in QEMU's HVF backend. The Makefile automatically uses
  TCG on Apple Silicon instead. Performance is roughly 10-20x slower than
  native.

- **riscv64 QEMU on macOS**: MacPorts ships `qemu-system-riscv64` but
  Homebrew's QEMU formula (if installed via `make install`) also includes it.
  The Makefile auto-detects via `$(BREW_PREFIX)/bin/qemu-system-riscv64`.
  If riscv64 QEMU is not found after `make install`, check that Homebrew qemu
  is fully linked: `brew link qemu`.

- **Guest VMMs (AArch64 only)**: The `linux_vmm.elf` and `freebsd_vmm.elf`
  protection domains are only built for `BOARD=qemu_virt_aarch64`. The
  x86-64 board includes a stub `linux_vmm.elf` for compatibility with the
  system description file.

- **Guest image selection**: `make fetch-guest` stages Ubuntu 26.04 and
  FreeBSD 15.0 assets into `build/guest-images`. ISOs are cached in
  `AGENTOS_ISO_DIR` (default `${XDG_CACHE_HOME:-~/.cache}/agentos/isos`); on
  cache miss they are downloaded from the vendor's official site
  (`cdimage.ubuntu.com`, `download.freebsd.org`) and persisted there for
  future runs. The VMM build and QEMU runtime use those build-local images
  by default; set `AGENTOS_FREEBSD_IMAGE`/`FREEBSD_IMAGE` only when testing
  a non-default FreeBSD image.

- **WASM agent execution**: `swap_slot` PDs load and execute WASM binaries
  via the embedded wasm3 interpreter. Binaries must be signed with
  `tools/sign-wasm` before deployment.

- **Network stack**: The `net_server` PD provides a TCP/IP stack stub. Full
  network functionality requires a VirtIO NIC (provided by the QEMU `-netdev`
  + `-device virtio-net-device` flags above).

- **Guest OS VMM selection**: Use `GUEST_OS=ubuntu` for the Linux path,
  `GUEST_OS=freebsd` for the FreeBSD path, or `GUEST_OS=both` to build a
  single aarch64 image containing both VMM PDs. `make build` and `make run`
  stage the selected images automatically through `make fetch-guest`.

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
