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
eight mouse buttons. The future virtio-input capability tables must advertise
exactly these accepted event classes. Unadvertised LED/absolute/touch events
are not part of this contract.

`make test-input-host` checks exact events, independent guest/device streams,
whole-batch rejection, private authorization, full queues, response ownership,
counter wrap and invalid occupancy. The service core is currently host-tested
only. Root mappings, persistent notifications, the target input PD, CC routing,
the virtio-input backend and guest enumeration/delivery proof remain required.
The common MMIO dispatcher has host regression coverage for all four byte
lanes of device configuration: input's selector and subselector are separate
byte fields. Reset also clears interrupt status before invoking backend reset,
so an old device interrupt cannot be reasserted after reset.
Queue attachment must not clear live state; lifecycle reclamation will require
coordinated quiescence before reset.

Refs: `task_cefc0f77327d4245ab9feb132cd1eb57`.
