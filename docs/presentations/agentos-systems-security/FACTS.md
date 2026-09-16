# Factual Claim Ledger

Refresh this ledger from the named authority before rendering a release
edition. “Planned” and “under qualification” are not synonyms for shipped.

| Claim | State | Authority or required evidence | Boundary |
| --- | --- | --- | --- |
| seL4 is the only kernel-mode component | Current invariant | `CLAUDE.md`, `AGENTS.md`, root-task architecture | Does not make all userspace code formally verified. |
| Services and VMMs communicate through explicit capability-bearing IPC | Current architecture | System descriptions, contract headers, PD implementations | Audit individual shared-memory channels and capability rights before making a least-authority claim. |
| Linux and FreeBSD guest lifecycle paths exist on AArch64 | Target-qualified at `d3da13e1` | Retained `make demo-test` transcript and image hash in `docs/TCB.md` | Concurrent key-only SSH and suspend/resume passed; destroyed slots cannot yet be recreated. A new release requires its own exact-revision proof. |
| Guest GPA and VMM HVA are distinct | Current implementation | Guest memory layout, guest RAM mapper, GPA helpers, host tests | Verify every virtio device before claiming complete elimination of identity assumptions. |
| Net, block, and console use agentOS-owned emulation paths | Current architecture with target-specific proof levels | Service contracts, VMM device implementations, focused target tests | Do not include display in this claim. |
| A desktop workload runs over the authenticated network path | Ubuntu 0.2 scope deferred; pinned Debian follow-on | `make demo-desktop-test` transcript and retained non-empty-frame evidence | Does not prove framebuffer, GPU, keyboard, or pointer virtualization. |
| `framebuffer_pd` provides a live target display path | Planned for 0.4 | Target create/write/flip/read test | Current implementation is predominantly host-tested surface management. |
| Guests receive canonical virtio-gpu and virtio-input devices | Planned for 0.4 | Linux DRM/input enumeration and captured frame | No host display MMIO or IRQ passthrough is permitted. |
| x86_64 guest operating systems run under agentOS | Planned for 0.4 | VMX/EPT target evidence and Linux userspace execution | Current x86 work proves a reduced root-task topology, not guest support. |
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
| Pinned Debian boots through agentOS-owned net, block and console | Live proof passed; acquisition and gate merged in PR #150; two-boot persistence and boot-timing qualification tooling added in PR #151/#154 | Retained `first-dce5b02` image SHA-256 `a2f9f5c49e657d9477ea4d1d877edbced6359601b5fe5fed820a4f8e3cca9061`; console and authenticated SSH transcript; `make test-debian-persistence` requires a synced-SSH witness across a second cold boot; timing receipts support a same-runner comparison | Two-boot persistence and matching Ubuntu/Debian launch-to-SSH receipts still need hosted qualification. Ordinary runs use disposable snapshots. Lifecycle parity and amd64 remain separate requirements, and `task_26e8b1157ffe449483d2fe1c44f2a8be` stays open in the 0.4 milestone. |

## Evidence still required for a fully qualified edition

- A retained successful `make demo-test` transcript from the release revision.
- Desktop process, protocol-handshake, and non-empty-frame evidence.
- This edition's checked release receipt and remote verification, produced
  during publication after its PDF is frozen.
- One real malformed-request or guest-fault recovery trace.
- A current capability path diagram generated from the shipping system
  description rather than redrawn from memory.
