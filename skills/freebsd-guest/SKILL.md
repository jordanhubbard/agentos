# freebsd-guest

FreeBSD is a virtio client, same ABI as Linux.

## Invariants

- Same emulated virtio devices as Linux; different kernel + FDT only.
- Do not give FreeBSD a private QEMU virtio-blk bus as architecture.
- Do not grow `kernel/freebsd-vmm/` — one VMM, two payloads (`PLAN.md` step 5).

## Helper

`make fetch-guest GUEST_OS=freebsd` and the virtio-device skill.
