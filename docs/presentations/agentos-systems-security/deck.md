# agentOS: Operating Systems as Capability-Scoped Services

Audience: operating-systems implementers, virtualization engineers, and
security reviewers.

Central claim: a guest operating system should be a replaceable,
least-authority component of the machine—not the machine's trusted center.

For operating-systems implementers, virtualization engineers, and security
reviewers: make each guest OS a replaceable, least-authority component rather
than the machine's trusted center.

> Speaker notes: This is a technical argument, not a product launch. Separate
> source-backed current behavior from gates still under qualification and from
> roadmap architecture on every page.

---

## 1. Protection domains separate workloads and device authority

**Current architecture**

agentOS boots on seL4 and places the root task, system services, VMMs, and
agents in separate protection domains. Linux and FreeBSD are workloads above
that boundary.

Each agent, service and VMM receives explicit authority. Scoped AArch64 and
Intel Debian tests now destroy and recreate guests in the same image, with
fresh handles and preserved disk witnesses. That is not yet a claim about
every guest type or physical platform.

> Speaker notes: Do not use numbered rings. On AArch64, seL4 is EL2,
> agentOS PDs are EL0, and guest kernels are EL1 in guest VSpaces.
> Source authority: `CLAUDE.md`, `AGENTS.md`, and
> `kernel/agentos-root-task/`.

---

## 2. Threat model: useful software is not automatically trusted

**Current invariant**

- Guest kernels may be compromised.
- Native agents may be compromised.
- Device services may fail independently.
- Capabilities must decrease as they are delegated.
- No guest receives host device access merely because its driver knows the
  protocol.

**Non-claim**

The current development tree has not completed a production security
evaluation or side-channel analysis.

> Speaker notes: Lead with the negative path. The security value is containment
> after compromise, not a promise that Linux, FreeBSD, drivers, or agents have
> no vulnerabilities.

---

## 3. seL4 is the mechanism, not the marketing adjective

**Current mechanism**

```text
seL4
  |
  +-- root task: initial objects and capability distribution
       |
       +-- policy/services: named IPC contracts
       +-- VMM domains: guest execution and emulated devices
       +-- agent domains: application-specific authority
```

The root task distributes initial capabilities. Policy belongs in dedicated
services. Every cross-domain operation is intended to be an explicit IPC
contract.

> Speaker notes: Show the contract headers and system description during a live
> talk. Avoid saying “formally verified OS”: seL4's verified kernel properties
> do not automatically verify agentOS services or VMMs.

---

## 4. The API is the product surface

**Current direction with implemented contracts**

The external lifecycle vocabulary is create, destroy, status, list, attach,
detach, snapshot, restore, migrate, and configure. Device and system services
have versioned C wire contracts and capability requirements.

**Boundary**

Contract presence, implementation presence, host tests, and target proof are
different evidence levels. The project records those distinctions rather than
treating a header as runtime proof.

> Speaker notes: Source authority lives under `contracts/`,
> `kernel/agentos-root-task/include/contracts/`, and `tests/`. Do not claim
> every lifecycle opcode has equivalent target maturity.

---

## 5. Virtual I/O without surrendering the device

**Current architecture under target qualification**

```text
guest virtio queue
   -> VMM queue validation + GPA translation
   -> net_virt / blk_virt / serial_virt PDs over shared queues
   -> agentOS-owned host backend
```

The guest sees a standard virtual device. The VMM validates descriptors and
translates guest physical addresses. A generic service owns the real backend.
Console uses a separate serial_virt PD. A native Rust PD reaches the same
canonical NIC through its own client page beside a live Ubuntu guest.

> Speaker notes: The key security distinction is emulation versus passthrough.
> Cite the virtio host tests and target evidence specifically.
> AArch64 display/input now have separate guest-visible qualification; physical
> GPU ownership and performance require additional evidence.
> Detailed current and target diagrams: `docs/security-architecture.md`.
> Each VMM maps only its own network and block client pages. The native client
> maps a third network page; driver transfers use a fourth. Target fault probes
> verify forbidden accesses. Virtualizers retain broader mapping authority.
> The native proof qualifies raw Ethernet/ARP, not a production network stack.

---

## 6. Two foreign kernels, one capability system

**Default scenario qualified at a recorded revision**

One AArch64 agentOS instance can create pinned Debian and FreeBSD guests through
CC-PD and `vm_manager`. The acceptance gate requires both to
remain live and accept distinct key-only SSH sessions concurrently.

**Why two guests matter**

A second kernel is not a screenshot feature. It tests whether lifecycle,
memory, console, network, and block abstractions are actually guest-neutral.

> Speaker notes: The gate is `make demo-test`; retain its exact transcript
> before changing this page to “proven in release.” The default scenario passed
> at `67c2fe04`; see `debian-default-scenario.json`. Release integration remains
> pending, and managed Debian reconstruction is a separate proof.

---

## 7. Memory identity is not an API

**Current mechanism**

Guests use conventional guest-physical windows. VMMs map the same frames at
distinct host virtual addresses and translate virtio descriptor addresses
through bounded GPA helpers.

This separation enables:

- per-guest VSpaces;
- non-overlapping host mappings;
- descriptor bounds checks;
- future migration and snapshot work without embedding host addresses in the
  guest ABI.

> Speaker notes: Source authority:
> `platform/include/platform/guest_memory_layout.h`,
> `platform/include/platform/vmm_guest_ram.h`, and virtio GPA helpers. State
> exactly which devices have completed translation.

---

## 8. Failure is part of the protocol

**Current mechanisms**

- malformed or out-of-range requests return contract errors;
- console input is bounded and backpressure-aware;
- CC host frames use explicit framing and retry state;
- guest lifecycle operations have explicit states;
- release gates distinguish host simulation from target behavior.

