# T6 — Hierarchical delegation and dynamic domain creation

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Let a protection domain create a child domain at run time and endow it from the parent's own authority, enforcing that no domain ever holds authority its parent did not hold.

**Why this exists:** seL4's assurance argument assumes the component set is known at build time, and `system_desc_aarch64.c` is a compile-time table for exactly that reason. That is right for an avionics box and wrong for a platform whose purpose is hosting agents created in response to work. This is the task that removes the assumption — and the invariant below is what keeps the authority graph analysable once it does.

**Spec:** [`docs/superpowers/specs/2026-10-03-trust-delegation-design.md`](../specs/2026-10-03-trust-delegation-design.md) — "Adopted model: hierarchical delegation".

## Scope — read before planning any code

**This creates a DOMAIN at run time. It does not load new CODE at run time.**

The child's ELF still comes from the signed bundle verified at boot (T3). What becomes dynamic is *who exists, with what authority, decided by a parent while the system runs* — not *what instructions the machine will execute*. That distinction is the whole reason this is tractable and safe: T3's guarantee that every image was signed before it ran is preserved exactly.

Loading unsigned or runtime-generated code is a different problem, needs its own threat analysis, and is **out of scope**. Do not drift toward it. If the design starts to require fetching an ELF from anywhere but the verified bundle, stop and report.

**The invariant, which is the entire safety argument:**

> No domain ever holds authority its parent did not hold.

seL4 enforces the mechanism for free — a parent cannot mint from a capability it does not possess. What this task must get right is that the *endowment path* cannot be tricked into granting something the parent holds but did not intend, and that the result is observable.

**What this is not:**
- Not a broker. The parent endows from its own capabilities. A component that mints for arbitrary requesters is ambient authority; that design is explicitly rejected.
- Not verified by the T4 ledger. The ledger records what is *reported*; seL4 exposes no capability-enumeration syscall. The kernel enforces subsetting regardless of reporting, but the record is only as honest as its reporter. Say so wherever the ledger is described.
- Not unbounded. A parent can only create children from an untyped pool it was granted. Exhaustion is a resource limit, not an authority limit.

## Global Constraints

- Language policy: C, Rust, or Assembly only. `make policy-check` enforces it.
- **Target verification requires `SEL4_SDK_VERSION=2.1.0`** here; CI's `os-claim-gate` runs under the verified SDK artifact.
- The root task parks after boot and enforces no policy afterward. It provisions the parent's untyped pool at boot; it must not gain a runtime role.
- **The default image must not change.** Parent/child demonstration PDs go behind a build guard, as `native_rust_probe`/`native_rust_client` and the T5 lender/borrower pair are. `make test-authority` and `make test-inspect` both assert exact PD counts — run **both**; a PD-count site was missed in a sibling task and reached CI.
- Do not modify anything under `services/legacy-pds/`; no `cap_broker`/`capstore`/`auth_server`.
- `docs/TCB.md` gains this component's qualification boundary in the same change.

## Precedent to follow, not reinvent

PDs already retype from granted untyped pools in this tree — read these first:
- `platform/blk-virt/blk_virt.c:518` — retypes a page from `AOS_QUEUE_SERVICE_RECEIVE`.
- `platform/guest-ram/vmm_guest_paging.c:22` — retypes a **VSpace object** from `AOS_GUEST_PAGING_POOL_CAP`.
- `platform/guest-ram/vmm_guest_execution.c:27` — retypes from `AOS_GUEST_EXECUTION_POOL_CAP`.
- `kernel/agentos-root-task/src/pd_tcb.c:30` — `pd_tcb_create`, how root binds a TCB to a CSpace, VSpace and IPC buffer via `seL4_TCB_Configure`.

The parent is doing a scoped version of what root does at boot. Mirror root's sequence rather than inventing one.

T5's `libs/pd-support/cap_lend.c` already mints badged, rights-reduced derivatives and revokes the lender's own original. **Use it for the endowment** rather than writing a second minting path — and note that a child's endowment is permanent for the child's life, where a T5 loan is withdrawn at operation end. Same primitive, different lifetime.

## Review Focus

