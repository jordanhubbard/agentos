# agentOS

agentOS is a bootable, capability-secured I/O and isolation platform on the
[seL4](https://sel4.systems/) microkernel. It owns devices in user mode and
hosts Linux and FreeBSD as virtio guests. QEMU is the hardware emulator used
for prototyping; the same driver PDs that own QEMU's virtio devices today are
meant to own real devices on a board.

[![License](https://img.shields.io/badge/license-BSD--2--Clause-blue.svg)](LICENSE)
[![Kernel](https://img.shields.io/badge/kernel-seL4-green.svg)](https://sel4.systems)

## What it is

- **seL4** is the only kernel-mode code (EL2 on AArch64). It is never modified.
- The **root task** (`kernel/agentos-root-task/`) distributes untyped memory,
  CSpaces, VSpaces, and initial capabilities to a fixed set of protection
  domains (PDs), then parks. It enforces no policy after spawn.
- **Driver PDs** own exactly one device class each: a device frame plus its
  IRQ. Nothing else maps that frame.
- **Virtualizer PDs** own no device. They are the only multiplexer for a
  class, moving data between clients over sDDF-shaped shared-memory queues
  with seL4 notifications, and speaking the driver's contract on the clients'
  behalf.
- **VMM PDs** run a vCPU and vGIC and **emulate** virtio-mmio net, block, and
  console devices for a guest, using `libvmm`. Guests run unmodified in-tree
  virtio drivers. Guests never see host MMIO.
- **Native agents** are clients of the virtualizers, the same as a VMM
  backend. They are not in the trusted computing base.

```
 Hardware / QEMU
      |
 seL4 (EL2)
   |-- root task
   |-- driver PDs        serial_pd, net_pd, virtio_blk (+ block_pd)
   |-- virtualizer PDs   net_virt, blk_virt         (console: still a library)
   |-- guest_vmm_*       vCPU + vGIC + emulated virtio-net/blk/console
   |     |-- Linux guest
   |     '-- FreeBSD guest
   '-- control           vm_manager, cc_pd, nameserver, log_drain, fault_handler
```

The binding description of the trust boundary, with a strict split between
what boots today and the target shape, is [`docs/TCB.md`](docs/TCB.md). The
project rules are [`CLAUDE.md`](CLAUDE.md) and [`AGENTS.md`](AGENTS.md).

## Today vs. target

| Area | Today | Target |
|------|-------|--------|
| One owner per device frame and IRQ | Held (lint-enforced) | Same |
| Network mux is a separate PD (`net_virt`) | Held | Same |
| Block mux is a separate PD (`blk_virt`) | Held | Same |
| Console mux is a separate PD (`serial_virt`) | Not yet: the console virtualizer is a library inside `guest_vmm`; bytes reach `cc_pd` by IPC | Separate `serial_virt` PD |
| Virtio is the guest ABI; host QEMU virtio is never passed through | Held (`xtask qemu-test` rejects a host-backed proof that used the VMM-local loopback) | Same |
| Guests are image + FDT, no agentOS-specific guest drivers | Held | Same |
| Target hardware | QEMU `virt` AArch64 (full guest path); QEMU x86_64 boots the root task only | Bare metal; x86 guest execution is roadmap 0.4 |
| `vibe_engine` in the image | Booted, not TCB: `cc_pd` relays dynamic-guest creation through it to `vm_manager` | Retired once `cc_pd` calls `vm_manager` directly |

## Booted PD set

The default AArch64 image boots 13 PDs. The list is checked in two places that
must agree: `kernel/agentos-root-task/src/system_desc_aarch64.c` (what the
root task spawns) and `kernel/agentos-root-task/agentos.toml` (what is bundled
into the image). A bundle entry with no descriptor row is never started; a
descriptor row with no bundle entry fails at ELF load.

| PD | Role | Source |
|----|------|--------|
| `nameserver` | Service name registry; spawned first | `kernel/agentos-root-task/src/nameserver.c` |
| `log_drain` | Log ring drain | `kernel/agentos-root-task/src/log_drain.c` |
| `serial_pd` | Owns the PL011 UART frame + IRQ | `services/serial-mux/serial_pd.c` |
| `vibe_engine` | Dynamic-guest relay hop (not TCB) | `services/vibe-engine/vibe_engine.c` |
| `virtio_blk` | Owns QEMU virtio-blk (bus.8) and the bounded DMA window | `kernel/agentos-root-task/src/virtio_blk.c` |
| `block_pd` | Block service | `kernel/agentos-root-task/src/block_pd.c` |
| `net_pd` | Owns QEMU virtio-net (bus.16) | `services/net-service/net_pd.c` |
| `net_virt` | Network virtualizer; no device, no IRQ | `platform/net-virt/net_virt.c` |
| `blk_virt` | Block virtualizer; no device, no IRQ | `platform/blk-virt/blk_virt.c` |
| `guest_vmm_primary` | vCPU, vGIC, emulated virtio for the primary guest | `kernel/agentos-root-task/src/guest_vmm.c` |
| `vm_manager` | Guest lifecycle control (create, bind, status) | `kernel/agentos-root-task/src/vm_manager.c` |
| `cc_pd` | Owns QEMU virtio-serial (bus.2): the harness/`agentctl` console; prints `agentOS boot complete` | `kernel/agentos-root-task/src/cc_pd.c` |
| `fault_handler` | Fault endpoint for the other PDs | `kernel/agentos-root-task/src/fault_handler.c` |

`guest_vmm_secondary`, `fault_inject`, `test_runner`, and `event_bus` are added
only to the image variants that use them (dual-guest, fault-injection, and TAP
test images). The other sources under `kernel/agentos-root-task/src/` and
`services/` are still compiled so they keep building, but they are not in the
image; `docs/TCB.md` lists them as museum code that must not be extended.

## Quick start

Full walkthrough: [`docs/QUICKSTART.md`](docs/QUICKSTART.md). Short form:

Host packages (the Makefile does not install these):

```bash
# macOS
brew install llvm lld qemu dtc libarchive zstd cmake ninja coreutils
# Debian/Ubuntu
sudo apt-get install -y clang lld llvm qemu-system-arm qemu-system-x86 \
    device-tree-compiler libarchive-tools zstd cmake ninja-build curl
# Rust (stable)
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
```

Then:

```bash
make setup                     # build xtask, download Microkit SDK 2.1.0, check tools
make build TARGET_ARCH=aarch64 GUEST_OS=none
make test  TARGET_ARCH=aarch64 GUEST_OS=none   # boot in QEMU, wait for "agentOS boot complete"
make test-host                 # host-only suite + policy check + source lint
make test-guest-net            # Buildroot: one frame through emulated virtio-net
make test-guest-blk            # Buildroot: one request through emulated virtio-blk
make test-guest-console        # Ubuntu initramfs: login over emulated virtio-console
make gate                      # the OS-claim gate: all of the above plus x86_64 boot
```

The Microkit SDK lands in `$HOME/.cache/agentos/microkit-sdk-2.1.0` unless
`SEL4_SDK` points elsewhere. QEMU logs and control sockets go to `build/tmp/`;
set `AGENTOS_TMP_DIR` to a short path when the checkout path is long (macOS
caps Unix socket paths at 104 bytes). `make help` lists every target.

The dual-guest demonstration (`make demo`, `make demo-test`) boots Ubuntu and
FreeBSD concurrently and proves key-only SSH to both. See
[`docs/demo.md`](docs/demo.md). It downloads two ISOs of about 4 GB each and
runs for a long time under TCG.

## Project structure

```
kernel/agentos-root-task/     root task, booted PD sources, IPC contracts
  src/main.c                  boot: parse image, spawn PDs, map regions, park
  src/system_desc_aarch64.c   compile-time PD descriptor table (pd_desc_t)
  agentos.toml                PD bundle manifest (must match the descriptor)
  include/contracts/          packed-struct IPC contracts, one header per PD
kernel/loader/                seL4 loader
platform/                     shared I/O platform
  include/platform/           sDDF-shaped queue layouts (net, blk, serial), GPA translation
  net-virt/                   net_virt PD, hub/loopback pump, VMM virtio-net glue
  blk-virt/                   blk_virt PD, RAM-disk pump, VMM virtio-blk glue
  serial-virt/                VMM virtio-console glue (library, not yet a PD)
  guest-vmm/ guest-ram/       guest-neutral VMM runtime, bounds-checked GPA->HVA
libvmm/                       vendored libvmm (UNSW): vCPU, vGIC, virtio device models
services/                     driver PD sources: net-service (net_pd), serial-mux (serial_pd), vibe-engine
guest-profiles/               versioned TOML guest personalities (buildroot, ubuntu, freebsd, ...)
guest-scenarios/              multi-guest compositions (dual-release.toml)
boards/                       per-board build config (qemu-aarch64, qemu-x86_64, rpi5, intel-nuc, ...)
xtask/                        Rust task runner: build image, QEMU tests, guest fetch, release
tests/                        host suites, source lint, guest-path tests, e2e scripts
tools/                        agentctl and Rust generators (gen-abi, gen-sdf, sign-wasm, ...)
skills/                       text-only operating notes per block (sel4-platform, sddf-net, ...)
docs/                         TCB.md, ROADMAP.md, RELEASES.md, guides; see docs/README.md
```

`contracts/`, `userspace/`, `libs/rust-pd`, `examples/`, and `manifests/` hold
host-side models, simulation crates, and older agent-facing contract drafts.
None of them runs on the booted target.

## Proof levels and status

Host tests stub seL4 IPC and cannot be cited as proof of I/O. Only a booted
image asserted by an automated QEMU test can.

| Level | Meaning |
|-------|---------|
| **guest-proven** | A guest OS booted under agentOS and an automated test asserted real I/O through the emulated device and the virtualizer PD. |
| **boot-proven** | The PD is in the booted image and a QEMU test waited for its marker. Proves load and reachability, not I/O. |
| **host-tested** | Compiled with `-DAGENTOS_TEST_HOST`; seL4 IPC is a stub. Logic only. |
| **lint** | `tests/platform/lint_source_invariants.c` checks the checked-in topology for a `docs/TCB.md` invariant. Not a test. |
| **target** | Described in `docs/TCB.md` or `docs/ROADMAP.md`; not in the booted image. |

| Claim | Level | Evidence |
|-------|-------|----------|
| Root task boots and spawns the PD set, AArch64 | boot-proven | `make test TARGET_ARCH=aarch64 GUEST_OS=none` waits for `agentOS boot complete`; CI `Build & boot test` |
| Root task boots, x86_64 | boot-proven (reduced topology, no guest) | `make test TARGET_ARCH=x86_64 GUEST_OS=none` waits for `[rt] boot complete` with no `[rt] FAULT` |
| Emulated virtio-net through `net_virt` | guest-proven | `make test-guest-net` (Buildroot); CI `Guest virtio-net packet proof` |
| Emulated virtio-blk through `blk_virt` | guest-proven | `make test-guest-blk` (Buildroot); CI `Guest virtio-blk I/O proof` |
| Emulated virtio-console (library path) | guest-proven | `make test-guest-console` (Ubuntu initramfs) |
| Ubuntu on agentOS net + blk + console only, host-backed | guest-proven | `make test-ubuntu-virtio`; CI `Ubuntu agentOS VirtIO net + blk + console proof` |
| Ubuntu Casper live filesystem to login | nightly qualification, not a per-push gate | `make test-ubuntu-live`; `ubuntu-live-nightly.yml` |
| Concurrent Ubuntu + FreeBSD with key-only SSH | acceptance gate, run on demand | `make demo-test` |
| Ubuntu desktop over an SSH tunnel (RFB frame) | experimental | `make demo-desktop-test`; `docs/desktop-demo.md` |
| Console virtualizer as its own PD | target | `docs/TCB.md` |
| Native agent attached to `net_virt`/`blk_virt` queues | target | `kernel/agentos-root-task/src/native_net_client.c` is a host-tested client of the older `net_pd` raw contract; nothing native attaches to a virtualizer yet |
| x86_64 guest execution, virtio-gpu/input, Debian baseline | target | `docs/ROADMAP.md` 0.3 and 0.4 |
| Guest snapshot/restore, live migration | not implemented | `vm_manager.c` returns not-implemented |
| WASM agents, capability hot-swap, agent-facing services (CapStore, MsgBus, ToolSvc, ModelSvc) | museum / host models only | Not in the booted image; see `docs/TCB.md` |

`make gate` is the OS-claim gate: `test-host`, both `GUEST_OS=none` boots, and
`gate-guest-io` (`test-guest-net`, `test-guest-blk`, `test-guest-console`).

## Roadmap and releases

- [`docs/ROADMAP.md`](docs/ROADMAP.md): 0.3 Debian baseline and guest
  graphics, 0.4 x86 VMM, 0.5 persistent x86 desktop, 0.6 Omarchy, 1.0
  dual-architecture qualification, and the trust-baseline corrective actions.
- [`docs/RELEASES.md`](docs/RELEASES.md): evidence-bound release protocol
  (`make release`, `release-prepare`, `release-check`, `release-publish`,
  `release-verify`).
- [`CHANGELOG.md`](CHANGELOG.md): release notes; current line is v0.2.x.
- [`PLAN.md`](PLAN.md): active implementation sequencing.

## FreeBSD guest

FreeBSD 15.0 AArch64 boots as a guest with the same emulated virtio devices
and the same VMM as Linux; `make demo-test` proves it beside Ubuntu.
[`docs/freebsd-vm-guest.md`](docs/freebsd-vm-guest.md) has the standalone
commands; its architecture diagram predates the current PD set.

## Contributing

- Language policy: first-party code is **C, Rust, or Assembly** — target code,
  host tools, tests, generators, and skill helpers alike. No Python,
  JavaScript, Go, Zig, or HTML/CSS anywhere in the repository, including
  vendored code. `make policy-check` enforces this.
- No human UI in this repository. `agentctl` (CLI) is the only in-tree
  consumer; GUIs live in external repositories.
- Contracts before callers: IPC contracts under
  `kernel/agentos-root-task/include/contracts/`, queue layouts under
  `platform/include/platform/`.
- Do not extend museum PDs (`docs/TCB.md`). Do not pass QEMU devices through
  to guests. Do not add guest drivers for a class that has a virtualizer.
- Run `make test-host` before pushing; OS-level claims need `make gate`.
- Tasks are tracked with `mac task` (project `agentos`), not GitHub issues or
  TODO lists. See `AGENTS.md`.
- Developer guide: [`docs/DEVELOPER_GUIDE.md`](docs/DEVELOPER_GUIDE.md).

## License

BSD 2-Clause. See [LICENSE](LICENSE). `libvmm/` carries its own licenses
(see `libvmm/LICENSES/`).

Built on [seL4](https://sel4.systems/) and
[libvmm](https://github.com/au-ts/libvmm) from the Trustworthy Systems group,
UNSW.
