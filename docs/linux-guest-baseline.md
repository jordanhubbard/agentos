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

The `both` integration scenario now selects the pinned, seeded Debian profile
alongside FreeBSD, so `make demo-test`, `make demo` and `make e2e` use Debian.
This selection follows the recorded [cold boots](evidence/2026-09-19-spark/debian-current-cold-boots.json),
[peer recreation](evidence/2026-09-19-spark/debian-peer-recreation.json), and
[Ubuntu timing comparison](evidence/2026-09-19-spark/guest-current-boot-comparison.json).
Qualification of the changed default remains required. Explicit Ubuntu
per-device, live-media and network-desktop targets remain available; the
minimal console proof is still Ubuntu initramfs. This is integration-scenario
promotion, not a claim that every release gate has completed the migration.

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

The pinned amd64 image was acquired and its SHA-512 verified on Spark on
2026-09-17. Its GPT partition 1 begins at sector 262144 and spans 6027264
512-byte sectors. The ext4 filesystem UUID is
`df4df5bb-613a-4382-8cf9-653304605a1f`. Read-only extraction from that filesystem
produced these unmodified artifacts:

| File under `/boot` | SHA-256 |
|---|---|
| `vmlinuz-6.12.107+deb13-amd64` | `2b2358b37674d2505350528875bb17afae2a36522a9e8a9417eaca65a7da0e08` |
| `initrd.img-6.12.107+deb13-amd64` | `52c4b03c086ad3843dac011f5c09b3d41751543fd7270ec6500269cc4338fd2a` |
| `config-6.12.107+deb13-amd64` | `9b460f1003db4d417288e9c14caecfa14245dab07186df948fef0f3661e38ceb` |

The kernel config enables EFI stub, ACPI, and virtio console as built-ins.
Virtio MMIO, network, block, and ext4 are modules; all four corresponding
modules are present in the stock initrd. This establishes acquisition and
boot-artifact inputs, not an agentOS Debian amd64 boot or SSH qualification.

The experimental Intel root-disk check is `make gate-x86_64-linux-login`.
It requires the existing hash-pinned `X86_FIRMWARE_IMAGE`, `X86_BOOT_KERNEL`,
`X86_BOOT_INITRD`, and `X86_BOOT_CMDLINE_FILE` inputs and a disposable raw
`X86_ROOT_DISK`, which the guest may write. The command-line file must end in
one NUL byte. `X86_BOOT_RAM_BYTES` defaults to `0x10000000u` (256 MiB); the
current EPT layout accepts aligned sizes from 32 MiB through 1 GiB and rejects
overlap with the private VMM ROM mapping. This check records the canonical
virtio-console stream and requires a login prompt, rejecting panic and
emergency-mode output. It does not assert authenticated access.

The initial September 17 Debian attempts were unsuccessful. After removing the small
fixture's exit budget from this mode and handling discovery of an absent
PS/2 controller, both 256 MiB and 1 GiB attempts timed out without console
output. See the [retained attempt record](evidence/2026-09-17-spark/x86-debian-login-attempts.json).
Those failures remain retained rather than being rewritten by later passes.
The September 19 managed Intel results below establish subsequent successful
Debian boots; they do not turn the original attempts into successful runs.

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

`make test-debian-nocloud-cold-boots` applies the same fresh, flushed file
witness to the automatically seeded guest. Both boots authenticate against
the original SSH host identity; only the first writes the witness. The second
fails on a missing or changed file and never repairs it. A failed or uncertain
witness command is not retried. Evidence remains under
`build/evidence/seeded-cold-boots-*`. The first target run at `e0d207f` wrote
and flushed the witness, but systemd froze before login on the second boot.
The [failure receipt](evidence/2026-09-18-spark/seeded-file-cold-boots.json)
retains the exact scope and logs. Seeded file persistence remains unqualified.
The profile rejects systemd's terminal `Failed to start up manager.` and
`Freezing execution.` messages so subsequent runs report that failure without
waiting for the authentication deadline. This changes only host-side failure
detection; guest logging, sandboxing and the boot deadline remain unchanged.

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

### Integrated seeded cold boots, 2026-09-19

