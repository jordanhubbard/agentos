# Debian test root provisioning

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
Full Spark `make gate` passed at `bff2e2c`. These results do not qualify
concurrent guest isolation, guest-slot resource reclamation, interactive GUI
performance or the complete release milestone.
