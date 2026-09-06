# linux-guest

Linux is a virtio client. The payload is a kernel + initrd + FDT.

## Invariants

- In-tree virtio-mmio drivers only.
- FDT must describe **emulated** virtio devices (agentOS IPA), not only QEMU.
- No `gpu_shmem` kernel module as the first GPU story.
- Guest flavor is data for the VMM, not a fork of VMM C.

## Helper

Use the virtio-device skill and `make test-host` for the device-list checks.
`make fetch-guest GUEST_OS=ubuntu` stages images via the Makefile.
