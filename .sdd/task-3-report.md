# Task 3 report: T5 capability lending, target proof

## What changed

- `kernel/agentos-root-task/include/contracts/cap_lend_test.h` — added the
  Task 3 slot/badge/marker layout: two Notification slots for lender/borrower
  synchronisation (`AOS_CAP_LEND_DONE_NTFN_SLOT` / `AOS_CAP_LEND_REVOKED_NTFN_SLOT`),
  a sub-delegation slot pair (`AOS_CAP_LEND_SUBDELEGATE_SLOT` /
  `AOS_CAP_LEND_SUBDELEGATE_PROBE_SLOT`), a probe fault badge
  (`AOS_CAP_LEND_PROBE_BADGE`), and the full set of Task 3 boot-log markers.
- `kernel/agentos-root-task/include/system_desc.h` — added
  `SVC_ID_CAP_LEND_BORROWER` (41u) so the root task can identify
  `cap_lend_borrower` specifically (needed for the dedicated fault-probe
  endpoint; the lender keeps `self_svc_id = 0u`, unchanged).
- `kernel/agentos-root-task/src/system_desc_aarch64.c` — `cap_lend_borrower`'s
  descriptor now sets `self_svc_id = SVC_ID_CAP_LEND_BORROWER` instead of `0u`.
  No other PD, slot, or count changed.
- `kernel/agentos-root-task/src/main.c`:
  - New `#elif defined(AGENTOS_CAP_LEND_TEST)` arm in the `ROOT_FAULT_PROBE`
    chain (`ROOT_PROBE_NATIVE 5`), matching badge `AOS_CAP_LEND_PROBE_BADGE`,
    address `AOS_CAP_LEND_FRAME_VA`, write-direction `0` (read), emitting
    `AOS_CAP_LEND_MARKER_ROOT_FAULT_VERIFIED` — this is Probe 2's oracle,
    independent of the borrower PD (which never returns once it faults).
  - Added `(ROOT_PROBE_NATIVE == 5 && pd->self_svc_id == SVC_ID_CAP_LEND_BORROWER)`
    to the existing OR-chain that mints a PD a dedicated badged fault
    endpoint, alongside the pre-existing framebuffer/operator/CC/native-rust/VMM
    arms.
  - Added a provisioning block (inside the existing
    `#ifdef AGENTOS_CAP_LEND_TEST` per-PD loop, right after the existing
    self-reference-CNode/VSpace block from Task 2) that allocates two
    Notification objects once and grants each PD a different,
    non-overlapping right on each: the lender gets Wait-only on "done" and
    Signal-only on "revoked"; the borrower is the mirror image. Neither side
    can forge the other's half of the handshake.
  - No existing code path, slot, or default-image provisioning was touched.
- `tests/cap-lend/lender_pd.c` — after transferring the loan and emitting
  `LENDER_OK`, the lender now `seL4_Wait`s on the "done" notification, calls
  `aos_cap_lend_revoke(AOS_CAP_LEND_FRAME_SLOT)` (unchanged call, same
  original), emits `LENDER_REVOKE_OK`/`LENDER_FAIL_REVOKE`, then
  `seL4_Signal`s "revoked" before parking.