1. **Endowment that exceeds the parent.** The one invariant. Every capability the child receives must be a rights-reduced derivative of one the parent holds. → Tasks 1, 2, 3.
2. **A child that starts without its full endowment.** A partially-endowed child that runs anyway is worse than one that fails to start: it is a domain whose authority nobody described. Endowment must complete before the TCB is resumed, and a failed endowment must leave the child unstarted. → Task 2.
3. **Untyped exhaustion mid-creation.** Running out of pool after retyping a CNode but before the VSpace leaves orphaned objects and a half-built domain. → Task 2.
4. **A proof that passes because the child did nothing.** "The child cannot do X" is vacuous if the child never ran. The child must demonstrably exercise what it *was* given before anything asserts what it was not. → Task 3.
5. **The ledger describing a domain that does not match reality.** The reported endowment and the actual grants must agree, and the gap between "reported" and "enforced" must be stated, not implied. → Tasks 2, 3.

---

### Task 1: Endowment descriptor and subsetting validation

**Files:** create `kernel/agentos-root-task/include/contracts/endowment_contract.h`, `libs/pd-support/endowment.c`; test `tests/test_endowment.c`.

A host-testable description of what a parent intends to grant a child, with validation that the request is a subset of what the parent declared it holds. No seL4 calls in this layer.

**Interfaces:** `AOS_ENDOWMENT_VERSION`; `AOS_ENDOWMENT_MAX_CAPS` (8); `aos_endow_cap_t { uint32_t kind; uint32_t rights; uint32_t parent_slot; }`; `aos_endowment_t { uint32_t version; uint32_t count; aos_endow_cap_t caps[8]; }`; `int aos_endowment_validate(const aos_endowment_t *req, const aos_endowment_t *parent_holdings)`.

- [ ] **Step 1:** Write `tests/test_endowment.c` asserting: a request naming only capabilities present in `parent_holdings`, each with rights a **subset** of the parent's, validates; a request naming a capability the parent does not hold is **rejected**; a request whose rights exceed the parent's for a held capability is **rejected**; `count == 0` is rejected (an endowment granting nothing is a mistake, not a domain); `count > AOS_ENDOWMENT_MAX_CAPS` is rejected without reading past the array; a version mismatch is rejected before anything else; NULL for either argument is rejected rather than dereferenced. Run it; it must fail.
- [ ] **Step 2:** Implement to satisfy exactly those assertions. Rights comparison is a subset test on the bitmask — `(req & ~parent) == 0` — not equality, and not "non-zero".
- [ ] **Step 3:** Run the test; it must pass. Wire `test-endowment-host` into `make test-host`, modelled on `test-remoteos-client-host` (`Makefile:921-928`).
- [ ] **Step 4:** `make test-host`, `make policy-check`. Commit.

---

### Task 2: The spawn primitive

**Files:** create `libs/pd-support/child_spawn.c`, `kernel/agentos-root-task/include/child_spawn.h`; modify the descriptor to provision a parent with an untyped pool and a child ELF, **behind a build guard only**.

**Interface:** `int aos_child_spawn(const aos_child_spawn_req_t *req, aos_child_handle_t *out)` — retype the child's CNode, VSpace and TCB from the parent's pool, endow it per a validated `aos_endowment_t`, configure and start it.

- [ ] **Step 1: Retype from the parent's pool.** Follow `vmm_guest_paging.c:22` for VSpace objects and `blk_virt.c:518` for the general retype shape. The parent holds an untyped pool granted by root at boot; it must not reach for anything else.
- [ ] **Step 2: Endow using T5's primitive.** Mint each capability in the validated endowment with `aos_cap_lend`'s mint path into the child's CNode. **Validate against the parent's actual holdings first** — Task 1's check is on a declaration; this is where it binds to reality.
- [ ] **Step 3: Configure and start, in that order.** Mirror `pd_tcb.c:30`'s `seL4_TCB_Configure` sequence. **The TCB must not be resumed until every endowment succeeded.** A partially-endowed running child is a domain whose authority nobody described — on any endowment failure, tear down and return an error with nothing started.
- [ ] **Step 4: Handle pool exhaustion.** If retype fails partway, free or account for what was already created. Report which step failed. An orphaned CNode with no VSpace is a leak that will be invisible later.
- [ ] **Step 5: Report the endowment** into the lease/ledger bookkeeping so T4's authority page can show the child. Note in the code that this is a *report*, not proof — seL4 cannot be asked what a domain holds.
- [ ] **Step 6:** Provision the parent and child behind a build guard, following the T5 lender/borrower pair. The child's ELF comes from the **verified bundle**; do not add a path that loads from anywhere else.
- [ ] **Step 7:** `make test-host`, `make policy-check`, the aarch64 boot test, **and both** `make test-authority` and `make test-inspect` — the default image must be unchanged. Commit.

