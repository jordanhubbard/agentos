# agentOS Quick Start

From a fresh clone to a booted agentOS under QEMU, then the guest I/O proofs.
Every command below is a Makefile target or an `xtask` subcommand that exists
in this tree; run `make help` for the full list.

Supported hosts: macOS (Apple Silicon or Intel) and Linux (x86_64 or
aarch64). FreeBSD hosts cannot run `make sdk` because the Microkit SDK
publishes no FreeBSD archive; point `SEL4_SDK` at a cross-built SDK instead.

## 1. Host packages

The Makefile checks for these tools but does not install them.
`make install` (alias `make deps`) only builds the Rust `xtask` runner.

macOS:

```bash
brew install llvm lld qemu dtc libarchive zstd cmake ninja coreutils
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
```

The Makefile finds Homebrew LLVM under `/opt/homebrew/opt/llvm/bin` (or
`llvm@*`) and `ld.lld` under `/opt/homebrew/opt/lld/bin`. `bsdtar` comes from
`libarchive`.

Debian/Ubuntu:

```bash
sudo apt-get install -y clang lld llvm \
    qemu-system-arm qemu-system-x86 \
    device-tree-compiler libarchive-tools zstd \
    cmake ninja-build curl
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
```

This is the package set the CI jobs install (`.github/workflows/ci.yml`),
plus `zstd`, `cmake`, and `ninja`, which `make demo-check` requires.

Rust stable is enough; no extra targets are needed for the target build.

## 2. Microkit SDK

agentOS builds against one external seL4 Microkit **2.1.0** SDK.

```bash
make sdk
```

`make sdk` downloads
`https://github.com/seL4/microkit/releases/download/2.1.0/microkit-sdk-2.1.0-<platform>.tar.gz`
for your host and unpacks it to `SEL4_SDK`, which defaults to
`$HOME/.cache/agentos/microkit-sdk-2.1.0`. It is idempotent: if
`$SEL4_SDK/board` exists it prints the path and exits.

To use an existing SDK:

```bash
export SEL4_SDK=/absolute/path/to/microkit-sdk-2.1.0
```

CI uses `SEL4_SDK=/tmp/microkit-sdk-2.1.0`; the Makefile default is the
per-user cache above. Both are the same archive.

`make setup` runs `make install`, `make sdk`, and `make demo-check` (a tool
presence check) in one step.

## 3. Boot agentOS with no guest

```bash
git clone https://github.com/jordanhubbard/agentos.git
cd agentos
make setup
make build TARGET_ARCH=aarch64 GUEST_OS=none
make test  TARGET_ARCH=aarch64 GUEST_OS=none
```

`make build` runs the root-task Makefile under
`kernel/agentos-root-task/`, links the PD ELFs, packs them with
`cargo xtask gen-pd-bundle` and `cargo xtask gen-image`, and writes
`build/qemu_virt_aarch64/agentos.img`.

`make test` runs `cargo xtask qemu-test --board qemu_virt_aarch64
--guest-os none` and waits for the serial marker

```
agentOS boot complete
```

which `cc_pd`, the lowest-priority PD in the image, prints right before it
enters its request loop. Exit status 0 means every PD in
`kernel/agentos-root-task/src/system_desc_aarch64.c` loaded and the root task
parked. It proves nothing about I/O: with `GUEST_OS=none` the VMM is a stub.

The x86_64 path boots a reduced root-task topology and waits for
`[rt] boot complete` with no `[rt] FAULT` line:

```bash
make test TARGET_ARCH=x86_64 GUEST_OS=none
```

To watch the boot interactively instead of asserting on it:

```bash
make run GUEST_OS=none        # serial on stdout; exit QEMU with Ctrl-A X
```

## 4. Host-only checks

```bash
make test-host
```

Runs `policy-check` (language and UI policy, `cargo fmt --check` on xtask),
`guest-profile-check` (compiles the guest profiles and runs their host tests),
`lint-source` (the `docs/TCB.md` invariant lint over the compiled topology),
and `test-integration` (host-compiled C suites with seL4 IPC stubbed). It is a
pre-filter, not a boot proof.

## 5. Guest I/O proofs (Buildroot)

```bash
make test-guest-net QEMU_TEST_TIMEOUT=480
make test-guest-blk QEMU_TEST_TIMEOUT=480
```

Both boot a small Buildroot Linux (kernel and rootfs from the libvmm example
images, downloaded on first run into `build/qemu_virt_aarch64/`, about 36 MB
in total; `make clean` removes them) under `guest_vmm_primary`. The guest DTB advertises only agentOS
emulated devices: virtio-net at guest IPA `0x0A010000`, virtio-blk at
`0x0A020000`, virtio-console at `0x0A030000`.

`test-guest-net` passes when the log contains
`emulated virtio-net: guest DRIVER_OK` and one guest frame has gone through
the sDDF queues to `net_virt` and back. `test-guest-blk` passes when
`emulated virtio-blk: guest DRIVER_OK` appears and one request (the kernel's
partition scan of the `blk_virt` RAM disk) completes. Both require
`BOARD=qemu_virt_aarch64` (the default on an AArch64 host; pass it explicitly
on x86_64 hosts).

## 6. Ubuntu initramfs proofs

```bash
make test-guest-console QEMU_TEST_TIMEOUT=480
make test-ubuntu-virtio  QEMU_TEST_TIMEOUT=480
```

Both use the `ubuntu-e2e.toml` profile: the Ubuntu 26.04 arm64 kernel taken
from the official desktop ISO plus a deterministic generated initramfs. On
first run `make fetch-guest` (invoked by `make build`) downloads
`ubuntu-26.04-desktop-arm64.iso` (about 4.2 GB) from `cdimage.ubuntu.com`
into `${AGENTOS_ISO_DIR:-$HOME/.cache/agentos/isos}` and stages extracted
artifacts under `build/guest-images/`.

