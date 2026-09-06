# agentOS Dual-Guest Demo

This is the shortest path to the current agentOS showcase: one seL4 system
boots Ubuntu and FreeBSD concurrently, routes their storage, network, and
console traffic through agentOS-owned services, proves key-only SSH access to
both guests, and leaves both systems running for inspection.

## First run

The supported demo hosts are macOS and Linux. Allow at least 8 GB of RAM,
20 GB of free disk space, and network access for the initial SDK and guest
image downloads.

```bash
make setup
make demo
```

`make setup` installs host packages, downloads the shared Microkit 2.1.0 SDK
to `$HOME/.cache/agentos`, and validates the demo toolchain. It is idempotent.
Set `SEL4_SDK=/absolute/path/to/microkit-sdk-2.1.0` before invoking it to use
an existing SDK.

`make demo` then:

1. stages the Ubuntu 26.04 and FreeBSD 15.0 AArch64 media under
   `build/guest-images`;
2. builds one AArch64 agentOS image containing both VMM protection domains;
3. starts QEMU with 3 GB assigned to the outer agentOS system;
4. boots both guests and provisions a temporary Ed25519 public key;
5. requires successful key-only SSH commands against both guests; and
6. prints two SSH commands and keeps QEMU running.

Initial image downloads and software-emulated AArch64 boot can take a long
time. Apple Silicon uses QEMU TCG because the HVF backend is not compatible
with the current seL4 AArch64 path. The default demo deadline is 90 minutes;
override it only when diagnosing a known slow host:

```bash
make demo DUAL_OS_TEST_TIMEOUT=7200
```

## Open both guests

After the acceptance gate succeeds, it prints commands equivalent to:

```bash
ssh -i build/tmp/dual-ssh/id_ed25519 -p 12222 \
  -o IdentitiesOnly=yes -o StrictHostKeyChecking=no \
  -o UserKnownHostsFile=/dev/null ubuntu@127.0.0.1

ssh -i build/tmp/dual-ssh/id_ed25519 -p 12223 \
  -o IdentitiesOnly=yes -o StrictHostKeyChecking=no \
  -o UserKnownHostsFile=/dev/null root@127.0.0.1
```

Run each command in a separate terminal. Keep the original `make demo`
terminal open; press Enter there to stop QEMU after the demonstration.

The SSH key is generated specifically for the run. Password and
keyboard-interactive authentication are disabled by the acceptance probes.
Ports 12222 and 12223 must be free before the demo starts.

## Non-interactive and fast checks

```bash
make demo-test   # same dual-guest authenticated-SSH gate; exit afterward
make demo-smoke  # host-only contract and logic checks; no QEMU
make demo-check  # prerequisites only
```

`make demo-test` is the automated acceptance target. It destroys both guest
instances after the SSH proof and exits with a non-zero status on failure.

`make demo-smoke` is useful before a presentation, but it is not evidence that
agentOS or either guest booted. Only `make demo` or `make demo-test` exercises
the complete seL4, VMM, device-service, guest, and SSH path.

## What the demo proves

A successful run demonstrates the following behavior in one live system:

- seL4 remains the only privileged kernel;
- Ubuntu and FreeBSD execute in distinct guest VSpaces at the same time;
- each guest sees a conventional GPA window while its VMM uses a separate HVA
  mapping;
- host devices are owned by agentOS driver/service protection domains;
- guests consume agentOS-emulated virtio network, block, and console devices;
- CC-PD can observe and control both guest consoles; and
- both guests accept authenticated, key-only SSH sessions over distinct
  forwarded addresses.

It does not prove unfinished operations such as guest snapshot, restore, or
live migration.

## Troubleshooting

- Run `make demo-check` for a concise missing-tool or SDK diagnosis.
- Run `make setup` again after changing hosts or toolchains.
- QEMU and harness logs are retained under `build/tmp/`.
- Guest images are cached under `build/guest-images`; `make clean` keeps them.
- `make clean-images` deliberately removes the staged guest media and forces a
  later download.
- Use `make help` for the lower-level build, device-proof, and architecture
  gates.
