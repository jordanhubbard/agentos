# Aligned-copy experiment: no demonstrated frame latency benefit

On Spark, candidate `70e0561d28d83178bca7ec2bfa9e70e002365a28` replaced
the shared freestanding memcpy byte loop with aligned scalar 64-bit copies.
Control runtime: `3999e6d1556bb900869fd8018926dc36973ae34e`.
The native 3 MiB copy microbenchmark improved from approximately 0.79–0.81 ms
to 0.12–0.14 ms. That did not translate into a demonstrated target benefit.

Both qualified Debian graphics guests remained running during sequential
candidate/control/candidate measurements. GUI client source was
`bdc2520c8a422aba8253b8cc38691d625c13f843`.

| Measurement | Candidate 1 | Control | Candidate 2 |
| --- | ---: | ---: | ---: |
| 128 immutable 4056-byte reads, ms | 1451.695 | 1346.495 | 1240.956 |
| Maximum individual read, ms | 189.424 | 182.395 | 191.383 |
| Full agentctl frame capture, seconds | 7.65 | 7.80 | 8.79 |

The region tests used `make benchmark-frame` with `BENCH_READS=128` and
`BENCH_GUEST_HANDLE=0`; all reported stable pixels. Full captures used
`agentctl frame-capture 0` timed with `/usr/bin/time`; all exited zero.
They transferred 3145728 bytes for a 1024×768 frame. These timings include
client lifecycle and file output, not native GUI rendering or input latency.
Host background load was uncontrolled, and the samples do not establish
statistical equivalence or exclude a smaller improvement.

The corrected candidate passed GCC, strict-alignment Clang, and ASan/UBSan
copy tests, including exact range boundaries and guard pages. Its full
`make gate SEL4_SDK_VERSION=2.3.0 QEMU_TEST_SSH_PORT=12275` passed.
`make demo-guest-display SEL4_SDK_VERSION=2.3.0 QEMU_TEST_SSH_PORT=12275
QEMU_TEST_TIMEOUT=1800` matched all 786432 QEMU scanout pixels and completed
guest provisioning. The harness reported its frame capture in 2 seconds;
that is a different client and scope from the agentctl timings above.

- Candidate image SHA-256: `59e3d4c332d54e071238b042dbac6c53c7364d40dde0a8ff562d7e19844f361f`
- Gate log SHA-256: `dc02ccf9720b0a3f40b092528058eba31a294eb788be989583b6b577d9eadc38`
- Qualified frame SHA-256: `cb9b1f94595564c62a17a1e987f5e8b3877467efcf4510191e78261c25de9643`
- Retained artifacts: `/home/jkh/.local/share/agentos-evidence/2026-09-19-memory-copy/`, with verified `SHA256SUMS`.

The shared-copy implementation and its dedicated test are withdrawn from
the proposed release: the measured remote-frame path does not justify the
added common runtime change. The experimental revisions remain in history
for reproducibility. This does not withdraw the separately measured observer
progress and CC refill cadence improvements. Interactive frame delivery,
native GUI/input acceptance, required reviews, and integration remain open.

Refs: `task_5dd48e2ddd46473ab45df5b8ae8ee6c1`.
