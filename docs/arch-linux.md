# Arch Linux on x86

The Arch profiles pin the official 2026.09.01 ISO, its kernel, and a reduced
initramfs. Package installation uses the matching Arch Linux Archive snapshot.
This is an Arch guest using agentOS's emulated virtio devices.

## Installation qualification

On an Intel Linux host with nested VMX/KVM and the repository's pinned SDK:

```sh
make gate-x86_64-arch GUEST_OS=none QEMU_TEST_TIMEOUT=14400 \
  X86_FIRMWARE_IMAGE=/path/to/OVMF.fd \
  X86_FIRMWARE_SHA256=101afad5f520753224b60b9555df60add435cf50c7a82b781d7d7d8c60db7eb5
```

Firmware provenance is described in [x86 firmware bring-up](x86-firmware.md).
`ARCH_AMD64_ISO` can point to an existing ISO; its pinned digest must still
match. `X86_SSH_PORT` overrides the default host port 12225.

The gate creates a new 8 GiB disk beneath `_build/evidence/x86-arch-install-*`.
It copies the verified ISO into that disposable disk. The stock live boot
copies the squashfs into guest RAM and unmounts the installation medium before
the console recipe repartitions it. No existing disk is accepted as an output.
The HTTP live-boot path is unsuitable for the 2 GiB guest because its stock
hook retains both the downloaded image and a second RAM copy.

The trimmed initramfs retains ISO9660 and its CD-ROM dependency alongside the
network, loop, squashfs, and overlay modules. The ISO's digest covers the live
filesystem; this local-media path does not require CMS verification against
the guest's deterministic RTC epoch.

The recipe installs Arch, creates the `agentos` account, provisions a generated
SSH public key, disables password authentication, and records installed
packages. It then starts a fresh VM using the same disk and requires a pinned
SSH host key, a terminal, identity checks, file/process operations, networking,
and package install/execute/remove checks. The reboot uses the profile's pinned
kernel and initramfs with the persistent root partition; it does not qualify
booting the installed bootloader or kernel updates.

The evidence directory contains the disk, SSH identity, phase logs, and
`receipt.json`. A passing receipt requires both installation and reboot.
`make clean` removes these generated files along with the rest of `_build`.

## Desktop qualification

Use the retained disk and identity from a successful installation:

```sh
make gate-x86_64-desktop GUEST_OS=none QEMU_TEST_TIMEOUT=14400 \
  X86_ROOT_DISK=/path/to/arch-root.raw X86_SSH_KEY=/path/to/id_ed25519 \
  X86_FIRMWARE_IMAGE=/path/to/OVMF.fd \
  X86_FIRMWARE_SHA256=101afad5f520753224b60b9555df60add435cf50c7a82b781d7d7d8c60db7eb5
```

This profile adds emulated virtio GPU and input, installs Sway and WayVNC,
and checks display/input through the existing desktop qualification harness.
The provisioning recipe sets the guest clock from the host timestamp before
HTTPS downloads; the private RTC does not provide persistent wall-clock time.
WayVNC listens on guest loopback and is reached through authenticated SSH.
Host tests and profile validation alone do not establish a passing native
desktop run.
