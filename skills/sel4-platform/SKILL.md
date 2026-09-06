# seL4 platform

You are composing agentOS **on-target topology**, not a Linux distro.

## Invariants

- seL4 is the only kernel-mode code.
- Root task distributes caps, then idles. No policy there.
- PDs in `docs/TCB.md` may own devices. Museum PDs must not be extended.
- QEMU is hardware. Do not passthrough QEMU virtio-mmio into guests.

## Forbidden

- Adding `oom_killer`, POSIX spawn/vfs, or vibe-swap-as-NIC.
- Documenting "rings 1–5".
- Python inside a PD.

## Helper

Run `make gate` to validate the generated topology and boot it on both
supported QEMU architectures. The topology authority remains generated C and
the documented TCB, with no rendered UI artifact.