---

### Task 3: Target proof

**Files:** `xtask/src/lib.rs`, `xtask/src/cmd_test.rs`, `Makefile`, `.github/workflows/ci.yml`, `docs/TCB.md`.

Four probes. **Probe 1 is what makes 2–4 mean anything.**

- [ ] **Probe 1 — the child runs and uses what it was given.** It exercises an endowed capability and reports a specific, asserted result (read an exact byte pattern from an endowed frame). Not "it started" — what it *did*.
- [ ] **Probe 2 — the child cannot exceed its endowment.** It attempts an access the parent deliberately withheld and **faults**. Use the existing fault-probe oracle (`main.c:134-140`, `ROOT_PROBE_*`): exact badge, address and direction. A timeout or unrelated fault must not satisfy it.
- [ ] **Probe 3 — a failed endowment leaves nothing running.** Force one endowment step to fail; assert the child never starts and the parent reports the failure. This pins Review Focus item 2.
- [ ] **Probe 4 — the ledger shows the child.** After a successful spawn, the T4 authority page reports the child domain with the endowed capability kinds. Assert the counts match what was endowed.
- [ ] **Step 5: Prove probe 2 is not vacuous.** Temporarily endow the withheld capability too; rebuild; confirm probe 2 **fails** — the child now succeeds where it should have faulted. Restore; confirm it passes. Paste all three outputs.
- [ ] **Step 6: CI.** Add `test-child-spawn` to `gate` **and** as a step in the `os-claim-gate` job. No CI job invokes `make gate`.
- [ ] **Step 7: `docs/TCB.md`:**

```markdown
A protection domain may create a child domain at run time and endow it from its
own authority. The parent retypes the child's CNode, VSpace and TCB from an
untyped pool root granted it at boot, mints rights-reduced derivatives of
capabilities it already holds into the child's CSpace, and starts it only after
every endowment succeeded. `make test-child-spawn` verifies on target that the
child uses an endowed capability, faults on one the parent withheld, does not
start at all when an endowment fails, and appears in the authority page with the
endowed kinds.

Scope and limits. This creates a domain at run time; it does not load code at
run time. The child's ELF comes from the bundle verified at boot, so image
verification is unaffected. The subsetting invariant — no domain holds authority
its parent did not hold — is enforced by seL4 itself, since a parent cannot mint
from a capability it does not possess; the authority page *reports* the
endowment but cannot verify it, because seL4 exposes no capability-enumeration
syscall. A parent can create children only from the pool it was granted;
exhaustion is a resource limit, not an authority boundary. The parent/child pair
exists only in the test image; no default-image PD creates children.
```

- [ ] **Step 8:** Full verification — `make test-host`, `make policy-check`, the aarch64 boot test, `make test-child-spawn`, `make test-cap-lending`, `make test-authority`, `make test-inspect`. Report that `make gate` was not run. Commit.

---

## Notes for the implementer

**The temptation here is to build a spawn service.** Resist it. A resident component that creates domains on request for anyone is the broker design in a new costume — its compromise would let an attacker create a domain with any authority that component can reach. The parent creates its own children, from its own authority, and that is the whole point.

**Do not let the child's code become dynamic.** The ELF comes from the signed bundle. If the design starts needing an ELF from elsewhere, that is a different task with a different threat model, and you should stop and report rather than quietly widening this one.

**Report honestly what the ledger can and cannot show.** It shows what the parent said it granted. seL4 will not tell anyone what a domain actually holds. The invariant holds regardless — but the record is a claim, not a measurement, and `docs/TCB.md` must keep saying so.
