# Factual Claim Ledger

Refresh this ledger from the named authority before rendering a release
edition. “Planned” and “under qualification” are not synonyms for shipped.

| Claim | State | Authority or required evidence | Boundary |
| --- | --- | --- | --- |
| seL4 is the only kernel-mode component | Current invariant | `CLAUDE.md`, `AGENTS.md`, root-task architecture | Does not make all userspace code formally verified. |
| Services and VMMs communicate through explicit capability-bearing IPC | Current architecture | System descriptions, contract headers, PD implementations | Audit individual shared-memory channels and capability rights before making a least-authority claim. |
| Linux and FreeBSD guest lifecycle paths exist on AArch64 | Current, release gate under qualification | `vm_manager`, both VMMs, `make demo-test` transcript | Concurrent key-only SSH remains the acceptance boundary. |
| Guest GPA and VMM HVA are distinct | Current implementation | Guest memory layout, guest RAM mapper, GPA helpers, host tests | Verify every virtio device before claiming complete elimination of identity assumptions. |
| Net, block, and console use agentOS-owned emulation paths | Current architecture with target-specific proof levels | Service contracts, VMM device implementations, focused target tests | Do not include display in this claim. |
| A desktop workload runs inside Ubuntu over the authenticated network path | Planned for 0.2 | `make demo-desktop-test` transcript and retained non-empty-frame evidence | Does not prove framebuffer, GPU, keyboard, or pointer virtualization. |
| `framebuffer_pd` provides a live target display path | Planned for 0.3 | Target create/write/flip/read test | Current implementation is predominantly host-tested surface management. |
| Guests receive canonical virtio-gpu and virtio-input devices | Planned for 0.3 | Linux DRM/input enumeration and captured frame | No host display MMIO or IRQ passthrough is permitted. |
| x86_64 guest operating systems run under agentOS | Planned for 0.4 | VMX/EPT target evidence and Linux userspace execution | Current x86 work proves a reduced root-task topology, not guest support. |
| Omarchy is a supported agentOS guest | Conditional 0.5 direction | Official reproducible artifact, persistent install, SSH, compositor, input, and frame gates | Do not claim support while official architecture/artifact requirements are unmet. |
| Releases bind claims and gates to one exact revision | Planned for 0.2 | Checked release receipt and remote verification | Current shell and Rust release paths overlap and are not the final authority. |

## Evidence still required for the first edition

- A retained successful `make demo-test` transcript from the release revision.
- Desktop process, protocol-handshake, and non-empty-frame evidence.
- A checked release-plan receipt produced by the replacement Rust workflow.
- One real malformed-request or guest-fault recovery trace.
- A current capability path diagram generated from the shipping system
  description rather than redrawn from memory.
