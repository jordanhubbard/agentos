# Remote input release

The input virtualizer records held state only after accepting a complete
keyboard or pointer batch. Key repeats do not create new held state. This
private state is separate for each client and device; motion and wheel events
are not held state.

A request with version `AOS_INPUT_RELEASE_VERSION` (2), count zero and zero
reserved fields requests release of all held keys or buttons for its selected
client/device. Its version-2 response acknowledges acceptance, with accepted
event count zero. Normal version-1 event batches retain their existing ABI.

Release acceptance is asynchronous. The service emits batches of at most 63
release events followed by SYN_REPORT. It never overwrites queued events or
resets queue cursors. If the guest is paused or its event queue is full, the
service retains pending releases and retries after VMM consumption wakes it.
New input for that device returns WOULD_BLOCK until all releases are queued;
other clients and devices remain independent. Repeated release requests are
idempotent. Guest consumption, rather than acceptance of the request, determines
when applications observe the release.

`make test-input-host` checks exact release events, unchanged queues under
backpressure, keyboard/pointer isolation, repeats, and all 255 supported keys
across a full queue. `make gate` checks the existing live input consumers and
canonical guest I/O paths for regressions; it is not a disconnect proof.

Connection-loss detection remains to be connected to this operation. CC-PD
currently negotiates only VirtIO VERSION_1 and its QEMU device is a
`virtconsole`. QEMU's [console frontend](https://github.com/qemu/qemu/blob/v10.2.0/hw/char/virtio-console.c)
installs host connection callbacks for serial ports, but not consoles. The
[serial control protocol](https://github.com/qemu/qemu/blob/v10.2.0/hw/char/virtio-serial-bus.c)
provides PORT_OPEN notifications with MULTIPORT negotiation. Supporting those
control queues and a serial port, then qualifying abrupt GUI termination with
guest-observed releases, is required before claiming disconnect cleanup.
The GUI remains a direct binary CC-PD consumer; no HTTP bridge is involved.
