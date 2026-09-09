# agentOS Tests: Host Unit Tests vs Target Proof

agentOS has **two distinct layers** of automated test, and they prove different
things. Conflating them is a category error: a green host run does **not** mean
the IPC contract holds on real seL4.

| Layer            | What runs                                   | seL4 IPC?        | How to run                                  |
|------------------|---------------------------------------------|------------------|---------------------------------------------|
| Host unit/mock   | `tests/**/*_test.c` under `-DAGENTOS_TEST_HOST` | **No** — `tests/microkit.h` stub echoes MR0 | `make test-host` |
| Simulator        | `userspace/sim/` in-memory seL4 model       | Modeled, in-process | `cargo test -p agentos-sim`                 |
| **Target proof** | seL4 root task + PDs in QEMU/hardware        | **Yes** — real `microkit_ppcall` | `make test-target`, `make run-tests`        |
| **Dual-guest acceptance** | seL4 + Ubuntu + FreeBSD + agentOS virtualizers | **Yes** | `make demo-test` |

> Rule of record (CLAUDE.md): never mock seL4 IPC for tests — the host stub is a
> *compile/logic* check only. The simulator and the target proof are the real
> coverage. This document exists so nobody mistakes the host mock for proof.

## Host unit / mock layer (NOT proof)

`tests/microkit.h` provides a stub `microkit_ppcall()` that simply returns its
argument and leaves the message registers untouched. So a contract test built
with `-DAGENTOS_TEST_HOST -I tests` exercises the *test body's* logic and the
struct/opcode definitions, but the PD under test is never invoked. Useful as a
fast compile gate and for pure-logic PDs (DVFS model, schedulers); useless as
proof that a live PD honours its contract.

Driven by:
- `make demo-smoke` — validates prerequisites and runs the host suite.
- `make test-host` / `make test-integration` — compile and run the host
  `tests/*.c` set.
- `make test-snapshot-sched`, `make test-power-mgr`, `make test-proc-server`,
  `make test-vibeos-contract` — individual host suites.

## Dual-guest authenticated-SSH acceptance

`make demo-test` builds one AArch64 agentOS image containing the Ubuntu and
FreeBSD VMM PDs, boots both guests concurrently, provisions a generated
Ed25519 key, and requires key-only SSH commands to succeed on ports 12222 and
12223. It is the non-interactive release-facing proof.

`make demo` runs the same gate and then retains both guests for manual SSH.
See `docs/demo.md` for the complete operator flow.

## Target proof layer (real seL4 IPC) — agentos-0h4

The target suite boots the actual seL4 root task and PDs in QEMU and issues
**real** `microkit_ppcall()`s across real channels, then reads the live replies.

- Runner: `tests/harness/target_contract_runner.c`
  → `target_contract_runner_main()` runs the SAME `run_*_tests(ch)` bodies that
  the host mock compiles, but against real channels:
  EventBus (`MONITOR_CH_EVENTBUS`), CC-PD (`CH_CC_PD`), serial_pd
  (`CH_SERIAL_PD`), log_drain (`CH_LOG_DRAIN`), guest lifecycle (`CH_GUEST_PD`).
- TAP: emitted to the serial console via `microkit_dbg_puts` (no libc), ending
  in the `TAP_DONE:<code>` sentinel that `xtask run-tests`
  (`xtask/src/cmd_run_tests.rs`) waits on.
- Image build: `make sel4-test-image BOARD=<board>` (sets `SEL4_TEST_IMAGE=1`,
  `GUEST_OS=none`).
- Gate: `make test-target` (per board) / `make test-target-all` (both arches),
  defined in `mk/target-tests.mk`.

```
make test-target TARGET_ARCH=aarch64 GUEST_OS=none
make test-target TARGET_ARCH=x86_64  GUEST_OS=none
```

## CC-PD VirtIO timeout proof — agentos-45b

CC-PD reaches its host controller over a VirtIO-MMIO serial console
(`build/cc_pd.sock`). `vio_serial_write()` / `vio_serial_read()` in
`kernel/agentos-root-task/src/cc_pd.c` spin on the VirtIO *used* ring with a
bounded wait (`CC_VIRTIO_WAIT_LIMIT`). If the ring never advances they log
`[cc_pd] TX timeout waiting for used ring` / `[cc_pd] RX timeout ...` and return
`false`, and the main loop `continue`s — the PD stays responsive.

`tests/harness/cc_virtio_timeout_test.sh` boots the test image with the CC-PD
console on a unix socket, sends one request frame, then **stops draining** the
socket so the TX used ring stalls. It asserts:
1. the bounded-wait timeout log line appears (error path is *observed*, not just
   compiled),
2. no kernel panic, and
3. QEMU/the root task is still alive afterwards (responsiveness).

Gate (in `mk/target-tests.mk`):

```
make test-cc-virtio-timeout BOARD=qemu_virt_aarch64
```

## How the runner is wired (agentos-8f5, DONE)

The root `Makefile` includes the gates (`-include mk/target-tests.mk`), and on
aarch64 a dedicated `test_runner` PD runs the contract suites against real seL4
IPC. The pieces:

1. **Runner PD** — `tests/harness/target_contract_runner.c` unity-`#include`s the
   `eventbus`, `serial_pd`, and `log_drain` suites (test_framework's `static`
   counters require a single translation unit), defines a libmicrokit shim
   (`microkit_dbg_*`, `microkit_name`, `microkit_pps`) that routes output to the
   PL011 UART — the release kernel disables `CONFIG_PRINTING` — and provides
   `pd_main`, which calls `target_contract_runner_main()` then emits `TAP_DONE`.
2. **Build** — `kernel/agentos-root-task/Makefile` compiles `target_contract_runner.o`
   (with `-Itests -Itests/harness`), links `test_runner.elf`, adds it to `IMAGES`,
   and appends a `test_runner` entry to a generated `agentos-test.toml` so
   `gen-pd-bundle` embeds it — all under `SEL4_TEST_IMAGE` only.
3. **Spawn + caps** — `system_desc_aarch64.c` appends the `test_runner` PD
   (`AGENTOS_SEL4_TEST_IMAGE` only) with the EventBus/serial_pd/log_drain
   endpoints minted at CNode slots `74 + ch` (`microkit_ppcall(ch)` =
   `seL4_Call(BASE_ENDPOINT_CAP + ch)`), priority 250 so a busy-polling PD can't
   starve it. `main.c` maps UART0 into the runner's vspace and, on aarch64,
   defers `TAP_DONE` to the runner (emitting it from the root task would make
   run-tests tear down QEMU before any PD runs).

Result: `make test-target TARGET_ARCH=aarch64 GUEST_OS=none` boots the image and
the runner emits real-IPC TAP (`ok 1..14`, `TAP_DONE:0`). On x86_64 (reduced
smoke, no runner) the root task still emits the boot-proof stub TAP.

### Not yet covered (tracked in agentos-yni)

- **cc_pd** — speaks its protocol over virtio-serial, not a seL4 endpoint, so a
  `microkit_ppcall` would block; needs a virtio-serial test driver.
- **guest lifecycle** — no guest VMM PD exists under `GUEST_OS=none`; needs a
  guest-enabled test image.

Also: EventBus STATUS/INIT return `AOS_ERR_INVAL` on target because its ring is
never mapped there (`eventbus_ring_vaddr` is only set by the host unit test) —
defect **agentos-gom**. The eventbus assertions accept that and tighten to strict
`OK` once the ring is wired.
