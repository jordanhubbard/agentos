# Functional SSH qualification — macOS AArch64

Cold standalone runs at clean revision
`436afaea615903d4f4a005878225ebbcea28ba32` passed every required step:
PTY/login, account/kernel, file/process operations, gateway/DNS, and package
installation, execution, query and removal. The terminal transcripts here
render control characters visibly with `cat -v` and trim trailing whitespace;
the original captured bytes remain in their local session evidence directories.

| Guest | Command | Image SHA-256 | Session |
| --- | --- | --- | --- |
| Ubuntu live | `CARGO_INCREMENTAL=0 make test-ubuntu-live QEMU_TEST_TIMEOUT=3600 QEMU_TEST_SSH_PORT=13222` | `50ac23244662ee40b06330bcd2611cfd9811e66e7b40147dfca8501be8f3b863` | `guest-session-HZLLt2` |
| FreeBSD 15 | `CARGO_INCREMENTAL=0 make e2e-freebsd15 QEMU_TEST_TIMEOUT=3600 QEMU_TEST_SSH_PORT=13223` | `41fdda39944aba1385ec0c10e7ffc39a0f8c2ad4a17113d5c11b7d97034a34f5` | `guest-session-GhvyRD` |

The associated v2 boot-timing receipts retain their original remote-exec
readiness boundary; they do not measure completion of the later functional
steps. The terminal transcripts and outer test success provide that evidence.
The standalone FreeBSD receipt's `host_backed_virtio` flag is false because
that command did not request the optional CLI assertion; its profile still
asserted the host-backed device markers before passing.

`make gate QEMU_TEST_TIMEOUT=600` also passed at that revision, including the
host suite, ARM/x86 boot, real backend builds, Buildroot network/block I/O,
and Ubuntu console proof. Raw gate/test logs, serial logs and compressed
images remain locally under `build/evidence/ssh-functional-436afaea/`.

An earlier cold dual Ubuntu/FreeBSD run passed at pre-v0.4 implementation
revision `7a41804abebc9982d356412f9d1b3d256de805bd`, image
`ef6ecd31c2809e906fc53ad5f8592e3450633db2740dfcf00d9a5df80738a8da`.
Its image and implementation patch are retained locally under
`build/evidence/ssh-functional-7a41804a/`; it is not an exact-revision receipt
for the subsequently rebased v0.4 implementation.

These checks use local package fixtures, not remote repository downloads.
FreeBSD's writable live package state is ephemeral. Its transcript records
one lost initial ICMP packet out of three; investigation is tracked in
`task_d14f86b7a6894fedaed4e3f1b06607a8`. See the
[acceptance contract](../../guest-session-acceptance.md) for scope and limits.

The first integrated Debian/FreeBSD cold run at `7388ed4c` passed FreeBSD's
functional session and Debian's remote-exec readiness, then correctly failed
Debian's login session (`guest-session-UrAa8W`). Its transcript reported
`Timeout, server 127.0.0.1 not responding.` The functional runner had selected
the short readiness keepalives (5-second interval, one unanswered request).
Revision `e2e3758c` uses the existing session keepalives instead, retaining
the independent 600-second host deadline and all command assertions.
The failed run's image hash is
`eaddcb569f4b72c0b969e1683597e907ed1755bbbdd81f12687d6b49d7b4082b`;
raw evidence remains under `build/evidence/ssh-functional-7388ed4c/`.

The cold retry at clean revision `e2e3758c` passed
`CARGO_INCREMENTAL=0 make demo-test DUAL_OS_TEST_TIMEOUT=3600` with the
**same image hash**. Both guests passed every functional step:
FreeBSD `guest-session-6o2EgZ` and Debian `guest-session-9waPGk`.
Debian used its normal `debian` account, seeded key-only authentication,
and pinned Ed25519 host identity. The test also passed concurrent readiness,
suspend/resume checkpoints, and final guest destruction/stale-handle checks.
The `*-dual-terminal.txt` files retain both successful command sessions.
Raw image, serial and test logs remain in
`build/evidence/ssh-functional-e2e3758c/`.
`make test-host` also passed at this revision: 133 Rust tests and the C suite.
