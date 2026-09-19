# Varied-frame qualification on Spark

The retained AArch64 QEMU guest used agentOS packed-read implementation
`a9391b0aeb83e5475bbb140ae12be8de8ad8050d` and image SHA-256
`d34fadcf4da8cc681893409a6b9802c593ab35f4513203f958e1c26c30ca677e`.
Fixture source was `3403e59424480e0aaf2dd8c1cf9eac113f355e8e`;
the external GUI benchmark was `88553f3`.

Each successful pair read the same immutable 1024×768 XRGB8888 snapshot
through raw reads and production GUI batching. Both complete 3,145,728-byte
results matched the separately generated source reference, byte for byte.
Times below are milliseconds; call counts are client API calls, not wire
messages. No GUI was connected during these measurements.

| Pattern | Order | Sequence | Raw ms / calls | Packed batch ms / calls |
|---|---|---|---|---|
| tiles | raw first | 2337 | 3088.10 / 776 | 674.52 / 97 |
| tiles | packed first | 2337 | 3399.49 / 776 | 672.05 / 97 |
| gradient | raw first | 2852 | 2867.94 / 776 | 2029.31 / 102 |
| gradient | packed first | 2852 | 3713.74 / 776 | 3074.80 / 120 |
| noise | raw first | 2885 | 2739.44 / 776 | 3234.97 / 123 |
| noise | packed first | 2885 | 3892.20 / 776 | 4308.53 / 148 |

Packed batching improves tiles substantially and gradients modestly in these
samples, but regresses noise by approximately 11–18%. This is not a universal
performance improvement. Time-bounded batching yields different call counts
between orders. These static graphics-mode fixtures suppress console redraws;
their absolute times are not directly comparable to earlier live-console runs.

The initial gradient attempt began before the fixture's write-completion
marker was observed and failed the pixel comparison. Its empty JSON and error
log are retained. A later capture passed in both orders. This is consistent
with capturing before the pattern was visible, but does not independently
identify the failing pixels or prove a display-fence mechanism.

The native GUI rendered the tiled scene at sequence 2337 and reported 0.7 s.
The inspected screenshot is retained externally. The GUI process subsequently
exited; the original process handle was no longer available when rechecked,
so this receipt does not assert an independently recovered GUI exit code.
All fixture logs contain `FRAME_PATTERN_RESTORED`; gradient and noise SSH
sessions both exited zero after SIGTERM to their verified fixture processes.

Reference SHA-256 values:

* tiles: `58087b3fdd4c375b359aa8a18ee2c1040482763a4ff67592ffb2fbee468e6285`
* gradient: `f140a7270d32daf96f80864bf8ea1693560e87b87efdc7e58e9b1c08e1fb5dd4`
* noise: `b7ff18b33bda8310719e18451b78aef5610e1a4f7cb655576937bda700bfe2bf`

Artifacts and their SHA256SUMS manifest are retained at
`/home/jkh/.local/share/agentos-evidence/2026-09-19-frame-pattern/`.
Host fixture tests and GUI Rust tests passed in the retained build/test logs.
The prior packed-image gate remains separate evidence. This receipt adds no
claim about desktop applications, pointer delivery, input latency, cold-boot
stability, Intel VMX, or release readiness.
