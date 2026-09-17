# Guest keyboard and pointer queues

The v0.4 input path will route external input through an input virtualizer
and per-VMM pages into libvmm-emulated virtio-input devices. No guest receives
a physical input device mapping. Keyboard and relative-pointer streams remain
separate so guest capability discovery can identify both devices.

`platform/include/platform/input.h` defines the bounded queue contract.
The frontend submits up to 64 events ending in SYN_REPORT. The virtualizer
validates the complete batch against the selected device's supported event
types, codes and values, and checks a private client-authorization mask.
It publishes the entire batch or accepts none. Response backpressure defers
execution; event backpressure returns WOULD_BLOCK so the producer can retry
without dropping part of a key/button transition. Requests, responses and
event rings have fixed private capacity and wrapping sequence counters.

Keyboard events cover key codes 1 through 255 with release, press and repeat.
The relative pointer supports X/Y, horizontal/vertical wheel movement and
eight mouse buttons. The virtio-input capability tables advertise
exactly these accepted event classes. Unadvertised LED/absolute/touch events
are not part of this contract.

`make test-input-host` checks exact events, independent guest/device streams,
whole-batch rejection, private authorization, full queues, response ownership,
counter wrap and invalid occupancy. It also links the actual libvmm input
backend, MMIO dispatcher and GPA copier to the canonical input service. It
queries the device's byte selectors, checks advertised event bits, and asserts
exact little-endian events across direct scatter buffers. Guest buffer
exhaustion retains pending input, including the terminating SYN_REPORT;
another client's stream remains untouched. Status buffers are returned without
unadvertised LED side effects. Invalid queue state consumes no source events
and requests a device reset. Private indices handle 16-bit ring wrap.

`libvmm/src/virtio/input.c` implements device ID 18 with event/status queues
and VERSION_1 as its only feature. It bounds each drain to 128 available
entries and snapshots direct descriptor chains before accessing payloads.
Reset clears device mappings and indices while retaining a source event if
its output copy failed; it does not clear the live virtualizer queue. The
source callback and drains run serially in the VMM. Device behavior follows
[VirtIO 1.2 section 5.8](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html).

The service and backend are currently host-tested only. Root mappings,
persistent notifications, the target input PD, CC routing, and guest
enumeration/delivery proof remain required.
The common MMIO dispatcher has host regression coverage for all four byte
lanes of device configuration: input's selector and subselector are separate
byte fields. Reset also clears interrupt status before invoking backend reset,
so an old device interrupt cannot be reasserted after reset.
Queue attachment must not clear live state; lifecycle reclamation will require
coordinated quiescence before reset.

Refs: `task_cefc0f77327d4245ab9feb132cd1eb57`.
