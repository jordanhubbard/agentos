# QEMU display driver

`platform/display/ramfb.c` implements the bounded configuration protocol for
QEMU's RAM framebuffer device. It discovers `etc/ramfb`, requires fw_cfg DMA,
and emits the 28-byte big-endian configuration for XRGB8888. The protocol
checks allocation bounds, physical-address overflow, geometry, file selectors,
duplicate entries and transport errors before reporting success.

The hardware specifications are QEMU's [fw_cfg interface](https://www.qemu.org/docs/master/specs/fw_cfg.html)
and [ramfb implementation](https://github.com/qemu/qemu/blob/master/hw/display/ramfb.c).
The transport callbacks take host-endian selectors and byte-preserving payloads;
the future MMIO adapter must perform register endian conversion. DMA payloads
and descriptors must live in private driver memory through completion, including
timeout recovery. A timeout cannot release storage still accessible to DMA.

This is a protocol component, not a working scanout path. No PD has been given
fw_cfg or display DMA capabilities by this change. Target integration requires
a dedicated TCB driver, root-provisioned contiguous scanout/DMA storage, a
framebuffer-to-driver queue, bounded completion handling and actual QEMU display
capture with exact pixel verification. The guest continues using emulated
virtio-gpu and never receives the fw_cfg device or display DMA memory.

`make test-ramfb-host` verifies the wire configuration, discovery bounds and
failure results. It is included in `make test-host`. Passing it does not prove
display ownership, scanout, bare-metal GPU support or v0.4 completion.
