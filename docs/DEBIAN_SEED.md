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
command does not discover or validate GPT metadata. QEMU launch and automated
SSH qualification are not yet wired to this command.
The earlier manual Intel boot evidence is retained in
`evidence/2026-09-17-spark/x86-debian-ssh-seed.json`.

Validation on Spark: the pinned Debian root was seeded successfully; malformed
ext4 input produced no output; an existing output was rejected. `make test-host`
and all 94 Rust library tests passed after disk assembly was added. The disk
assembly test checks exact replacement, preserved surrounding bytes, unchanged
source, wrong source region, unaligned offsets and out-of-bounds offsets.
This host-tool validation does not establish
that the newly generated media has booted.
