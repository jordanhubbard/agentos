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
| **0.3** | Reproducible Linux and guest graphics | A pinned Debian guest replaces Ubuntu as the cross-architecture integration baseline, the canonical framebuffer is live on target, generic virtio-gpu plus virtio-input virtualizers drive an AArch64 guest without host-device passthrough, and the official Omarchy compatibility ledger is kept current. |
| **0.4** | x86 guest foundation | A real VMX-backed x86_64 VMM boots Linux and reuses canonical net, block, and console services with isolated GPA translation. |
| **0.5** | Persistent x86 desktop platform | A pinned Arch Linux x86_64 guest installs through UEFI, reboots from writable storage, reaches key-only SSH, and runs a Hyprland-class compositor through canonical graphics and input. |
| **0.6** | Official Omarchy qualification | A reproducible official Omarchy artifact installs to encrypted persistent storage, reaches its normal Hyprland desktop, and survives evidence-bound update and recovery gates. |
| **1.0** | Dual-architecture qualification | AArch64 and x86_64 claims, contracts, isolation, lifecycle, guest I/O, release evidence, and maintained technical narrative agree on one immutable revision. |

Dependency order:

```text
dual-guest SSH
      |
      v
0.2 network desktop proof -----> 0.2 systems/security narrative
      |                                  ^
      +---- 0.2 release workflow --------+

0.3 declarative guest profiles -> 0.3 Debian integration baseline
              |                              |
              |                    0.3 framebuffer target proof
              |                              |
              +------------------------------+-- 0.3 Omarchy compatibility ledger
      |
      v
0.3 virtio-gpu + virtio-input -------------------+
                                                   |
one data-driven VMM                               |
      |                                            |
      v                                            |
0.4 VMX/x86 VMM -> UEFI/ACPI -> x86 canonical devices
                         |              |
              resources + writable storage
                         |
                         v
                0.5 persistent Arch SSH
                         |
                         v
             0.5 x86 DRM/input/compositor <-------+
                         |
                         v
       0.6 official artifact -> encrypted install
                         |
                         v
            Omarchy desktop -> update/recovery
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
- `task_c0cd7e7648c348e3a72fd9d66dadc121` — sustain bidirectional virtio-net
  traffic beyond one host RX descriptor cycle and shared-ring window.
- `task_745e6c09bb5f4415ba8707c3d39192d0` — verify a bounded raw RFB frame
  without adding a viewer.
- `task_c5135a55f029480a800796e2b4fe33df` — prove the network desktop session.
- `task_30b9bcb838654949b26fd30595c26c3e` — implement the release workflow.
- `task_f74e7395155a44019774e20ece7390b2` — publish the original,
  evidence-backed systems/security narrative after the proof and release gate
  are true.

## 0.3 — Reproducible Linux and guest graphics

Ubuntu was the v0.2 proof-of-life guest. Its retained evidence remains valid,
but it is not the long-term Linux acceptance baseline. The distribution roles
from v0.3 onward are deliberately separate:

- Buildroot remains the smallest deterministic per-device and boot proof.
- A pinned Debian stable generic image is the cross-architecture integration
  guest for net, block, console, lifecycle, provisioning, and release gates.
- FreeBSD remains the second-kernel compatibility guest.
- Official Arch Linux x86_64 is the desktop-platform precursor in v0.5.
- Official Omarchy is qualified only after that reusable platform passes.

Debian replaces Ubuntu in required gates only after it reaches parity on the
same agentOS-owned VirtIO paths. No v0.2 receipt or historical claim is
rewritten. The initial artifact decision and checksums are recorded in
`docs/linux-guest-baseline.md`.

Guest identity must also stop selecting target implementations. Versioned
TOML source profiles are validated and compiled by the Rust host tooling into
a compact, bounded runtime manifest. The target selects generic boot protocols
and device capabilities from that manifest; artifact acquisition, console
matchers, provisioning, and test recipes remain host-only profile data. If a
recipe needs more than static fields, it uses a finite declarative state
machine interpreted by Rust host tooling, never an arbitrary interpreter in a
VMM or device-service PD. Debian and Omarchy extend reusable Linux/Arch
profiles rather than introducing new VMM implementations.

Linux baseline acceptance evidence:

- the dated Debian image, checksum manifest, provisioning input, extracted
  boot artifacts, and conversion to agentOS block media are reproducible;
- AArch64 reaches key-only SSH through agentOS VirtIO net, block, and console
  without QEMU device passthrough or a guest-specific backend;
- the boot contract is documented and tested rather than assuming that the
  image's firmware environment exists in the current AArch64 VMM;
- cold-boot time and failure diagnostics are retained beside the Ubuntu v0.2
  measurement before Ubuntu leaves required gates;
- the same Debian release and provisioning contract extend to amd64 when the
  v0.4 x86 VMM is ready.

MAC work:

- `task_7f6653b7dcc840b9ab7fa092685c9d57` - collapse Linux and FreeBSD onto
  one flavor-driven VMM implementation.
- `task_a1d4e9d734e246c4a3807d614c2a6dc7` - compile declarative guest source
  profiles into a guest-neutral runtime manifest and host-side test recipes.
- `task_26e8b1157ffe449483d2fe1c44f2a8be` - replace Ubuntu live media with the
  pinned Debian integration guest after parity is proven.

### Guest graphics foundation

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
- `task_93ddbd0f497e4209a162e0f5527fc7cf` — maintain an evidence ledger for
  official Omarchy architecture, artifacts, repositories, and requirements;
  the maintained snapshot is `docs/omarchy-compatibility.md`.

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
- The guest boots through UEFI with generated ACPI and a virtual interrupt
  topology owned by the VMM.
- Net, block, and console requests traverse the same canonical service
  contracts used by AArch64 guests.
- Desktop-class RAM, vCPU, and CPU-feature profiles fail closed when resources
  are unavailable and reclaim all capabilities at teardown.
- Writable guest disks preserve flushed data across reboot and remain isolated
  between concurrent guests.
- Negative tests prove invalid GPA and capability use cannot escape the guest
  domain.

MAC work:

- `task_ede60b058fc745d296bad77044a57420` — implement VMX guest execution.
- `task_5870ef2f51974ffe95099c3032d0f077` — implement UEFI and ACPI guest
  boot.
- `task_3a5da27d553a475092d35a9fa1cb90e9` — port canonical guest devices.
- `task_5a7af19b3d99497a8c13b1f7ac57b230` — add desktop-class resource
  profiles.
- `task_4090f05598f74a9cafbb571a6271b6e5` — provide persistent writable guest
  storage.

## 0.5 — Persistent x86 desktop platform

A pinned vanilla Arch Linux x86_64 image is the distribution gate. It
separates reusable platform work from Omarchy's installer, package repository,
and release cadence. Passing this milestone proves an Omarchy-class machine,
not Omarchy itself.

Acceptance evidence:

- installation to writable block storage and successful reboot;
- key-only SSH after reboot;
- UEFI, ACPI, RAM, vCPU, and CPU-feature evidence;
- DRM/KMS and Hyprland-class compositor session startup;
- keyboard and pointer delivery through canonical services;
- a non-empty frame captured through `framebuffer_pd`;
- explicit artifact and package-set provenance records.

MAC work:

- `task_0d6640b7822d4203b4f099fc66ad5aa9` — boot persistent Arch Linux to
  authenticated SSH.
- `task_c683669084ad40078995b500d1934a59` — qualify x86 DRM, input, and a
  Hyprland-class compositor.

## 0.6 — Official Omarchy qualification

Omarchy support begins only when upstream provides a reproducible official
artifact and architecture-compatible package repository. Community AArch64
ports may inform the compatibility ledger, but they do not satisfy this
milestone and must not be presented as agentOS Omarchy support.

Acceptance evidence:

- checksummed official installation media, package repositories, installer
  revision, and complete resource requirements;
- installation through the canonical UEFI, network, and writable block paths;
- Btrfs and LUKS behavior that does not bypass agentOS capability boundaries;
- reboot to key-only SSH from the installed disk;
- the normal Omarchy Hyprland session on canonical DRM/KMS and input devices;
- injected keyboard and pointer input plus a non-uniform captured frame;
- successful pinned update and reboot;
- snapshot-backed rollback or recovery from an injected failed update;
- retained cold-boot timing from vCPU start to key-only SSH and to the first
  non-uniform compositor frame, compared with the pinned Ubuntu desktop proof;
- exact guest release identity recorded in release evidence.

MAC work:

- `task_67e3fd4664c84a97a41531b1644339cd` — qualify an official reproducible
  Omarchy artifact.
- `task_a2fa1b6bafa34b3293c27279a359e864` — install to encrypted persistent
  storage and reboot to SSH.
- `task_656b748000c94294aaf2ddba22666a96` — prove the official Hyprland desktop.
- `task_3c0aaeeeec54412f86dd69273755feef` — qualify update, rollback, and
  recovery.

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