`test-guest-console` boots to the login prompt over the emulated
virtio-console (`console=hvc0`), injects input through `cc_pd`, and requires
the guest to echo it. `test-ubuntu-virtio` additionally attaches the ISO as
host media owned by `virtio_blk` and requires real I/O markers for all three
classes, including `[net_virt] TX accepted by net_pd` and
`[blk_virt] host media`. Neither QEMU virtio device is visible to the guest.

The full Casper live-filesystem proof, `make test-ubuntu-live
QEMU_TEST_TIMEOUT=7200`, takes up to two hours under TCG and runs nightly
(`.github/workflows/ubuntu-live-nightly.yml`), not per push.

## 7. The gate

```bash
make gate
```

Runs `test-host`, the aarch64 and x86_64 `GUEST_OS=none` boots, and
`gate-guest-io` (`test-guest-net`, `test-guest-blk`, `test-guest-console`).
This is the command that must pass before any OS-level claim is made.

## 8. Dual-guest demo

```bash
make demo-test                 # non-interactive: boot Ubuntu + FreeBSD, prove SSH, exit
make demo                      # same gate, then keep QEMU running and print SSH commands
```

`make demo` needs an interactive terminal, 8 GB of host RAM, roughly 20 GB of
disk, and both ISOs (Ubuntu 26.04 desktop arm64 and
FreeBSD-15.0-RELEASE-arm64-aarch64-dvd1, about 4 GB each). It provisions a
run-specific Ed25519 key under `build/tmp/dual-ssh/` and forwards SSH to
`127.0.0.1:12222` (Ubuntu) and `127.0.0.1:12223` (FreeBSD). The default
deadline is `DUAL_OS_TEST_TIMEOUT=7200` seconds. Press Enter in the `make
demo` terminal to stop QEMU. Details and troubleshooting: `docs/demo.md`.

`make demo-desktop-test` / `make demo-desktop` are the experimental Ubuntu
desktop-over-SSH proof (`docs/desktop-demo.md`). They are not part of the OS
claim.

## 9. Reading the logs

`xtask qemu-test` writes one log per run to `build/tmp/agentos-qemu-*.log`
and puts the control socket beside it (`agentos-qemu-*.cc_pd.sock`). `make
run` writes `build/tmp/agentos-run.log` and exposes `build/cc_pd.sock`.

Set `AGENTOS_TMP_DIR=/path` to move all of these. The path is used for Unix
sockets, so keep it short.

Markers worth searching for:

| Marker | Printed by | Meaning |
|--------|------------|---------|
| `[rt] boot complete` | root task | PD spawn finished (x86_64 gate marker) |
| `agentOS boot complete` | `cc_pd` | lowest-priority PD reached its loop (aarch64 gate marker) |
| `[net_virt] READY` | `net_virt` | virtualizer attached to the shared net frame |
| `emulated virtio-net: guest DRIVER_OK` | `guest_vmm` | guest driver finished feature negotiation |
| `[net_virt] TX accepted by net_pd` | `net_virt` | a guest frame reached the host NIC driver |
| `[blk_virt] host media` | `blk_virt` | block virtualizer is serving real media |
| `[rt] FAULT` | root task | a PD faulted; the x86_64 gate fails on this |

`make -C tools/agentctl` builds the CLI that talks to `cc_pd` over the socket:

```bash
./tools/agentctl/agentctl --batch list-guests
```

## 10. Troubleshooting

**macOS: QEMU fails to bind its socket, or the harness times out
immediately.** macOS limits Unix socket paths to 104 bytes. A checkout under a
deep path (for example a git worktree under `.claude/worktrees/`) exceeds it.
Set a short temp directory:

```bash
export AGENTOS_TMP_DIR=/tmp/agentos
```

**"Microkit SDK not found at ..."** Run `make sdk`, or export `SEL4_SDK`
pointing at a directory that contains `board/`. The cache is
`$HOME/.cache/agentos/microkit-sdk-2.1.0` by default and
`/tmp/microkit-sdk-2.1.0` in CI.

**"Homebrew LLVM not found" on macOS.** `brew install llvm lld`. The
Makefile does not use Apple's `clang`.

**First run is slow.** The Buildroot images are small, but the Ubuntu and
FreeBSD ISOs are about 4 GB each. They are cached in
`${AGENTOS_ISO_DIR:-$HOME/.cache/agentos/isos}` and survive `make clean`;
`make clean-images` removes the staged copies under `build/guest-images/`.

**Timeouts.** `QEMU_TEST_TIMEOUT` (default 300 s) bounds every `xtask
qemu-test` run. Apple Silicon runs AArch64 QEMU under TCG because HVF trips
on seL4's memory access patterns, so use the values in `make help`: 480 s for
the guest proofs, 3600 to 7200 s for `test-ubuntu-live`. Linux hosts use KVM
when `/dev/kvm` exists.

**Ports in use.** `make demo` needs 12222 and 12223 free; `make run` forwards
8789.

**Stale sockets or keys.** `make demo-clean` removes `build/cc_pd.sock`,
`build/agentos-serial.sock`, `build/tmp/dual-ssh/`, and the QEMU logs; guest
image caches are kept.

**Which board.** The guest proofs only run on `qemu_virt_aarch64`. On an
x86_64 host, `make test` defaults to the x86_64 board; pass
`TARGET_ARCH=aarch64` (or `BOARD=qemu_virt_aarch64`) for the AArch64 image.
