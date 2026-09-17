# Reusing canonical virtio devices on x86

The device register implementation is shared in `libvmm/src/virtio/mmio.c`.
`virtio_mmio_reg_read` and `virtio_mmio_reg_write` operate on the existing
`virtio_device_t`, feature callbacks and GPA translation hook. They do not
inspect a seL4 CPU context or decode an architecture fault. Failed reads
leave the caller's output unchanged. The caller must validate the instruction,
MMIO region and access width and handle masking and register writeback.

The existing AArch64 adapter is in `libvmm/src/arch/aarch64/virtio_mmio.c`.
It retains fault registration, access masking, CPU register updates and virtual
IRQ registration. Console, network and block continue to use their existing
libvmm device implementations through this adapter. The utility header no
longer imports VCPU operations merely to name the PD in diagnostics.

`make test-virtio-mmio-core-host` links the actual shared register and GPA
implementation under ASan/UBSan. It checks feature negotiation, register
readback, callback failure, queue addresses above 4 GiB, GPA translation,
notification selection, interrupt acknowledgement, reset, and invalid queues
and GPAs. Host-only seL4 declarations make the CPU context opaque, so the
shared code cannot accidentally depend on AArch64 register fields.

This separation enables the x86 EPT adapter to call the same register code;
it does not yet connect an Intel guest to a service. The x86 qualification
composition still needs faulting guest MMIO regions, guest discovery, IOAPIC
delivery and the existing serial/net/block virtualizer queues and caps.
Physical hardware must remain owned by the driver PDs. The initramfs
userspace proof and its CPUID completion trap do not satisfy these device
requirements. Canonical device work is tracked by
`task_3a5da27d553a475092d35a9fa1cb90e9`.

The full Spark gate passed at `5d3a438`, including live guest console, network
and block through the shared implementation. The core also compiled against
the real x86 seL4 headers. The [receipt](evidence/2026-09-17-spark/virtio-mmio-core.json)
records the commands, artifact hashes and limits of that evidence.
