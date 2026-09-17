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

CC `MSG_CC_INPUT_SUBMIT` accepts this version-2 request after resolving the
same public live guest handle used for ordinary input. The privileged CLI
exposes it as `agentctl input-release GUEST_HANDLE keyboard|pointer`. Its
zero accepted-event count acknowledges retained work, not completion in the
guest. Existing GUI version-1 requests remain unchanged.

`make test-guest-input QEMU_TEST_TIMEOUT=1800` runs the evdev checker twice:
first with explicit release events, then with zero-event server release
requests. Both passes require identical key, button, motion and SYN packets.
Separate `.input.json` and `.input-release.json` receipts distinguish the two
proofs. This does not simulate a lost connection.

`make test-input-host` checks exact release events, unchanged queues under
backpressure, keyboard/pointer isolation, repeats, and all 255 supported keys
across a full queue. `make gate` checks canonical guest I/O for regressions;
`make test-guest-input` separately checks live guest input delivery. Neither
is an abrupt-disconnect proof without the transport changes below.

Connection-loss detection remains to be connected to this operation. CC-PD
currently negotiates only VirtIO VERSION_1 and its QEMU device is a
`virtconsole`. QEMU's [console frontend](https://github.com/qemu/qemu/blob/v10.2.0/hw/char/virtio-console.c)
installs host connection callbacks for serial ports, but not consoles. The
[serial control protocol](https://github.com/qemu/qemu/blob/v10.2.0/hw/char/virtio-serial-bus.c)
provides PORT_OPEN notifications with MULTIPORT negotiation. Supporting those
control queues and a serial port, then qualifying abrupt GUI termination with
guest-observed releases, is required before claiming disconnect cleanup.
The GUI remains a direct binary CC-PD consumer; no HTTP bridge is involved.

## Spark qualification

At runtime revision `d10620b8eaa5ee13b5c4fe5c79f6e3cd4b100795`,
`make gate SEL4_SDK_VERSION=2.3.0 QEMU_TEST_SSH_PORT=12267` passed on
2026-09-17. This includes the host release tests, both root boot architectures,
and guest network, block and console I/O. The gate log SHA-256 is
`c9a640a355130b514d8369047de61a5cf485ad6c63fd7a2a8309c09fb9ac7ef7`.

SDK 2.1 timed out at the loader-to-kernel handoff after 300 seconds. The initial
SDK 2.3 build exposed this branch's obsolete read-only ISR VCPU access; the
existing compatibility fix removes that access. Neither failure is counted as
a passing qualification. Local logs are retained under
`build/evidence/input-release-d10620b/`.

The separate SDK 2.3 `make test-guest-input` run failed before event injection:
its CC console wait timed out after 300 seconds waiting for `# `. The transcript
reached `agentos-debian-init-validate-ok` and terminal initialization bytes.
This is not a passing input qualification and does not establish whether the
failure is caused by this change. Its log SHA-256 is
`ee4faba67890b4b4dd43cbfc6a9f81ba88425e0992df470101c7c4bb33fffbbb`.
That run did not complete live input regression; it provides no disconnect proof.

The longer baseline run at `676186a4a1d792d122c38bcedfc9166055532623`
passed ordinary input delivery with SDK 2.3 and `QEMU_TEST_TIMEOUT=1800`.
Its boot-through-SSH time was 598,820 ms, consistent with the earlier recorded
617,211 ms input qualification. This resolves the short-budget regression
failure; it does not qualify the later CC release API or abrupt disconnect.
The [input receipt](evidence/2026-09-17-spark/input-release-baseline.json)
and [boot timing](evidence/2026-09-17-spark/input-release-baseline-timing.json)
record the exact clean revision. The input Make targets now default to 1,800
seconds while preserving explicit environment and command-line overrides.
