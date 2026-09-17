# QEMU display driver

`platform/display/ramfb.c` implements the bounded configuration protocol for
QEMU's RAM framebuffer device. It discovers `etc/ramfb`, requires fw_cfg DMA,
and emits the 28-byte big-endian configuration for XRGB8888. The protocol
checks allocation bounds, physical-address overflow, geometry, file selectors,
duplicate entries and transport errors before reporting success.

The hardware specifications are QEMU's [fw_cfg interface](https://www.qemu.org/docs/master/specs/fw_cfg.html)
and [ramfb implementation](https://github.com/qemu/qemu/blob/master/hw/display/ramfb.c).
The transport callbacks take host-endian selectors and byte-preserving payloads;
the MMIO adapter performs register endian conversion. DMA payloads
and descriptors must live in private driver memory through completion, including
timeout recovery. A timeout cannot release storage still accessible to DMA.

`ramfb_mmio.c` implements the standard fw_cfg register layout and DMA
descriptor in a private, uncached 64-byte region. It copies caller bytes before
triggering DMA, writes the high address half before the triggering low half,
and uses device barriers around submission and completion. Polling has a fixed
upper bound. Device errors and timeouts permanently disable that context;
later calls cannot overwrite its DMA storage or change the active selector.
The root/driver integration must retain that allocation until device shutdown.

The framebuffer-to-driver contract is `platform/include/platform/display.h`.
A one-entry SPSC request/reply queue transfers at most 64 KiB per request;
the producer retains payload ownership until receiving its reply. BEGIN assigns
a fresh cookie, WRITE requires sequential complete coverage, PRESENT switches
the driver's private banks only after successful hardware configuration, and
ABORT discards an unfinished transfer. A failed hardware presentation disables
further transactions and retains both banks. Root must map the queue only into
the framebuffer service and display driver; banks remain private to the driver.
Both sides must signal persistent notifications after publishing work or freeing
queue space. `make test-display-host` verifies a complete 1024x768 frame,
unchanged front pixels during transfer, backpressure, wrap, stale cookies,
abort and failed presentation. This contract is not yet mapped on target.

This is a protocol component, not a working scanout path. No PD has been given
fw_cfg or display DMA capabilities by this change. Target integration requires
a dedicated TCB driver, root-provisioned contiguous scanout/DMA storage, a
framebuffer-to-driver queue, bounded completion handling and actual QEMU display
capture with exact pixel verification. The guest continues using emulated
virtio-gpu and never receives the fw_cfg device or display DMA memory.

`make test-ramfb-host` verifies the wire configuration, discovery bounds,
MMIO endian layout, DMA descriptor, completion, timeout and retained-storage
behavior. It is included in `make test-host`. Passing it does not prove
display ownership, scanout, bare-metal GPU support or v0.4 completion.
