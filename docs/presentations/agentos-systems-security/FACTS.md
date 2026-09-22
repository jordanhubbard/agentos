# Factual Claim Ledger

Refresh this ledger from the named authority before rendering a release
edition. “Planned” and “under qualification” are not synonyms for shipped.

Development checkpoint: 2026-09-22. Final metadata integration and publication
are separate from the completed implementation qualification.
The receipts below qualify their named revisions, not an unpublished v0.4 tag.

| Claim | State | Authority or required evidence | Boundary |
| --- | --- | --- | --- |
| seL4 is the only kernel-mode component | Current invariant | `CLAUDE.md`, `AGENTS.md`, root-task architecture | Does not make all userspace code formally verified. |
| Services and VMMs communicate through explicit capability-bearing IPC | Current architecture | System descriptions, contract headers, PD implementations | Audit individual shared-memory channels and capability rights before making a least-authority claim. |
| Linux and FreeBSD guest lifecycle paths exist on AArch64 | Default Debian/FreeBSD scenario qualified at `67c2fe04`; Debian recreation qualified twice at `adcf5a80` | `docs/evidence/2026-09-19-spark/debian-default-scenario.json`, `graphics-recreation-adcf5a80.json`, `graphics-recreation-repeat.json` | Concurrent SSH and FreeBSD suspend/resume passed. Debian reconstruction retains disk and SSH identity with fresh handles; do not extend that reconstruction claim to FreeBSD or every profile. |
| Guest GPA and VMM HVA are distinct | Current implementation | Guest memory layout, guest RAM mapper, GPA helpers, host tests | Verify every virtio device before claiming complete elimination of identity assumptions. |
| Net, block, and console use agentOS-owned emulation paths | Current architecture with target-specific proof levels | Service contracts, VMM device implementations, focused target tests | Do not include display in this claim. |
| A desktop workload runs over the authenticated network path | Ubuntu 0.2 scope deferred; pinned Debian follow-on | `make demo-desktop-test` transcript and retained non-empty-frame evidence | Does not prove framebuffer, GPU, keyboard, or pointer virtualization. |
| `framebuffer_pd` provides a live target display path | AArch64 target-qualified | `make test-display`; retained PR #176 target proof; guest graphics receipts above | Surface create/write/flip/read, CC export and QEMU scanout are distinct from physical GPU support. |
| Guests receive canonical virtio-gpu and virtio-input devices | AArch64 Debian target-qualified, including managed recreation | `graphics-recreation-adcf5a80.json`, `graphics-recreation-repeat.json` | Exact frame pixels and evdev input/release behavior passed. No physical GPU/input or interactive latency guarantee. |
| x86_64 Debian executes userspace under agentOS | Intel nested VMX target-qualified | `docs/evidence/2026-09-19-spark/intel-current-admission.json`, `intel-current-smp.json`, `intel-current-storage.json` and related scoped receipts | Generated ACPI, pinned UEFI, canonical I/O, bounded 2 GiB profiles and two virtual CPUs are qualified at recorded revisions. This is not arbitrary x86 hardware, x86 desktop or Omarchy support. |
| The SDK is reproducible and contains all required target inputs | Independent Spark/Intel packaging matched all nine distribution files; default-path gate passed at `b2b86f5c`; hosted SDK and SSH qualification passed at `c2a25050` | `docs/evidence/2026-09-19-spark/sdk-qualified-default.json`, September 22 evidence index | Approved scoped CR2 patch atop pinned upstream sources; no claim that the modified kernel inherits upstream formal verification. Release-asset publication is checked separately. |
| The external GUI displays and controls a real guest through binary IPC | GUI `41b6c1e` paired with core `c2a25050` passed on September 22 | `docs/evidence/2026-09-22-release/native-gui.json` | Native Linux/X11 frames, exact input, held-state cleanup on abrupt disconnect, reconnect, suspend/resume and normal shutdown. No physical GPU performance, one-way latency or other window-system claim. |
| Omarchy is a supported agentOS guest | Conditional 0.6 direction | Official reproducible artifact, encrypted persistent install, SSH, compositor, input, frame, update, and recovery gates | Do not claim support while official architecture/artifact requirements are unmet. |
| Releases bind claims and gates to one exact revision | Current implementation | `xtask release`, `docs/RELEASES.md`, host tests | This edition's checked receipt and remote verification are produced during publication, after its PDF is frozen. |
| Queue client pages are isolated against a compromised VMM | Implemented with target fault probes | Root-task network/block/serial mapping branches in `main.c`; eight forbidden-mapping probes per class | Each VMM maps only its own page. Virtualizers retain broader authority; this is not protection against a compromised virtualizer. |
| Console uses a separate virtualizer PD | Implemented and target-qualified | `docs/TCB.md`, console gate, 262,144-byte backpressure proof and dual-guest transcript | CC and VMM exchange payloads through separate pages muxed by serial_virt. Lifecycle control still uses IPC. |
| Native Rust can use the canonical NIC beside a live guest | Target-qualified at `39c4f8bb` | Three fresh native ARP batches interleaved with guest pings after real Ubuntu userspace and authenticated SSH; exact image in `docs/TCB.md` | Raw queues/ARP only; no production TCP/IP stack claim. Native test PDs are absent from default images. |
| The native client cannot map guest queues or driver resources | Ten target fault probes passed | `make test-native-network-isolation`; root checks fault identity, address and access direction | Covers both guest pages, driver transfer page, NIC MMIO and driver DMA; does not prove physical IOMMU confinement. |
| Inspection does not require guest lifecycle or device capabilities | Native operator implemented in merged PR #147 | `make test-operator-session` and `make test-operator-isolation`; `docs/TCB.md` and serial contract v2 | The operator reads immutable boot facts, not live scheduler state. The external CC transport still has privileged control operations. |
| A compromised operator cannot access guest console pages or alter boot observations | Seven target fault probes passed in PR #147 | Root checks fault identity, address and direction; retained operator isolation images | Covers both guest pages, CC frontend page and snapshot writes. Serial virtualizer compromise remains a separate threat. |
| Log identities come from root configuration | Generic AArch64 logging implemented in merged PR #149 | `make test-log-rings`, `make test-log-isolation`; `docs/TCB.md` | Clients control their own message contents. A correct identity does not authenticate the truth of a log message. |
| Malformed logging metadata cannot cause an unbounded drain scan | Host behavior tests and target logging/isolation proof | Cursor validation and per-client line tests; bounded configured-slot scan | Full rings drop new bytes. Fair scheduling under notification flooding is not established; x86 retains its reduced path. |
| Pinned Debian is the default integration guest on AArch64 and the Intel qualification guest | Local cold-boot persistence, timing comparison, default dual scenario and managed recreation passed | `docs/linux-guest-baseline.md`, `docs/evidence/2026-09-19-spark/debian-default-scenario.json` and Intel receipts above | Dated generic images and key-only provisioning; default dual scenario pairs Debian with FreeBSD. Final release-revision qualification and canonical integration remain required. |

