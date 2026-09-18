# Guest network quiescence

`aos_vmm_virtio_net_quiesce()` retires the libvmm guest-ring references and
stops subsequent adapter wakeups from processing guest buffers. The caller
must first stop all guest vCPUs and return from the active device callback.
The operation is idempotent; calling it before device initialization is a
no-op. Guest reset and feature renegotiation do not reopen a quiesced device.

TX copies finish before the queue-notify callback returns. Subsequent service
completion uses canonical packet storage, not guest RAM. RX retains no guest
pointer between callbacks. Quiescence leaves service queues and their storage
allocated: detach, shared-storage reclamation and rebind require a separate
service lifecycle. The adapter continues to reject a second initialization.

`make test-x86-net-host` first asserts exact live TX/RX bytes through the real
libvmm backend, canonical pump and x86 MMIO/IOAPIC adapter. It then leaves RX
and copied TX pending, quiesces the device, marks every guest RAM page
`PROT_NONE`, completes TX through the pump, and delivers late wakeups and queue
notifications. RX stays queued, no interrupt or kick is emitted, and valid
guest reset/feature/queue negotiation cannot resume access. Both loopback and
host-NIC attachment fixtures run under address and undefined-behavior sanitizers.

Host tests and production backend compilation for AArch64 and x86_64 passed.
This is a prerequisite for connecting production teardown; it is not yet a
target-tested destroy/recreate path or complete capability reclamation.
