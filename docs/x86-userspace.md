# Minimal x86 Linux userspace qualification

`make x86-userspace-initramfs` cross-compiles a static x86_64 `/init` and
packs it into a deterministic newc archive at `build/x86-userspace/initrd.bin`.
The Rust packer reuses the archive and ELF-normalization code used by the
AArch64 guest probes. No host libraries or binaries enter the archive.

The init program checks that `getpid` returns 1, mounts devtmpfs, opens and
closes `/dev/hvc0`, and creates a regular file on
the guest initramfs, writes a fixed string, seeks, reads and compares every
byte, closes the file, and checks its PID again. All these operations use
Linux syscalls. Failure takes a distinct completion path.

The dedicated completion CPUID leaf is a qualification trap, compiled into
the VMM only with `X86_USERSPACE_PROOF=1`. The VMM checks the PID and marker
registers, guest CS selector and descriptor privilege level 3, active long
mode and paging, consumption of kernel/initrd input, a serial-service wake,
and the console driver's negotiated ready state. Root requires its
separate status, CPUID exit reason, ring-3 detail and user-range instruction
address. The harness requires the dedicated userspace marker and rejects
VMM failure/root-fault output. Ordinary firmware and HLT markers do not pass.

On an Intel nested-KVM host, use `make gate-x86_64-userspace` with
`SEL4_SDK_VERSION=2.3.0`, the pinned firmware and kernel inputs documented in
[boot payload delivery](x86-boot-payload.md), plus `X86_BOOT_INITRD` pointing
to the generated archive and its explicit `X86_BOOT_INITRD_SHA256` digest.
This runs Linux through the same OVMF reset and generated ACPI path.
The generated DSDT advertises the VMM console as `LNRO0005` with a 4 KiB
MMIO resource at `0xf0000000` and a level/high GSI 16 interrupt. The guest
kernel must include virtio-mmio, virtio-console and devtmpfs support; no
device command-line override or custom guest driver is used.

The initramfs file roundtrip is guest memory-backed filesystem I/O. It does
not qualify canonical console/network/block services, disk persistence,
multi-guest isolation, or desktop profiles. The completion trap is not a
production console or other service ABI. Target success must be established
by the gate; building the archive alone proves no guest execution.

At `accaafe`, the Intel target gate passed. A second run with deliberately
wrong PID assertions produced the explicit failure result from ring 3 and
did not pass the gate. Spark's full `make gate` also passed. The
[receipt](evidence/2026-09-17-spark/x86-userspace.json) records exact inputs,
artifacts and the negative-control construction. Required GitHub checks and
the other v0.4 acceptance items remain outstanding.
