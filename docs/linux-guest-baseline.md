# Linux Guest Baseline

Status: accepted roadmap decision, implementation pending

Snapshot date: 2026-09-08

## Decision

agentOS uses different Linux guests for different evidence:

| Guest | Role | Why |
| --- | --- | --- |
| Buildroot | Minimal device proof | Small, deterministic, and controlled by the repository. |
| Debian stable | Canonical integration and release guest | Official arm64 and amd64 generic images share one stable release and standard kernel. |
| Arch Linux x86_64 | Desktop-platform precursor | Matches Omarchy's package ecosystem without coupling platform bring-up to Omarchy. |
| Omarchy x86_64 | Product qualification guest | Exercises the official installer, Hyprland session, encryption, update, and recovery paths. |

Ubuntu remains the historical v0.2 proof-of-life guest. It is removed from
required gates only after Debian proves equivalent net, block, console,
lifecycle, provisioning, and authenticated SSH behavior. Existing Ubuntu
evidence and release receipts are not rewritten.

Arch Linux ARM is not the canonical AArch64 guest. It is a separately produced
rolling distribution rather than the same official artifact stream used for
Arch Linux x86_64, which weakens cross-architecture provenance and release
reproducibility.

## Initial Debian artifact pin

The initial implementation target is Debian 13 stable's dated generic image
build `20260831-2587`. Use `generic`, not `genericcloud`: the generic image
retains the standard kernel and broad driver set. Do not consume the moving
`latest` aliases in release evidence.

Base URL:

```text
https://cloud.debian.org/images/cloud/trixie/20260831-2587/
```

Pinned images and SHA-512 digests:

```text
debian-13-generic-arm64-20260831-2587.qcow2
27287048285224e662722b54c77849c9bd1a2fd60ccea5f2e75c644fbd638fad7e32a97e8e3d628c8bbfdb9cf757951c8300f3c21758647c8906359d9c892b42

debian-13-generic-amd64-20260831-2587.qcow2
5a069019420fb9441ad4f8004c661fadb747edd5662ca54a17c8f923dee7d717e21dbdaa4ba72d6fce7f920e0217f0a9af382298a7d46ed4bc9dc33ac19181b6
```

The source of record for the digests is the `SHA512SUMS` file in the dated
directory. A later implementation may advance this pin through a reviewed
change that updates the artifact, manifest, and retained boot evidence
together.

## Delivery contract

The Debian artifact is guest media, not a reason to map QEMU devices into the
guest. The implementation must:

1. Fetch the exact dated artifact and verify its SHA-512 digest before use.
2. Reproducibly convert or expand qcow2 content into media served by the
   agentOS block path.
3. Extract or otherwise supply the kernel, initrd, and device description
   required by the existing AArch64 boot contract. Do not assume guest UEFI is
   already present.
4. Provide deterministic cloud-init NoCloud data or an equivalently audited
   pre-provisioning step, with key-only SSH and no default password.
5. Boot using only emulated VirtIO devices backed by agentOS-owned services.
6. Prove ordered network traffic, persistent block behavior, console I/O,
   authenticated SSH, teardown, and a second clean boot.
7. Retain cold-boot timings from vCPU start to login and authenticated SSH,
   compared on the same runner with the Ubuntu v0.2 proof.
8. Reuse the same release, provisioning contract, and acceptance assertions
   on amd64 once the v0.4 VMM supplies the architecture-specific boot path.

## Promotion rule

Debian becomes the required integration guest only when the full parity suite
passes at one immutable revision. Until then, Ubuntu remains the required v0.2
gate and Debian is additive. Buildroot continues to own low-level device proof
regardless of the integration distribution.
