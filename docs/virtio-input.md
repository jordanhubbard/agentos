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

The `GUEST_INPUT` AArch64 image wires `input_virt` to root-provisioned pages:
each VMM receives its own keyboard/pointer event page, CC receives a separate
frontend page, and only the virtualizer maps all three. VMM wakeups use a
dedicated bit on their bound notification; CC and the service have separate
wait objects. All peer notification caps are send-only. The service owns no
device frame or IRQ.

Profiles selecting `input` initialize two faulting MMIO devices at guest IPAs
`0x0a050000` and `0x0a060000`, with virtual IRQs 55 and 56. Device initialization
fails closed if the backend is absent or fails. `debian-input.toml` selects
this path independently of GPU support. `make test-guest-input` requires both
Linux input device names as part of the live guest probe.

The live test additionally builds `tests/guest/input_probe.c` with
`make guest-input-probe` (override `GUEST_LINUX_CC` for the AArch64 Linux
cross compiler). After authenticated SSH provisioning, it uploads the static
helper to the disposable root-account guest. The helper discovers and grabs
both evdev devices before announcing readiness. The host then sends four
batches through the public `agentctl` command. The helper requires exact F13
press/release, relative X/Y/wheel motion, left-button press/release and
SYN_REPORT boundaries. Unexpected or trailing events fail the proof.
Successful runs retain an `.input.json` receipt beside the serial log; host
tests and compilation alone do not create a passing target receipt.

External clients can submit a packet with:

```text
CC_PD_SOCK=/path/to/cc.sock tools/agentctl/agentctl --batch input-batch 0 keyboard 1 183 1
CC_PD_SOCK=/path/to/cc.sock tools/agentctl/agentctl --batch input-batch 0 keyboard 1 183 0
```

Each command takes a public guest handle, `keyboard` or `pointer`, then one
or more numeric TYPE/CODE/VALUE triples. The CLI appends SYN_REPORT and
prints structured status and accepted-event count. Signed relative motion is
supported. A nonzero exit signals invalid arguments, rejected input or a
transport failure; the CLI never automatically retries stateful batches.

`MSG_CC_INPUT_SUBMIT` accepts a public guest handle and a complete event batch.
CC resolves the handle before passing a private client index to the service,
validates the returned count/status, and reports whole-batch backpressure.
The exact wire contract is in `contracts/cc_contract.h`. A failed transport
must not cause blind retries of stateful key/button transitions.

The target variant builds and the host tests pass. Target enumeration,
event delivery through Linux evdev, and mapping-isolation proofs remain
required; this implementation is not yet qualified as working guest input.
The common MMIO dispatcher has host regression coverage for all four byte
lanes of device configuration: input's selector and subselector are separate
byte fields. Reset also clears interrupt status before invoking backend reset,
so an old device interrupt cannot be reasserted after reset.
Queue attachment must not clear live state; lifecycle reclamation will require
coordinated quiescence before reset.

Refs: `task_cefc0f77327d4245ab9feb132cd1eb57`.
