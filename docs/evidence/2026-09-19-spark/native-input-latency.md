# Native keyboard delivery upper bounds on Spark

The guest probe at `12069b7` adds `--gui-latency`: twenty alternating F12
transitions, exact values and order, complete evdev packets and no trailing
events. Each validated packet emits a flushed sequence receipt. Host checker
tests reject repeats, wrong values/types/codes and extra events; `make
test-host` and `make guest-input-probe` passed.

The external GUI harness at `7990ffc` sends brief native X11 F12 pairs and
measures receipt arrival on the host's monotonic clock. The interval includes
GUI, CC transport, guest execution and SSH return; it is an upper bound, not
one-way latency. Pairs share a starting timestamp and samples are correlated.

| Native GUI refresh | Transitions | Median ms | p95 ms | Maximum ms |
| --- | --- | --- | --- | --- |
| Live, first pass | 20 | 553.894 | 667.619 | 1269.621 |
| Stopped control | 20 | 355.786 | 469.041 | 469.055 |
| Live, repeat | 20 | 524.200 | 556.915 | 1232.137 |

Every counted pass ended with the guest's final assertion and SSH exit zero.
This is keyboard delivery evidence, not mouse latency or input-to-render
latency. The return-path contribution remains unmeasured, so these results
cannot localize delay or establish an acceptable one-way desktop response.

The running agentOS image was retained unchanged from `a9391b0`, SHA256
`d34fadcf4da8cc681893409a6b9802c593ab35f4513203f958e1c26c30ca677e`.
Native GUI runtime `ae21ece` binary SHA256:
`19a55f77b7cda03581c1f02d568e6c648bd27ab4769157b6743ea61aaaab1bb5`.
Guest probe SHA256:
`f05a26453253b91708134bd8a3a3b4a492b9a5eb06b55b7b78911b7bf3ee5908`.

The GUI's new measurement example compiled and checked, `make check` passed,
and all 130 browser tests passed. No production GUI or OS runtime changed.
The final native GUI close returned exit zero; the retained QEMU remains up.

Evidence directory:
`/home/jkh/.local/share/agentos-evidence/2026-09-19-native-input-latency/`.
It contains full sample JSON, stderr, screenshots, checks and probe executable
under a SHA256 manifest. Rejected held-key/autorepeat and final-deadline runs
are retained separately and excluded from the table. The exact SSH invocation
is retained in each benchmark stderr; it references the authorized key path,
never key contents. Fleet dispatch remains paused; integration/review and the
full v0.4 release qualification remain incomplete.
