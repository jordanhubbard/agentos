# Input timing measurement-channel control

Guest helper `7ec8cb8` adds `--ssh-control`, which validates twenty ordered
ping records and echoes receipts without evdev access or injected input.
The external GUI harness `8e2652c` can run this control and optionally record
backend scheduling/socket wait separately from input request duration.

The native live-display probe passed twenty evdev transitions and final
assertions. Its receipt upper-bound median was 532.353 ms. Backend request
median was 2.160 ms (maximum 4.030 ms); shared-client wait median was 4.981 ms
(maximum 155.301 ms). These are not guest-delivery timestamps.

SSH-only controls passed twenty receipts each: refresh stopped median
345.498 ms, maximum 521.066 ms; live refresh median 579.259 ms, maximum
2102.707 ms. The live control used here ran after browser tests finished.
An earlier control overlapping those tests is retained separately.
This establishes substantial measurement-channel overhead. It does not
isolate its one-way contribution or permit subtraction to estimate true
input latency. Input-to-render acceptance remains outstanding.

Core host tests and guest helper build passed. The GUI's 15 Rust tests,
130 browser tests, typecheck, Cargo/example checks and native build passed.
Native shutdown returned zero. No production OS runtime changed.

Complete report and manifest:
`/home/jkh/.local/share/agentos-evidence/2026-09-19-input-stage-timing/`.
The SSH-control helper SHA256 is
`48749fa16ae56b5809f0d3f9686e7a43089a3f18250541538c915730f5ba4e13`.