## September 22 qualification additions

- `docs/evidence/2026-09-22-release/dual-recreation.json`: local and hosted
  Ubuntu/FreeBSD concurrent SSH, FreeBSD suspend/resume, surviving peer during
  destroy, fresh Ubuntu handle and stale-handle rejection at core `c2a25050`.
  The hosted retry passed; the original timeout remains an undiagnosed failure,
  not evidence of statistical reliability.
- `docs/evidence/2026-09-22-release/native-gui.json`: GUI `41b6c1e` with core
  `c2a25050`, 130 browser tests, 16 Rust tests, native build and real binary-IPC
  frame/input/lifecycle qualification. Abrupt process exit released held keys
  and buttons before the test host released its injected events.
- `docs/evidence/2026-09-22-release/resource-profiles.json`: measured managed
  2 GiB capacity, oversize admission rejection, two-vCPU workloads and fresh
  final-SDK 2 GiB terminal teardown. The latter used `fd7d1dcd`; its only
  difference from `c2a25050` is the dual-guest CI workflow.
- SDK distribution packaging again produced target archive SHA-256
  `fb4290f10c2e59a0baa4d85d477726c3713dec5c497e0d232968bcb6675d566b`.
  All nine distribution files passed the packaged checksum manifest.

## Final release checks and deferred workload evidence

- A retained successful `make demo-test` transcript from the release revision.
- The network-desktop workload remains deferred in `docs/ROADMAP.md`.
  Claiming that workload additionally requires desktop process,
  protocol-handshake and non-empty-frame evidence; direct binary-IPC graphics
  qualification does not establish it.
- This edition's checked release receipt and remote verification, produced
  during publication after its PDF is frozen.

The failure-path slides retain lifecycle rejection and Intel guest-fault
recovery output. `control-topology.mmd` is generated by `make topology-report`
from the compiled default AArch64 system descriptor, restricted to CC, the
manager and the primary VMM. It records initial endpoint grants, including
CC's retained direct VMM endpoint; it does not imply exclusive routing through
the manager or enumerate dynamic queue mappings.