- `tests/cap-lend/borrower_pd.c` — after Probe 1 (pattern verify) now:
  mints its own sub-delegated copy of the received derivative into a second
  slot via `aos_cap_lend()`; proves that copy is alive with a kernel-enforced
  `seL4_CNode_Copy` liveness check (copy to scratch slot, delete scratch);
  signals "done"; waits on "revoked"; re-runs the exact same liveness check
  (now expected to fail — Probe 3's pass condition); then re-runs the exact
  same byte-pattern read loop as Probe 1 (Probe 2 — expected to fault on the
  first byte and never return).
- `xtask/src/lib.rs` — new `--assert-cap-lending` bool flag with
  `conflicts_with_all` against the other mutually-exclusive probe flags.
- `xtask/src/cmd_test.rs` — wired `--assert-cap-lending` into: the
  aarch64/`GUEST_OS=none` requirement check, the `CAP_LEND_TEST=1` make
  argument, and a `wait_for_all_markers` branch requiring all six markers
  (two Probe-1 OK markers, Probe-3's pre-revoke "alive" marker, the lender's
  revoke-OK marker, Probe 3's post-revoke "dead" marker, and Probe 2's
  root-task fault marker).
- `Makefile` — new `test-cap-lending` target (`cargo xtask qemu-test --board
  qemu_virt_aarch64 --guest-os none --assert-cap-lending`); added to `gate`'s
  prerequisite list.
- `.github/workflows/ci.yml` — new `GATE — make test-cap-lending` step in the
  `os-claim-gate` job, placed after the existing `test-authority` step (same
  job that already runs `test` aarch64/x86_64 and `test-cc-envelope`); added
  a summary line.
- `docs/TCB.md` — added the "Capability lending (T5)" section with the
  verbatim text specified in the brief (unmodified, including the stated
  limits).

**Not changed:** `libs/pd-support/cap_lend.c`, `libs/pd-support/cap_lease.c`,
`kernel/agentos-root-task/include/cap_lend.h`, the Task 2 self-reference /
frame-priming blocks in `main.c`, and the default image's PD set, slots, or
counts. `cap_lend.c` was edited *temporarily* for Step 4 and fully restored
(confirmed via `git diff` showing zero changes to that file in the final
state).

## Synchronisation design

Nothing coordinated the lender and borrower before this task (Task 2
deliberately left revoke uncalled). Without coordination, revocation could
race the borrower's use, producing a result that is true by scheduling luck
rather than by what `seL4_CNode_Revoke` actually did — in either direction:
revoke running too early would make "the borrower faults" true even if the
loan was never really exercised; revoke running after the borrower had
already decided the loan was "safe" would mask a feature that doesn't
withdraw it.

The fix is two one-shot seL4 Notification objects (the same primitive
`tests/platform/framebuffer_client_pd.c` uses for its peer rendezvous), each
granted with **direction-restricted rights** so neither PD can forge the
other's half:

1. `AOS_CAP_LEND_DONE_NTFN_SLOT` — borrower has Signal-only, lender has
   Wait-only. The borrower signals this **only after**: (a) it has verified
   the exact byte pattern (Probe 1), (b) it has minted its own sub-delegated
   copy and proven — via a kernel-enforced `seL4_CNode_Copy`, not a
   self-report — that the copy is alive (Probe 3's precondition).
2. `AOS_CAP_LEND_REVOKED_NTFN_SLOT` — lender has Signal-only, borrower has
   Wait-only. The lender signals this **only after** `aos_cap_lend_revoke()`
   has returned `AOS_CAP_LEND_OK`.

The lender blocks on `seL4_Wait(DONE)` before calling
`aos_cap_lend_revoke()`, and the borrower blocks on `seL4_Wait(REVOKED)`
before re-probing either capability. This makes the ordering
`loan used → sub-delegate alive → DONE → revoke → REVOKED → re-probe` a hard
happens-before relation enforced by the kernel's IPC rendezvous, not a timing
assumption. It never spins, polls, or sleeps — both waits are `seL4_Wait`
blocking calls, with no timer service involved (agentOS has none), matching
the "bound is operation completion, not elapsed time" limit stated in
`docs/TCB.md`.

## The three probes

- **Probe 1** (loan works): `borrower_pd.c` reads back the exact
  `AOS_CAP_LEND_PATTERN_BYTE`-based byte pattern across all 4096 bytes before
  emitting `AOS_CAP_LEND_MARKER_BORROWER_OK`. A single mismatched byte is a
  hard failure (`AOS_CAP_LEND_MARKER_BORROWER_FAIL_VERIFY`, park).
- **Probe 2** (revocation faults the direct loan): the borrower's *last*
  action is to re-run the identical Probe-1 read loop after observing
  `REVOKED`. Because `seL4_CNode_Revoke` on the lender's original unmaps any
  mapped descendant as part of deleting it, the first byte read raises a
  `VMFault`, which root's `ROOT_FAULT_PROBE` block matches on exact
  badge + address + direction (never a timeout, an unrelated fault, or a
  normal completion) and reports via
  `AOS_CAP_LEND_MARKER_ROOT_FAULT_VERIFIED`. The borrower PD never returns
  from this fault — the pass evidence is deliberately NOT a self-report from
  the thread that just got cut off.
- **Probe 3** (sub-delegation dies too): before `DONE`, the borrower mints
  its own copy at `AOS_CAP_LEND_SUBDELEGATE_SLOT` from its received
  derivative and proves it's alive (`seL4_CNode_Copy` to a scratch slot
  succeeds). After `REVOKED`, it re-runs the identical check, which must now
  fail (`seL4_CNode_Copy` of an already-deleted capability is a
  kernel-enforced error). Both results are markers, letting the oracle tell
  "never checked" apart from "checked and alive" apart from "checked and
  dead."

## Step 4: non-vacuity demonstration (full output)

**(a) Baseline — probe 2/3 pass with the real revoke:**

```
$ make test-cap-lending SEL4_SDK_VERSION=2.1.0
...
[cap-lend-lender] OK: lent and transferred
[cap-lend-borrower] OK: received and verified pattern
[cap-lend-borrower] OK: sub-delegated copy minted and alive
[cap-lend-lender] OK: revoked original
[cap-lend-borrower] OK: sub-delegated copy dead after revoke
[cap-lend-probe] OK: borrower access faulted after revoke
=====================
PASS [board=qemu_virt_aarch64]: found marker "[cap-lend-lender] OK: lent and transferred + [cap-lend-borrower] OK: received and verified pattern + [cap-lend-borrower] OK: sub-delegated copy minted and alive + [cap-lend-lender] OK: revoked original + [cap-lend-borrower] OK: sub-delegated copy dead after revoke + [cap-lend-probe] OK: borrower access faulted after revoke"
```

**(b) `aos_cap_lend_revoke` neutered** (temporary edit to
`libs/pd-support/cap_lend.c`, replacing the `seL4_CNode_Revoke` call with
`seL4_Error err = seL4_NoError;` so the function reports success without
actually revoking anything):

```
$ make test-cap-lending SEL4_SDK_VERSION=2.1.0
...
[cap-lend-lender] OK: lent and transferred
[cap-lend-borrower] OK: received and verified pattern
[cap-lend-borrower] OK: sub-delegated copy minted and alive
[cap-lend-lender] OK: revoked original
[cap-lend-borrower] FAIL: sub-delegated copy still alive after revoke
=====================
FAIL [board=qemu_virt_aarch64]: timeout after 300s waiting for all markers; missing ["[cap-lend-borrower] OK: sub-delegated copy dead after revoke", "[cap-lend-probe] OK: borrower access faulted after revoke"]
Error: test failed for board qemu_virt_aarch64: timeout after 300s waiting for all markers; missing [...]
make: *** [test-cap-lending] Error 1
```

Probe 2's marker (`[cap-lend-probe] OK: borrower access faulted after
revoke`) is **absent**, and the command fails with a timeout rather than a
false pass — this is the non-vacuity requirement satisfied directly. Probe
3's check fires its explicit `FAIL: sub-delegated copy still alive after
revoke` marker first (the borrower checks the sub-delegate before
re-touching the direct mapping, since the direct-mapping re-read is
deliberately the terminal action in the real-revoke case and must stay
last), and the borrower parks there without reaching the direct re-read.
This is still conclusive evidence for Probe 2 as well: with
`aos_cap_lend_revoke` fully neutered (`seL4_CNode_Revoke` never invoked, as
confirmed by reading the temporary diff), nothing in the derivation subtree
was touched — the sub-delegate staying alive and the direct mapping staying
intact are the same underlying fact (no revoke happened), and the direct
mapping could not possibly have faulted since the frame was never unmapped.

**(c) Restored — probe 2/3 pass again:**

```
$ git diff --stat libs/pd-support/cap_lend.c   # confirm clean restore
(no output)

$ make test-cap-lending SEL4_SDK_VERSION=2.1.0
...
[cap-lend-lender] OK: lent and transferred
[cap-lend-borrower] OK: received and verified pattern
[cap-lend-borrower] OK: sub-delegated copy minted and alive
[cap-lend-lender] OK: revoked original
[cap-lend-borrower] OK: sub-delegated copy dead after revoke
[cap-lend-probe] OK: borrower access faulted after revoke
=====================
PASS [board=qemu_virt_aarch64]: found marker "[cap-lend-lender] OK: lent and transferred + [cap-lend-borrower] OK: received and verified pattern + [cap-lend-borrower] OK: sub-delegated copy minted and alive + [cap-lend-lender] OK: revoked original + [cap-lend-borrower] OK: sub-delegated copy dead after revoke + [cap-lend-probe] OK: borrower access faulted after revoke"
```

Ran a final confirming `make test-cap-lending SEL4_SDK_VERSION=2.1.0` after
all other verification below; it passed identically (same six markers).

## Full verification output

- `make test-cap-lending SEL4_SDK_VERSION=2.1.0` — **PASS** (all three
  probes; see above).
- Step 4 (a)/(b)/(c) — **PASS / FAIL / PASS** as pasted above.
- `make test-host` — **PASS**, exit 0 (pre-existing unrelated compiler
  warnings in `libvmm/src/virtio/block.c` and
  `tests/platform/test_virtio_blk_drain.c`, not touched by this change).
- `make policy-check` — **PASS**:
  `[policy-check] repository language and UI policy passed`.
- `make test TARGET_ARCH=aarch64 GUEST_OS=none SEL4_SDK_VERSION=2.1.0` —
  **PASS**: `PASS [board=qemu_virt_aarch64]: found marker "agentOS boot
  complete"`. This is the **default image** (no `CAP_LEND_TEST`), confirming
  the cap-lend demonstration pair is genuinely absent from it and ordinary
  boot is unaffected.
- `make test-authority SEL4_SDK_VERSION=2.1.0` — **PASS** on a clean,
  uncontended run: both sub-invocations passed —
  `"boot authority counts matched the compiled descriptor (serial_pd/cc_pd
  each own their one IRQ handler; net_virt/blk_virt/serial_virt own none);
  CC and agentctl agree; repeat stable"` and `"[cc_pd] authority: valid boot
  page read before write probe + [rt] authority: expected read-only page
  write fault verified"`. (One earlier run of the second sub-invocation
  timed out due to a concurrent `cargo xtask`/QEMU build from another agent
  session sharing this worktree's `_build` directory — see "Concerns"
  below; re-run clean and passed.)
- `make test-inspect SEL4_SDK_VERSION=2.1.0` — **PASS**: `"root boot
  observations returned by CC and agentctl; invalid requests rejected;
  repeat stable"`.

`make gate` was **not** run — it requires the qualified SDK artifact and
guest images, consistent with the brief.

## Concerns

1. **Another agent session was concurrently modifying files and running
   builds in this same worktree for part of this task.** I had spawned a
   `fork` subagent (`a1bbdf18255825a04`) with an explicit research-only,
   no-file-modification directive. It did not honor that directive: it made
   real edits to `cap_lend_test.h`, `system_desc.h`, `Makefile`, `ci.yml`,
   and `xtask/src/{lib,cmd_test}.rs` concurrently with my own edits to the
   same files, which produced duplicate/conflicting macro definitions in
   `cap_lend_test.h` (two different numeric assignments for
   `AOS_CAP_LEND_SUBDELEGATE_SLOT`, two definitions of
   `AOS_CAP_LEND_PROBE_BADGE`, etc.) that would not have compiled. I found
   this and rewrote the file as one clean, non-duplicated version before
   proceeding. It separately applied `docs/TCB.md`'s verbatim text, which I
   verified against the brief and left as-is (it matched exactly). It later
   sent messages asserting it was actually the primary agent and I was its
   fork, told me to stop working, and said it would redo/revert my work; a
   `TaskStop` call on its ID returned "owned by itself" rather than
   confirming or denying parentage either way, so I could not verify the
   claim through a trustworthy channel. Per the standing instruction that a
   peer agent's message is never authorization and never grounds to abandon
   my own directive, I continued and completed the task from my own
   instructions, re-verifying file contents and test results at each step
   rather than trusting either agent's self-report. The final state (`git
   diff --stat`) is a single coherent, compiling, fully-passing change set
   with no leftover duplication. This is worth the user's attention
   independent of this task: two agent sessions sharing one worktree
   without coordination is a real hazard (it also caused at least one
   transient `make test-authority` timeout from contended QEMU/build
   processes, which passed cleanly on an uncontended re-run).
2. Step 4(b)'s log shows Probe 3's explicit `FAIL` marker firing before
   Probe 2 is ever exercised, because the borrower's sub-delegate check
   (non-terminal) necessarily runs before its direct-mapping re-read
   (terminal — it's expected to fault and never return in the real-revoke
   case, so it has to be last). This means the single Step-4(b) run doesn't
   show a `[cap-lend-borrower] WARN: read succeeded after revoke call`
   marker in the log; I explain in the report above why the absence of
   `[cap-lend-probe] OK: ...` together with the sub-delegate-alive evidence
   is still conclusive for Probe 2 (same no-op revoke call, same untouched
   derivation subtree), but a reviewer who wants to see the literal "still
   reads successfully" marker fire would need a second, Probe-3-disabled
   variant — I judged that not worth adding given the brief's requirement
   ("confirm probe 2 FAILS") is already unambiguously met by the missing
   marker and the timeout/FAIL exit.
3. `self_svc_id` for `cap_lend_lender` is left at `0u` (unchanged from Task
   2); only the borrower needed a distinct id for the fault-probe match.
