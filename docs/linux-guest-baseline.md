# Linux Guest Baseline

Status: accepted roadmap decision, implementation in progress

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

The acquisition recipe also retains and pins the complete `SHA512SUMS`
manifest from `cdimage.debian.org`. On 2026-09-14, the cloud download alias
redirected the image to `chuangtzu.ftp.acc.umu.se`, whose TLS certificate
verification failed as expired. The recipe now uses the same dated image on
`laotzu.ftp.acc.umu.se`, with valid TLS and the unchanged SHA-512 pin. Its
manifest was byte-identical to the primary server's manifest. Certificate
verification remains enabled; no moving release alias is introduced.

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

The executable Debian profile now stages and verifies the pinned qcow2,
converts it to writable raw media, extracts its bounded ARM64 root partition,
and obtains the kernel and initrd through generic host recipe actions. Boot,
authenticated SSH, desktop, lifecycle, and cross-architecture evidence remain
qualification work; `status = "runtime"` means the profile is executable, not
that those release claims have passed.

`make test-debian-live QEMU_TEST_TIMEOUT=1800` is the additive single-guest
qualification command. It requires the real Debian console, key-only SSH,
and host-backed network, block and bidirectional console traffic through
agentOS. It does not by itself prove lifecycle parity, persistence across a
second boot, or the required comparison with Ubuntu; Ubuntu remains required.

`make test-debian-persistence QEMU_TEST_TIMEOUT=1800` additionally boots twice
with a managed copy of the writable disk. The first boot writes and syncs a
fresh witness through authenticated SSH; the second reads it after a new QEMU
boot. Both rounds require the console/SSH/VirtIO proofs. The pinned base disk
is never opened for persistent writes. The runner retains the working disk,
source digest, resolved profile, agentOS image, serial logs and result under
`build/evidence/persistent-boot-*`, including failures. It rejects a changed
source disk, image or resolved profile between rounds. This is a cold-boot
storage test, not proof of guest-slot recreation or orderly guest shutdown.

Successful single-profile live tests also write an
`agentos-qemu-*.boot-timing.json` receipt beside the serial log. The monotonic
host clock measures the QEMU launch request through completed authenticated
SSH proof. It excludes acquisition, compilation and persistent-media copying;
it includes host scheduling, QEMU and agentOS startup, guest boot, console
provisioning and SSH authentication. It is not guest CPU time or an exact
vCPU-start timestamp. Persistent tests retain one receipt per successful boot.
Compare Ubuntu and Debian on the same runner, clean agentOS Git revision and
QEMU configuration. The canonical live commands both require the same
host-backed VirtIO assertion and use 3 GiB guest memory. Keep first and
subsequent persistent boots separate. A timing receipt records SSH readiness,
not the success of assertions that run afterward. New receipts require a clean
source tree for the full run and bind the agentOS image plus guest-bundle
SHA-256 values. The expected Ubuntu and Debian guest bundle digests differ, so
they are recorded rather than compared for equality. Older v1 receipts are
deliberately not comparable.

Use the receipt-only comparison after retaining one successful Ubuntu live and
one successful Debian live run on the same recorded host platform:

```sh
make test-guest-boot-timing-compare \
  UBUNTU_BOOT_TIMING_RECEIPT=/absolute/path/agentos-qemu-ubuntu.boot-timing.json \
  DEBIAN_BOOT_TIMING_RECEIPT=/absolute/path/agentos-qemu-debian.boot-timing.json \
  GUEST_BOOT_TIMING_COMPARISON=build/evidence/guest-boot-timing-comparison.json
```

The command fails closed for missing or malformed receipts, a non-authenticated
SSH status or different timing boundary, wrong Ubuntu/Debian profile,
agentOS revision, board, recorded host OS/architecture, timing scope, or
persistent-boot phase, clean source-tree status, or QEMU configuration. It
writes a deterministic comparison receipt containing both receipt hashes,
the immutable image and guest-bundle digests, elapsed times and their unsigned
difference. It sets no performance threshold: a generated receipt is
measurement evidence only, not a target-success or guest-performance claim.
The existing host fields establish a matching recorded host platform; they do
not independently identify a physical runner. No comparison claim exists until
both underlying authenticated SSH runs have completed their full qualification
successfully.

The comparator accepts both historical CLI aliases and the canonical IDs
resolved from the current repository guest profiles. It preserves the original
receipt bytes and their hashes; it does not rewrite historical measurements.

### Spark qualification, 2026-09-16

On Spark (Linux AArch64), both guest qualifications passed at clean revision
`522bd024d934ce3688dfd75e03c5ded204807927` with matching recorded QEMU
configuration. The commands were `make test-debian-persistence
QEMU_TEST_TIMEOUT=1800` and `make test-ubuntu-live QEMU_TEST_TIMEOUT=1800`.
Both proved authenticated SSH and host-backed agentOS VirtIO net/block/console.
Debian also read its disk witness after a second cold boot. This does not prove
guest-slot recreation, orderly guest shutdown, x86 parity or bare-metal Spark
device support.

| First authenticated boot | Elapsed time |
|---|---:|
| Debian stable AArch64 | 643,015 ms |
| Ubuntu live AArch64 | 1,124,868 ms |

These are individual end-to-end observations including provisioning and host
scheduling, with no performance threshold. The receipt-only comparison passed
after correcting the comparator's old-alias-only profile check. The unmodified
input receipts, second Debian boot receipt, persistence result and comparison
are retained in [the evidence directory](evidence/2026-09-16-spark/).
Their image and bundle hashes bind the original runs, not a later release.
The full local artifacts are under `build/evidence/persistent-boot-aaumdV/`
and `build/evidence/v04-spark-ubuntu/`; serial log paths in the receipts refer
to the original runner. The JSON files alone are not substitutes for those
runtime artifacts. Final release qualification must run at the release revision.
