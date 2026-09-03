# virtio device (guest-facing)

The VMM **emulates** virtio-mmio (or virtio-pci) for guests using libvmm
`virtio_mmio_*_init`. That is a different virtio world from QEMU's host
virtio-mmio, which is a physical device for a driver PD.

## Invariants

- Guest IPA for emulated net is `AOS_VIRTIO_NET_GUEST_IPA` (`0x0A010000`).
- That IPA must **fault** into the VMM. It must not be identity-mapped host MMIO.
- Backend is sDDF queues (`aos_net_virt_pump` until a nic_drv exists).
- QEMU buses at `0x0A000000` are a kill-dated crutch.

## Forbidden

- `virq_register` of a **host** INTID as the I/O path for a class that has
  an emulated virtio device.
- Guest DTB pointing only at QEMU virtio-mmio for that class.

## Helper

```sh
python3 skills/virtio-device/scripts/guest_dtb.py
```
