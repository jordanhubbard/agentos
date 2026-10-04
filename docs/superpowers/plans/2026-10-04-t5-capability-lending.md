# T5 — Capability lending (caretaker pattern)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Let one protection domain lend a capability to another for the duration of an operation, and withdraw it verifiably when the operation ends — so that authority can be passed temporarily instead of only granted permanently at boot.

**Architecture:** A **library, not a service**. The holder of a capability mints a badged, rights-reduced derivative, transfers it over an existing endpoint by IPC capability transfer, and calls `seL4_CNode_Revoke` when the operation completes. The kernel's derivation tree removes every descendant, so a borrower that sub-delegated cannot outlive the revocation. No broker, no third party: the lender is the only domain that *can* mint, because it is the one that holds the original.

**Spec:** [`docs/superpowers/specs/2026-10-03-trust-delegation-design.md`](../specs/2026-10-03-trust-delegation-design.md) — section "T5 — Capability lending".

## What this is, and what it is not

**Is:** a bounded, revocable loan. The borrower receives strictly less authority than the lender holds (rights-reduced, badged), uses it for one operation, and loses it on revocation whether or not it cooperates.

**Is not confinement.** Revocation withdraws *future* use. It does not undo what the borrower did while holding the capability, and it does not recover data the borrower copied. A lender that lends a frame must assume its contents were read.

**The bound is operation completion, not wall-clock time.** agentOS has no timer service (`services/timer-service/timer_svc.c` is still a stub), and a time-based lease would additionally need a policy for work in flight when a lease lapses mid-operation. Operation-scoped lending has neither problem and matches the stated use. Do not invent a TTL.

**The ledger cannot verify a lease.** T4's authority page is assembled from what the root task recorded granting plus what delegators report; seL4 exposes no capability-enumeration syscall. A lease that is reported is visible; a lease that is not reported is invisible. The kernel still enforces that no domain holds authority its lender did not hold — that part needs no reporting — but the *record* is only as honest as its reporter. Say so wherever the lease ledger is described.

## Global Constraints

- Language policy: C, Rust, or Assembly only. `make policy-check` enforces it.
- Host tests under `-DAGENTOS_TEST_HOST` are a compile/logic pre-filter only. Only a booted image asserted by an automated test proves target behaviour.
- **Target verification requires `SEL4_SDK_VERSION=2.1.0`** in this environment; CI's `os-claim-gate` job runs under the verified SDK artifact.
- Do not extend museum components. `cap_broker`, CapStore and `auth_server` are quarantined — a broker is explicitly the rejected design, and this task must not drift toward one.
- The root task parks after boot and enforces no policy afterward. **Do not add a resident authority that mints on request.** Lending is performed by the holder.
- Any component that lands updates `docs/TCB.md` in the same change with its qualification boundary stated.

## Review Focus

1. **A lease that outlives its revocation.** The whole property. If the borrower retains usable authority after `seL4_CNode_Revoke`, the feature is worse than nothing because it will be trusted. → Task 3 target proof.
2. **Rights escalation.** The derived capability must carry a strict subset of the lender's rights. A mint that preserves or widens rights breaks the subsetting invariant the whole track rests on. → Task 1, Task 2.
3. **A receive slot that is not empty.** `seL4_SetCapReceivePath` into an occupied slot silently fails to deliver, so the borrower proceeds with a stale capability from a previous loan. The existing code deletes before setting the path (`net_virt.c:567-570`) — follow it. → Task 2.
4. **Revocation that misses a sub-delegation.** `seL4_CNode_Revoke` on the lender's original removes all descendants; revoking the *derived* capability instead would not. Verify which object is revoked. → Task 2, Task 3.
5. **A proof that passes because nothing was lent.** If the borrower never successfully used the capability, "it faults after revocation" is vacuous. The proof must show use succeeding first. → Task 3.

---

### Task 1: Lease contract and host-testable bookkeeping

**Files:**
- Create: `kernel/agentos-root-task/include/contracts/lease_contract.h`
- Create: `libs/pd-support/cap_lease.c`
- Test: `tests/test_cap_lease.c`

**Interfaces produced:** `AOS_LEASE_VERSION`; `AOS_LEASE_MAX_ACTIVE` (8); `aos_lease_t { uint32_t lease_id; uint32_t borrower_pd; uint32_t rights; uint32_t state; }`; states `AOS_LEASE_FREE`, `AOS_LEASE_ACTIVE`, `AOS_LEASE_REVOKED`; `void aos_lease_table_init(aos_lease_table_t *)`; `int aos_lease_open(aos_lease_table_t *, uint32_t borrower_pd, uint32_t rights, uint32_t *out_id)`; `int aos_lease_close(aos_lease_table_t *, uint32_t lease_id)`; `const aos_lease_t *aos_lease_get(const aos_lease_table_t *, uint32_t lease_id)`.

This task is bookkeeping only — no seL4 calls. It tracks which leases are open so a lender can close them deterministically and so the state can be reported.

- [ ] **Step 1:** Write `tests/test_cap_lease.c` asserting: a fresh table has no active leases; opening returns a distinct id and marks `ACTIVE`; opening more than `AOS_LEASE_MAX_ACTIVE` fails rather than overwriting; closing marks `REVOKED` and frees the slot for reuse; closing an unknown or already-closed id fails rather than silently succeeding; `aos_lease_get` bounds-checks its id. Run it; it must fail.
- [ ] **Step 2:** Implement the header and `cap_lease.c` to satisfy exactly those assertions. Rights are stored for reporting; this layer does not interpret them.
- [ ] **Step 3:** Run the test; it must pass. Wire a `test-cap-lease-host` target into `make test-host`, modelled on `test-remoteos-client-host` (`Makefile:921-928`).
- [ ] **Step 4:** `make test-host` and `make policy-check`. Commit.