**Question for reviewers**

When a VMM, device service, guest, or host transport stops responding, which
capabilities and resources remain recoverable?

> Speaker notes: Show one real failure transcript and the corresponding state
> transition. A success-only demo is insufficient evidence for an OS lifecycle
> manager.

---

## 9. Observation can use a client with limited authority

**Implemented and target-qualified**

The native operator reads a root-published, immutable boot snapshot and answers
through its own serial queue page. It receives no guest lifecycle, guest memory
or device capabilities.

Seven target fault probes reject access to guest console pages, the CC frontend
page and writes to the snapshot. A bounded roundtrip test returns 128 exact
reports while exercising backpressure.

> Speaker notes: PR #147 and `docs/TCB.md` retain the proof. Snapshot fields
> describe boot observations, not current thread health or free memory. The
> external CC socket still exposes privileged control operations; a read-only
> command on that socket is not a separate authorization boundary.

---

## 10. Diagnostic identity is assigned outside the producer

**Implemented on AArch64**

Root gives each logging client a private ring and read-only configuration.
The drain assigns identities from that configuration and keeps partial lines
separate. Clients notify the drain without a synchronous call chain.

Malformed cursors are rejected, scans are bounded, and full rings drop new
bytes. The producer still controls its own message contents.

> Speaker notes: PRs #148–149 qualify bounded drain behavior, exact UART output,
> and three native access faults. This makes diagnostics more attributable;
> it does not make client statements trustworthy or prove availability under
> notification flooding. x86 logging remains on its reduced boot path.

---

## 11. Evidence has levels

```text
source contract
   -> host unit test
   -> seL4 target test
   -> guest-visible behavior
   -> concurrent authenticated acceptance
```

**Current policy**

`make test-host` is a fast filter. `make gate` proves both stub-boot targets
and guest network, block, and console behavior. `make demo-test` must pass
before claiming concurrent authenticated dual-guest SSH. The Debian/FreeBSD
scenario passed at `67c2fe04`, including FreeBSD suspend/resume. The full
default-SDK gate passed at `b2b86f5c`. A release still requires evidence for
its exact revision.

> Speaker notes: This page is deliberately about epistemology. “Tests pass”
> means little unless the audience knows what layer the tests execute.

---

## 12. Fast desktop proof: workload first

**Deferred workload proof — pinned Debian follow-on**

The original Ubuntu desktop milestone was deferred before v0.2 shipped.
The pinned Debian path now passes console, authenticated SSH and agentOS-owned
VirtIO net/block/console checks. A graphical session carried through that
network path needs separate retained evidence.

Required proof:

- key-only SSH remains the access boundary;
- the desktop and remote-display processes run in the guest;
- a client completes the remote protocol handshake;
- retained evidence contains a non-empty frame or screenshot checksum.

> Speaker notes: The viewer is external to agentOS. This proves a desktop
> workload and network transport. It does not prove virtual GPU, display,
> keyboard, or pointer devices.

---

## 13. Real display virtualization follows the service boundary

**AArch64 guest-visible qualification**

```text
guest DRM / input drivers
       |
virtio-gpu + virtio-input virtualizers
       |
framebuffer + input service contracts
       |
host display/input driver PDs
```

The guest receives standard virtual devices. It does not receive the host
framebuffer, GPU MMIO, or interrupts. External tools consume exported frames
through a documented API.

> Speaker notes: Two independent managed-recreation runs at `adcf5a80` passed
> exact frame pixels, evdev events, held/disconnected-input release and disk
> witnesses in both generations. GUI main includes the tested native IPC
> consumer. These are software QEMU/X11 results, not physical GPU qualification
> or an input-to-render latency claim.

---

## 14. x86 is a virtualization milestone, not a compiler flag

**0.4 through 0.6 milestones — critical path**

Intel Linux with nested VMX now runs Debian userspace through generated ACPI,
pinned UEFI and canonical network, block and console services. Scoped tests
cover managed recreation, bounded 2 GiB memory, two virtual CPUs, persistent
writes and independent media.

Later milestones still require persistent Arch Linux, x86 graphics/input and a
Hyprland-class compositor. Official Omarchy additionally requires reproducible
artifacts, encrypted installation, update and recovery evidence.

> Speaker notes: Keep the generic x86 root smoke distinct from the Intel VMX
> guest tests. The SDK uses an approved scoped CR2 preservation patch; its
> source, recipe and hashes ship together. Two vCPUs do not establish parallel
> execution on two host cores, arbitrary CPU-feature support or physical
> power-loss durability. Read the revision-specific Intel receipt limits.

---

## 15. Releases bind claims to one revision

**Current release mechanism**

```text
plan (read-only)
  -> prepare (declared files only)
  -> check (exact clean commit + gates)
  -> publish (authorized non-forced writes)
  -> verify (read-only remote evidence)
```

The plan names the claims and their gates. Any change to code, policy, gate
selection, or artifacts invalidates the checked receipt.

> Speaker notes: `xtask release` is the sole mutation authority and the Make
> targets are thin entry points. The checked receipt and remote verification
> for this edition are publication evidence created after the deck is frozen.

---

## 16. What expert review can change

Near-term review questions:

- Are capability delegations minimal and revocable?
- Which shared-memory channels need stronger integrity or availability
  arguments?
- Where can a malicious guest consume unbounded VMM or service resources?
- Which virtio translations still assume trusted descriptor structure?
- What target evidence is necessary before migration and snapshot claims?
- Which x86 virtualization primitives belong in common VMM code, and which
  must remain architecture-specific?

The project invites criticism at contract, mechanism, and evidence boundaries—
before those boundaries become compatibility promises.

> Speaker notes: End with falsifiable engineering questions. The intended
> audience should leave knowing where to inspect and what remains unproven.
