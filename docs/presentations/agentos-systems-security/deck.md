# agentOS: Operating Systems as Capability-Scoped Services

Audience: operating-systems implementers, virtualization engineers, and
security reviewers.

Central claim: a guest operating system should be a replaceable,
least-authority component of the machine—not the machine's trusted center.

> Speaker notes: This is a technical argument, not a product launch. Separate
> source-backed current behavior from gates still under qualification and from
> roadmap architecture on every page.

---

## 1. The unit of isolation is an operating system

**Current architecture**

agentOS boots on seL4 and places the root task, system services, VMMs, and
agents in separate protection domains. Linux and FreeBSD are workloads above
that boundary.

The design question is not “how do we put agents in containers?” It is “what
authority does each agent, service, and guest OS need, and how is every
delegation revoked?”

> Speaker notes: Do not call the ring labels hardware rings. seL4 is the only
> kernel-mode component; the numbered outer rings in project diagrams describe
> decreasing authority. Source authority: `CLAUDE.md`, `AGENTS.md`, and
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
      |
      v
VMM queue validation + GPA translation
      |
      v
generic serial / net / block service contract
      |
      v
agentOS-owned host backend
```

The guest sees a standard virtual device. The VMM validates descriptors and
translates guest physical addresses. A generic service owns the real backend.
The same service boundary can serve native agents.

> Speaker notes: The key security distinction is emulation versus passthrough.
> Cite the virtio host tests and target evidence specifically. Do not imply the
> future display path is already at this maturity.

---

## 6. Two foreign kernels, one capability system

**Current release gate under qualification**

One AArch64 agentOS instance can create Linux and FreeBSD guests through
CC-PD, `vibe-engine`, and `vm_manager`. The acceptance gate requires both to
remain live and accept distinct key-only SSH sessions concurrently.

**Why two guests matter**

A second kernel is not a screenshot feature. It tests whether lifecycle,
memory, console, network, and block abstractions are actually guest-neutral.

> Speaker notes: The gate is `make demo-test`; retain its exact transcript
> before changing this page to “proven in release.” The open qualification work
> is MAC task `task_73b8e18d4e71424fb8223d7e555b1831`.

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

## 9. Evidence has levels

```text
source contract
      |
host unit test
      |
seL4 target test
      |
guest-visible behavior
      |
concurrent authenticated acceptance
```

**Current policy**

`make test-host` is a fast filter. `make gate` supports root-task and service
claims. `make demo-test` supports the dual-guest SSH claim. A broader statement
requires the broader gate.

> Speaker notes: This page is deliberately about epistemology. “Tests pass”
> means little unless the audience knows what layer the tests execute.

---

## 10. Fast desktop proof: workload first

**0.2 milestone — not yet a framebuffer claim**

Start a real graphical session in the Ubuntu ARM64 guest and carry its
remote-display protocol through the existing authenticated SSH path.

Required proof:

- key-only SSH remains the access boundary;
- the desktop and remote-display processes run in the guest;
- a client completes the remote protocol handshake;
- retained evidence contains a non-empty frame or screenshot checksum.

> Speaker notes: The viewer is external to agentOS. This proves a desktop
> workload and network transport. It does not prove virtual GPU, display,
> keyboard, or pointer devices.

---

## 11. Real display virtualization follows the service boundary

**0.3 milestone — planned**

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

> Speaker notes: Current `framebuffer_pd` maturity is predominantly host-tested
> surface management. State that limitation before showing the planned path.

---

## 12. x86 is a virtualization milestone, not a compiler flag

**0.4 and 0.5 milestones — critical path**

The reduced x86_64 root-task topology already supports build and smoke work.
Guest support still requires:

1. VMX/vCPU and EPT-backed guest isolation;
2. interrupt virtualization and x86 boot descriptions;
3. architecture-neutral guest flavors;
4. canonical virtio net, block, and console reuse;
5. persistent x86 Linux and authenticated SSH;
6. graphics/input before an Omarchy desktop claim.

> Speaker notes: Do not call the present x86 target “guest support.” The
> acceptance line is userspace execution plus device, lifecycle, and isolation
> evidence.

---

## 13. Releases bind claims to one revision

**0.2 release-engineering milestone**

```text
plan (read-only)
  -> prepare (declared files only)
  -> check (exact clean commit + gates)
  -> publish (authorized non-forced writes)
  -> verify (read-only remote evidence)
```

The plan names the claims and their gates. Any change to code, policy, gate
selection, or artifacts invalidates the checked receipt.

> Speaker notes: Today the repository has overlapping shell and Rust release
> mutation paths. `docs/RELEASES.md` records the replacement protocol; do not
> present it as implemented until its MAC task closes.

---

## 14. What expert review can change

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
