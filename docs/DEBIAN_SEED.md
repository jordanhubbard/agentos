# Debian test root provisioning

The native udev wrappers share `guest-profiles/helpers/debian_init_hook.h`.
It implements Linux syscall and stat layouts for x86_64 and AArch64, preserving
stock udev execution, filling missing standard descriptors before startup and
checking console nodes after shutdown. `make debian-aarch64-console-hook` builds
the ARM wrappers. Their native Spark chroot checks cover closed standard
descriptors, successful stock-hook execution, correct node creation/reuse,
wrong-node rejection and unexpected `rootmnt` rejection. The x86 helper and
initrd pins remain unchanged. The opt-in ARM NoCloud profile uses these
wrappers; on-target qualification remains pending.

Make captures `AGENTOS_HOST_TOOL_PATH` before entering the kernel sub-make.
Native guest-helper compilation and stripping use that path even when the
kernel build selects another LLVM installation. This prevents nested builds
from silently changing a pinned guest artifact's compiler. Direct xtask calls
use their current PATH unless this variable is supplied explicitly. Artifact
hash verification remains mandatory.

Prepare a disposable copy of the acquired Debian ext4 root with stock cloud-init
NoCloud data:

```sh
make seed-guest-root \
  SEED_ROOT_EXT4=build/guest-images/debian-amd64/root.ext4 \
  SEED_PUBLIC_KEY=/path/to/test-identity.pub \
  SEED_OUTPUT=/path/to/new-root.ext4 \
  SEED_INSTANCE_ID=agentos-test-unique-id
```

This Rust command requires `debugfs` and `ssh-keygen`. It accepts an Ed25519
public key, retains the stock default account, disables password authentication,
and requests guest-generated Ed25519 host keys. It does not read the private
login key. Use a fresh source image and unique instance ID for each new guest.

The network configuration matches the qualified Intel test composition:
`eth0`, `10.0.2.15/24`, gateway `10.0.2.2`, DNS `10.0.2.3`. This is a single-guest
test-network configuration, not a deployment or multi-guest network policy.

The source stays unchanged. A private temporary directory holds the copy;
all three seed files must read back exactly through debugfs before the output
is published without replacing an existing destination. An invalid filesystem
or a conflicting existing seed fails the readback check.

The opt-in AArch64 profile is `debian-arm64-nocloud.toml` (alias
`debian-arm64-nocloud`). Acquire it with
`make fetch-guest GUEST_PROFILE=debian-arm64-nocloud.toml`. It uses the same
dated Debian release, native hooks and NoCloud root seed as amd64, with stock
cloud-init enabled and the default Debian account. Its canonical acquisition
directory is `build/guest-images/debian-arm64-nocloud`; use `root.ext4` and
`disk.raw` there as seed inputs and `seeded.raw` as output. The root partition
offset is 134217728 bytes. Its initial console boot check is
`make test BOARD=qemu_virt_aarch64 GUEST_OS=debian-arm64-nocloud QEMU_TEST_TIMEOUT=1200`.
Fresh-disk boot and key-only authentication passed on Spark. This profile does
not replace the existing `debian` baseline until full parity is demonstrated.

The preseeded ARM authentication gate is
`make test-debian-nocloud-ssh SEEDED_SSH_KEY=/path/to/identity SEEDED_DIRECTORY=build/evidence/arm-seeded-boot QEMU_TEST_SSH_PORT=12222 QEMU_TEST_TIMEOUT=1200`.
It drains and retains the console through CC-PD, requires the profile's login
markers and host-key report, then uses the same strict bounded SSH probe as
Intel with an exact `aarch64` result. The gate creates a private writable disk
copy in `SEEDED_DIRECTORY`, retaining guest writes even after failure. If the
directory is omitted, it creates and prints a unique evidence directory.
For a later boot of the same disk, pass that same `SEEDED_DIRECTORY` and
`SEEDED_SSH_KNOWN_HOSTS` pointing to the first successful run's receipt. The
source image must remain unchanged; a first boot refuses to overwrite an
existing copy. No QEMU process may still be using the retained disk.
The ARM profile gives device discovery a bounded 600-second wait under TCG.
With the stock deadline, systemd entered emergency mode before udev's startup
prerequisites completed. The longer wait preserved the stock EFI mount: the
partition was discovered, checked and mounted, then Debian reached login and
passed pinned-key SSH with exact `aarch64` output and a successful disk sync.
The first SSH attempt timed out; the second succeeded. Failed attempts and
the successful boot are recorded in
`docs/evidence/2026-09-17-spark/arm-nocloud-boot-attempts.json`.
Same-disk cold-boot authentication also passed using the original host-key
receipt: the third SSH attempt succeeded after a banner timeout and a server
response timeout. Both receipts were byte-identical. Journald replaced its
unclean prior journal after the QEMU stop; this test syncs the disk but does
not shut the guest down cleanly.

