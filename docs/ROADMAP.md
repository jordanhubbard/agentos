# agentOS Release Roadmap

This roadmap records product direction, dependency order, and release
acceptance. MAC is the execution authority: task state, ownership, and
dependencies live in `mac task`. Releases are integration boundaries, not date
promises, and close only when their runtime evidence matches their claims.

The current published line is `v0.1.x`. New work lands on the default branch
first. Patch releases may carry bounded correctness and security fixes without
changing the dependency order below.

## Release map

| Release | Theme | Required outcome |
| --- | --- | --- |
| **0.2** | Network desktop proof and release discipline | Ubuntu exposes a real desktop session over the already authenticated network path; releases become exact-revision, evidence-bound transitions; the first systems/security narrative is grounded in retained evidence. |
| **0.3** | Guest graphics foundation | The canonical framebuffer is live on target, and generic virtio-gpu plus virtio-input virtualizers drive an AArch64 guest without host-device passthrough. |
| **0.4** | x86 guest foundation | A real VMX-backed x86_64 VMM boots Linux and reuses canonical net, block, and console services with isolated GPA translation. |
| **0.5** | x86 distribution and graphical guest | A persistent x86_64 distribution reaches key-only SSH; an official Omarchy image is supported only when reproducible, then reaches a graphical session through agentOS graphics and input. |
| **1.0** | Dual-architecture qualification | AArch64 and x86_64 claims, contracts, isolation, lifecycle, guest I/O, release evidence, and maintained technical narrative agree on one immutable revision. |

Dependency order:

```text
dual-guest SSH
      |
      v
0.2 network desktop proof -----> 0.2 systems/security narrative
      |                                  ^
      +---- 0.2 release workflow --------+

0.3 framebuffer target proof
      |
      v
0.3 virtio-gpu + virtio-input -------------------+
                                                   |
one data-driven VMM                               |
      |                                            |
      v                                            |
0.4 VMX/x86 VMM -> 0.4 x86 canonical devices      |
                         |                         |
                         v                         |
                  0.5 x86 Linux/Omarchy SSH -------+
                         |
                         v
                  0.5 Omarchy desktop
                         |
                         v
                  1.0 qualification
```

## 0.2 — Network desktop proof and release discipline

The fastest desktop proof deliberately does not wait for a virtual display
device. It starts a graphical session inside the existing Ubuntu ARM64 guest
and carries its remote-display protocol through key-only SSH. This proves that
a desktop workload runs above agentOS networking; it does **not** prove
framebuffer, GPU, keyboard, or pointer virtualization.

Acceptance evidence:

- `make demo-test` proves the underlying concurrent authenticated SSH path.
- `make demo-desktop-test` must authenticate, start the desktop and
  remote-display service, complete a protocol handshake, and verify a non-empty
  frame or screenshot checksum.
- `make demo-desktop` must retain the QEMU instance and print one copyable
  tunnel/viewer command. The viewer remains external to this repository.
- The guest package set, RAM requirement, writable-state requirement, ports,
  credentials policy, and cleanup path are documented.

MAC work:

- `task_73b8e18d4e71424fb8223d7e555b1831` — finish the dual-guest authenticated
  SSH prerequisite.
- `task_745e6c09bb5f4415ba8707c3d39192d0` — verify a bounded raw RFB frame
  without adding a viewer.
- `task_c5135a55f029480a800796e2b4fe33df` — prove the network desktop session.
- `task_30b9bcb838654949b26fd30595c26c3e` — implement the release workflow.
- `task_f74e7395155a44019774e20ece7390b2` — publish the original,
  evidence-backed systems/security narrative after the proof and release gate
  are true.

## 0.3 — Guest graphics foundation

This release turns display claims into an agentOS-owned path:

1. A host display driver PD owns the QEMU or hardware display resource.
2. `framebuffer_pd` owns surfaces and bounded frame transfer.
3. Generic virtio-gpu and virtio-input virtualizers translate guest queues to
   canonical service operations.
4. Guest descriptors are always translated and bounds-checked as GPA, never
   treated as trusted host pointers.
5. External consumers may observe exported frames through the API; no viewer
   is added to agentOS.

Acceptance evidence is a target-tested create/write/flip/read cycle followed
by Linux DRM/KMS, keyboard, and pointer enumeration and a captured non-empty
guest frame.

MAC work:

- `task_9cc7b9d4fbd14601b6b0851de4d300b8` — target-test the canonical
  framebuffer.
- `task_cefc0f77327d4245ab9feb132cd1eb57` — implement guest virtio-gpu and
  virtio-input.

## 0.4 — x86 guest foundation

x86 support means guest execution, not merely compiling or booting the reduced
root-task topology. The architecture-neutral VMM runtime and guest-flavor
description come first. The x86 implementation then supplies VMX/vCPU state,
EPT-backed guest isolation, interrupt virtualization, boot protocol, and
architecture-specific fault handling while reusing common lifecycle and
device backends.

Acceptance evidence:

- `make gate` remains green for both root-task architectures.
- A minimal x86_64 Linux guest executes userspace under the seL4 VMM.
- Net, block, and console requests traverse the same canonical service
  contracts used by AArch64 guests.
- Negative tests prove invalid GPA and capability use cannot escape the guest
  domain.

MAC work:

- `task_7f6653b7dcc840b9ab7fa092685c9d57` — make guest flavor data-driven.
- `task_ede60b058fc745d296bad77044a57420` — implement VMX guest execution.
- `task_3a5da27d553a475092d35a9fa1cb90e9` — port canonical guest devices.

## 0.5 — x86 distribution and graphical guest

A conventional persistent x86_64 Linux image is the first distribution gate.
Omarchy follows only after its official artifact, architecture, and
installation requirements are reproducible. Until then, a pinned Arch Linux
desktop may exercise the technical path, but agentOS must not claim Omarchy
support.

Acceptance evidence:

- installation to writable block storage and successful reboot;
- key-only SSH after reboot;
- DRM/KMS and compositor session startup;
- keyboard and pointer delivery through canonical services;
- a non-empty frame captured through `framebuffer_pd`;
- explicit RAM, disk, CPU-feature, and artifact provenance records.

MAC work:

- `task_0d6640b7822d4203b4f099fc66ad5aa9` — prove persistent x86 Linux and
  conditional Omarchy SSH.
- `task_656b748000c94294aaf2ddba22666a96` — prove the graphical Omarchy path.

## 1.0 — Dual-architecture qualification

`task_9b6e2ee0bc5249cab64a402b3cb1ae60` owns the stable release decision. The
release must bind its version, source revision, contracts, gate results,
artifact checksums, limitations, release notes, and presentation edition. A
missing proof is a stated limitation or a release blocker; it is never inferred
from a host test or roadmap entry.

## Roadmap maintenance

- Human maintainers choose when a minor or major release is scheduled.
- MAC tasks own execution. This file records ordering and product boundaries,
  not live completion state.
- Every release outcome names the command and retained artifact that prove it.
- Current behavior, current limitation, near-term investment, and long-term
  direction remain distinct in documentation and presentations.
- Significant scope discovered during a release is assigned to a later
  milestone unless it is required to make an existing claim truthful.
