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

`make test-guest-input QEMU_TEST_TIMEOUT=1800` runs the evdev checker three times:
first with explicit release events, then with zero-event server release
requests. Both passes require identical key, button, motion and SYN packets.
Separate `.input.json` and `.input-release.json` receipts distinguish the two
proofs. The third pass suspends the guest and fills both device paths until
the CLI returns validated WOULD_BLOCK responses, first for full batches and
then for a single press plus SYN. Duplicate presses occupy transport queues
but Linux filters them to one down event, avoiding an evdev-buffer overflow
when the guest resumes. Each device accepts repeated release requests while
full and rejects new input. After resume, the checker requires exactly one
down/SYN and one up/SYN for each device, with no extra events. The
`.input-backpressure.json` receipt records accepted batches and actual guest
events. A timeout or malformed reply cannot count as backpressure. This does
not simulate a lost connection; target results must be recorded separately.

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

The CC release API passed both live input passes at clean runtime revision
`1a4bb3821b31927b405c691d3ad42e5b54d9cd15` with SDK 2.3 on Spark.
The [explicit-events receipt](evidence/2026-09-17-spark/input-release-api-events.json)
and [server-held-state receipt](evidence/2026-09-17-spark/input-release-api-held.json)
each require four keyboard and seven pointer events, including exact releases
and packet boundaries. The second pass sends no explicit release events:
the service derives them from accepted held state. The
[timing receipt](evidence/2026-09-17-spark/input-release-api-timing.json)
records 445,468 ms through authenticated SSH.

At that revision, the complete SDK 2.3 `make gate` also passed. Its log SHA-256
is `f65124df316b59d37b3d46d6a6d0dfdf60ee56c43032a9e23337135b15a1d4e8`;
the two-pass live log SHA-256 is
`31eb16371bc5ea21d524e86754db42d7f15409af0e8f98d89975257d860ad964`.
The qualified image SHA-256 is
`17af4812a75a39a1fdd9a7b48ae732c85209d23a5c64987b5f6ed2926e225f53`.
Artifacts are retained under `build/evidence/input-release-api-1a4bb38/`.
Host tests qualify retained queue backpressure; this live proof qualifies
normal guest consumption of server-generated releases. Abrupt socket loss,
paused-guest backpressure on target, and reconnect cleanup remain unqualified.

MAC accepted the signed executor evidence, but automatic review
`review_0cc5a8f915024408aa9c9083deee3e61` could not run `make test-host`:
its verifier image lacks Cargo. The rejected image digest was
`sha256:1d4c5eb635c5fc53737b89949e22e04246870d57fa92f6b29547395716ea745d`.
The repository contract now explicitly requires Cargo, rustc and rustfmt.
The verifier-runtime repair is tracked by
`task_c66efe1c0eb44ee5926250ebcd38e5e3`; declaring prerequisites does not install
them or turn that rejected review into a pass. Policy and profile checks remain
part of the unchanged `make test-host` contract.

The first three-pass attempt at `55fd91e` passed the host suite and full OS
gate, but the explicit-events checker ended its output before its READY
marker. No input was injected. Debian had reported udev and network-service
startup failures before eventually reaching the shell and authenticated SSH.
The checker stderr was empty, and its SSH process exit status was not retained.
The [failed-attempt receipt](evidence/2026-09-19-spark/input-backpressure-first-run.json)
preserves the logs and limits. The harness now includes phase, SSH process
status and bounded stderr in such failures; no cause or target backpressure
pass is inferred from this attempt.

The diagnostic revision `bc785e9ffd9065c7d2d8ad5efd3ee89563f49b58` then
passed all three target input passes on Spark with SDK 2.3. The
[paused-guest receipt](evidence/2026-09-19-spark/input-backpressure-paused.json)
records four accepted full batches per device before saturation, validated
WOULD_BLOCK responses, retained repeated releases and rejection of new input.
After resume, Linux observed exactly four keyboard and four pointer events
(down/SYN, up/SYN). The
[explicit-events](evidence/2026-09-19-spark/input-backpressure-events.json) and
[server-release](evidence/2026-09-19-spark/input-backpressure-held.json)
passes also succeeded. All receipts identify a clean source tree.

[Boot through authenticated SSH](evidence/2026-09-19-spark/input-backpressure-timing.json)
took 461,181 ms. The image SHA-256 is
`fbea8d2404fb4eeb013527172d8f50ef6b952a9b42298f9397c637aa872b1885`;
the live log is
`355b0d18b431f48a49d2007c95ed804437679218609f726db1a36146fb1335d0`.
The retained local archive
`/home/jkh/.local/share/agentos-evidence/2026-09-19-input-backpressure/bc785e9-pass.tar.gz`
has SHA-256 `b81056be9586233aae64ee5d514c01570c23dfec6fbc7f262e4d587b2b49d5a8`.
The prior early exit did not recur, but no cause or fix is established for it.
Debian baseline task `task_26e8b1157ffe449483d2fe1c44f2a8be` retains that
reliability concern. Abrupt socket loss remains unqualified.