---

### Task 2: The lending primitive

**Files:**
- Create: `libs/pd-support/cap_lend.c`, `kernel/agentos-root-task/include/cap_lend.h`
- Modify: the descriptor to provision a lender/borrower pair for the test image only

**Interfaces produced:** `int aos_cap_lend(seL4_CPtr original, seL4_CPtr dest_cnode, seL4_Word dest_slot, seL4_Word dest_depth, seL4_CapRights_t rights, seL4_Word badge)`; `int aos_cap_lend_revoke(seL4_CPtr original)`.

- [ ] **Step 1: Mint the derivative.** `aos_cap_lend` uses `seL4_CNode_Mint` to produce a badged copy of `original` with **reduced** rights into the lender's own slot, ready for transfer. Assert at the call site that the requested rights are a subset of what the lender holds — a mint that preserves or widens rights breaks the subsetting invariant.

- [ ] **Step 2: Transfer by IPC.** The borrower prepares its receive slot and the lender sends. Follow the existing pattern exactly — `net_virt.c:567-570` deletes the receive slot, then calls `seL4_SetCapReceivePath(cnode, slot, bits)` before `seL4_Recv`. **Deleting first is not optional:** `seL4_SetCapReceivePath` into an occupied slot silently fails to deliver, and the borrower then proceeds with whatever was already there.

- [ ] **Step 3: Revoke the original, not the derivative.** `aos_cap_lend_revoke` calls `seL4_CNode_Revoke` on the **lender's original** capability. That removes every descendant, including anything the borrower sub-delegated. Revoking the derived capability instead would leave sub-delegations alive. State this in a comment — it is the single easiest thing to get wrong here.

- [ ] **Step 4: Provision a lender/borrower pair** in the descriptor, **behind a build guard for a test image only** (follow how `native_rust_probe`/`native_rust_client` are added — they are absent from the default image). The default booted PD set must not change: no new TCB surface for a demonstration.

- [ ] **Step 5:** `make test-host`, `make policy-check`, and the aarch64 boot test with `GUEST_OS=none`. The default image must be unchanged — confirm the PD count and the authority page's `pd_count` are untouched. Commit.

---

### Task 3: Target proof

**Files:** `xtask/src/lib.rs`, `xtask/src/cmd_test.rs`, `Makefile`, `.github/workflows/ci.yml`, `docs/TCB.md`

Three probes, in this order. **Probe 1 is what makes probes 2 and 3 meaningful** — without it, "the borrower faults" could be true because nothing was ever lent.

- [ ] **Probe 1 — the loan works.** The borrower receives the lent capability and *successfully uses it* (map the frame and read a known byte pattern the lender wrote). Assert the exact bytes. A loan that never worked proves nothing about revocation.
- [ ] **Probe 2 — revocation withdraws it.** The lender revokes; the borrower's next access to the same address **faults**. Follow the existing fault-probe oracle: root emits its success marker only on an exact badge, address and access-direction match (`main.c:134-140`, `ROOT_PROBE_*`). A timeout, an unrelated fault, or a normal completion must not satisfy it.
- [ ] **Probe 3 — sub-delegation does not survive.** The borrower mints its own copy before revocation; after the lender revokes the original, **both** the borrower's received capability and its sub-delegated copy must be dead. This is the property that distinguishes revoking the original from revoking the derivative, and it is the one a careless implementation gets wrong.

- [ ] **Step 4: Prove probe 2 is not vacuous.** Temporarily make `aos_cap_lend_revoke` a no-op, rebuild, and confirm probe 2 **fails** — the borrower still reads successfully. Restore and confirm it passes. Paste all three outputs.
- [ ] **Step 5: Makefile and CI.** Add `test-cap-lending` to `gate` **and** as a step in the `os-claim-gate` job in `.github/workflows/ci.yml`, beside the existing gate steps. No CI job invokes `make gate`, so a proof reachable only through that target never runs.
- [ ] **Step 6: `docs/TCB.md`.** State precisely:

```markdown
A protection domain may lend a capability to another for the duration of an
operation: the holder mints a badged, rights-reduced derivative, transfers it by
IPC capability transfer, and calls seL4_CNode_Revoke on its own original when
the operation ends, which removes every descendant. `make test-cap-lending`
verifies on target that the borrower can use the lent capability, that its next
access faults after revocation, and that a copy the borrower sub-delegated is
dead too.

Limits. Revocation withdraws future use; it does not undo what the borrower did
while holding the capability and does not recover data the borrower copied —
lending bounds authority in time, it is not confinement. The bound is operation
completion, not elapsed time: agentOS has no timer service. The lender/borrower
pair exists only in the test image; no default-image PD lends anything yet.
```

- [ ] **Step 7:** Full verification — `make test-host`, `make policy-check`, the aarch64 boot test, `make test-cap-lending`, plus `make test-authority`, `make test-cc-envelope`, `make test-image-verify` and `make test-entropy-unavailable` to confirm nothing regressed. Report that `make gate` was not run. Commit.

---

## Notes for the implementer

**This is the task most likely to drift toward a broker.** If you find yourself adding a component that mints capabilities on behalf of others, stop — that design is explicitly rejected in the spec, because a component able to mint any capability for any requester is ambient authority and its compromise is total. The lender mints, because the lender is the only domain that holds the original.

**Revoke the original.** Not the derivative. `seL4_CNode_Revoke` on the lender's own capability removes the whole subtree; revoking the copy you handed out leaves the borrower's sub-delegations alive. Probe 3 exists to catch exactly this.

**Do not change the default image.** The demonstration pair belongs in a test variant. Adding TCB surface to prove a library works is the wrong trade, and the authority page's `pd_count` assertion will catch you if you do it by accident.