The gate also records `*.boot-timing.json` from the host QEMU launch request
through completed authenticated SSH. Acquisition, build and managed disk
preparation are excluded. A receipt is marked passed only after the canonical
host-backed net, block and bidirectional console checks succeed. Cold-boot
receipts are marked separately. The timing comparison accepts both Debian
profiles, requires matching revisions and QEMU configurations, and rejects
dirty-tree receipts. Matched measurements at clean revision `22a22b3` passed:
Debian took 787.025 seconds and Ubuntu took 1478.667 seconds. Both passed the
canonical I/O assertions; these are individual runs, not a performance
threshold qualification. Receipts are retained in
`docs/evidence/2026-09-17-spark/nocloud-boot-timing-comparison.json`; older
receipts remain historical evidence. Ubuntu's boot warnings and failed
Debian attempts remain recorded alongside the successful measurements.

By default the output is an ext4 partition image. To produce a full raw disk,
also supply `SEED_DISK_RAW` and `SEED_PARTITION_OFFSET`. For the pinned
`debian-amd64.toml` image, use
`SEED_DISK_RAW=build/guest-images/debian-amd64/disk.raw` and
`SEED_PARTITION_OFFSET=134217728`. Set `SEED_OUTPUT` to a fresh raw disk path.

Assembly copies the disk, checks alignment and bounds, and compares every byte
of the original root image against that disk region before replacement. It
requires the seeded root to retain exactly the original size and reads back
the complete replacement before publication. The offset is explicit; this
command does not discover or validate GPT metadata.

The separate Intel SSH gate consumes the prepared full disk:

```sh
make gate-x86_64-debian-ssh \
  SEL4_SDK_VERSION=2.3.0 \
  X86_ROOT_DISK=/path/to/new-disk.raw \
  X86_SSH_KEY=/path/to/test-identity \
  X86_SSH_PORT=12224 QEMU_TEST_TIMEOUT=600 \
  X86_FIRMWARE_IMAGE=/path/to/pinned-ovmf.fd \
  X86_FIRMWARE_SHA256=<expected-sha256>
```

This requires an accessible Intel Linux KVM host. The private key must match
the public key used to seed this disk. The gate forwards only a loopback port
through the restricted test network. It waits for a login prompt and complete
cloud-init host-key report, pins that Ed25519 key, then requires key-only SSH
to return exactly `x86_64` and successfully sync the guest disk. It disables
the SSH agent and system/user client configuration. Each attempt has a maximum
90-second deadline within the overall gate timeout; stdout, stderr and pinned
known-host data are retained beside the console log.

For a second cold boot of the same disk, also pass
`X86_SSH_KNOWN_HOSTS=/path/to/first-run.known_hosts`, using the first successful
gate's receipt and the same port and login key. Do not reseed the disk. The
gate accepts exactly one Ed25519 key bound to that loopback endpoint and uses
strict host verification. This preserves the first boot's identity when
cloud-init no longer emits its first-instance host-key report. Cold-boot
qualification of this option passed on Intel in attempt 25 at `3007751`.

The integrated SSH path passed on Intel at `3007751`: fresh native-generated
media authenticated against the console-pinned host key, and the same disk
passed after a new QEMU cold boot using the retained host key. Both runs returned
exactly `x86_64`, synced the guest disk and exited successfully. Evidence is in
`evidence/2026-09-17-spark/x86-ssh-gate-attempts.json`. The
300-second native-media login experiment timed out during cloud-init startup;
its receipt remains in `evidence/2026-09-17-spark/x86-native-seed-boot.json`.
The earlier manual Intel boot evidence is retained in
`evidence/2026-09-17-spark/x86-debian-ssh-seed.json`.

Validation on Spark: the pinned Debian root was seeded successfully; malformed
ext4 input produced no output; an existing output was rejected. `make test-host`
and all 97 Rust library tests passed after the interleaved login-prompt fix. The disk
assembly test checks exact replacement, preserved surrounding bytes, unchanged
source, wrong source region, unaligned offsets and out-of-bounds offsets.
Full Spark `make gate` passed at `bff2e2c` and again at `4456848` after the
NoCloud timing harness changes; the latter receipt is retained in
`evidence/2026-09-17-spark/nocloud-timing-full-gate.json`. These results do not qualify
concurrent guest isolation, guest-slot resource reclamation, interactive GUI
performance or the complete release milestone.
