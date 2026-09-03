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

```sh
python3 skills/sel4-platform/scripts/tcb_view.py
```

Prints HTML of the TCB vs the live `system_desc_aarch64.c` PD list.