On Spark, clean revision `d40350c711b7551fcc7679c7d3c9f62b869c9798`
passed `make test-debian-nocloud-cold-boots SEL4_SDK_VERSION=2.3.0
QEMU_TEST_SSH_PORT=12270 QEMU_TEST_TIMEOUT=1800`. Both QEMU processes
exited. The [result](evidence/2026-09-19-spark/debian-seeded-cold-boots.json)
records two authenticated boots using the retained writable disk and original
Ed25519 SSH host identity. Both passed the architecture, disk-sync and
host-backed VirtIO net/block/console checks. Source media and agentOS image
identity were checked before the second boot; the image SHA-256 was
`89d0e2aec1e32a21091d0b993b9864daa0e9319f976bbe5aa40a635a68b63c0f`.

The [first timing](evidence/2026-09-19-spark/debian-seeded-first-timing.json)
was 735,401 ms and the
[second timing](evidence/2026-09-19-spark/debian-seeded-second-timing.json)
was 603,190 ms, from launch request to completed authenticated SSH. These are
individual observations, not a performance comparison. The second boot's
first SSH attempt timed out; the bounded retry succeeded with the original
host key. Journald also reported its journal as corrupted or uncleanly shut
down, renamed it, and continued. Preserve both observations when assessing
baseline reliability: this gate uses sync followed by QEMU stop, not orderly
guest shutdown, and does not prove journal integrity or guest-slot recreation.

Full local artifacts, including both disk copies, the target image, logs and
hash manifest, are retained under
`/home/jkh/.local/share/agentos-evidence/2026-09-19-debian-cold-boots/`.
This checkpoint does not promote Debian into every required gate or establish
x86 parity, concurrent storage isolation, or final release acceptance.

### Managed recreation and peer qualification, 2026-09-19

The following later checks use the isolated
`2.3.1-agentos-e60776ac-cr2` SDK. Each receipt identifies its exact source
revision; these results are not one final release qualification.

| Check | Revision | Retained result |
| --- | --- | --- |
| Spark managed Debian recreation | `6df47ad` | [Pinned host identity, persistent disk witness, fresh handle, stale-handle rejection and full gate](evidence/2026-09-19-spark/debian-managed-recreation.json) |
| Spark Debian with running FreeBSD peer | `cd9efb9` | [Peer SSH after Debian destruction, Debian recreation, concurrent SSH afterward and full gate](evidence/2026-09-19-spark/debian-peer-recreation.json) |
| Spark Ubuntu compatibility with running FreeBSD peer | `d3bd0ea` | [Deferred ISO initrd reconstruction, concurrent SSH after recreation and full gate](evidence/2026-09-19-spark/ubuntu-peer-recreation.json) |
| Intel two-vCPU Debian | `6df47ad` | [Authenticated boots across recreation and overlapping x87/SSE workers](evidence/2026-09-19-spark/intel-current-smp.json) |
| Intel 2 GiB Debian profile | `cd9efb9` | [Authenticated managed recreation and stale-handle rejection](evidence/2026-09-19-spark/intel-current-large-profile.json) |
| Intel storage and guest faults | `cd9efb9` | [Flushed storage across cold boots and VMX guest-fault recovery](evidence/2026-09-19-spark/intel-current-storage.json) |

The 2 GiB result qualifies the configured profile, not a measured Linux
`MemTotal` or memory stress workload. Peer recreation asserts authenticated
availability at explicit checkpoints; it does not measure uninterrupted peer
throughput or concurrent writable-storage isolation. Final promotion still
requires the full parity suite at one immutable revision, including the
current cold-boot comparison and architecture-specific storage acceptance.

At clean revision `2bb18a0`, the current Ubuntu live qualification and the
first seeded Debian cold boot both passed authenticated SSH and host-backed
VirtIO net/block/console assertions on Spark. Their
[comparison receipt](evidence/2026-09-19-spark/guest-current-boot-comparison.json)
passed the same-revision and QEMU-configuration checks: Ubuntu measured
1,061,081 ms and Debian 753,604 ms from launch request through authenticated
SSH. These include provisioning and host scheduling, impose no performance
threshold, and do not establish second-boot persistence or full baseline
promotion. The [second Debian cold boot](evidence/2026-09-19-spark/debian-current-cold-boots.json)
subsequently passed in 623,489 ms with the retained disk and original pinned
host identity. This is a sync-and-cold-boot result, not orderly shutdown or
managed guest-slot recreation.
