# T2 — Entropy Service Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the `for(;;)` entropy stub with a real driver protection domain that owns a virtio-rng device and serves bounded entropy requests, so that nonces, keys and challenges can be generated on target at all.

**Architecture:** `entropy_pd` is a **driver** PD, not a virtualizer: there is no multiplexing decision to make, because every client receives independent output. It owns one virtio-mmio RNG device and nothing else, following the shape `virtio_blk` and `net_pd` already use. It polls rather than taking an IRQ — entropy has no latency requirement and polling avoids IRQ provisioning entirely.

**Tech Stack:** C11 freestanding (seL4/Microkit), host tests under `-DAGENTOS_TEST_HOST`, `cargo xtask qemu-test` for target proof.

**Spec:** [`docs/superpowers/specs/2026-10-03-trust-delegation-design.md`](../specs/2026-10-03-trust-delegation-design.md) — section "T2 — Entropy service".

## What this task may and may not claim

The proof obligation is deliberately modest and must stay that way.

**May claim:** the service draws from the virtio-rng device it owns; distinct clients receive distinct values; a client cannot obtain entropy without the capability to call the service.

**May NOT claim:** anything about statistical randomness quality, entropy estimation, or cryptographic suitability of the underlying source. A boot test cannot establish those, and QEMU's virtio-rng is backed by the host's RNG, which says nothing about a real board. Any comment or `docs/TCB.md` sentence asserting randomness *quality* is a defect. State what was observed — bytes arrived, and they differed — not what it implies.

## Global Constraints

- Language policy: C, Rust, or Assembly only. `make policy-check` enforces it.
- Host tests under `-DAGENTOS_TEST_HOST` are a compile/logic pre-filter only (`tests/TARGET_TESTS.md`). Only a booted image asserted by an automated test proves target behaviour.
- **Target verification in this environment requires `SEL4_SDK_VERSION=2.1.0`.** The default pin is built from upstream clones absent here. Results under 2.1.0 are genuine boot evidence but not the release-qualified configuration; CI's `os-claim-gate` job runs under the verified SDK artifact.
- One owner per device frame and IRQ (TCB invariant 1). `entropy_pd` owns its virtio-rng MMIO frame and nothing else owns it.
- Do not extend museum components: `cap_broker`, CapStore, `auth_server` are quarantined.
- Do not modify anything under `services/legacy-pds/`.
- Adding a PD changes the booted set. `kernel/agentos-root-task/src/system_desc_aarch64.c` and `kernel/agentos-root-task/agentos.toml` **must agree** — a bundle entry with no descriptor row is never started; a descriptor row with no bundle entry fails at ELF load. `docs/TCB.md` and `README.md` both state the booted PD count and must be updated.

## Review Focus

1. **A stub that looks like a driver.** The service must actually read the device, not return a counter or a fixed buffer. A test asserting "bytes arrived" passes for a `memset`. Assert that two successive reads differ. → Task 2, Task 3.
2. **Device frame ownership collides.** The chosen virtio-mmio bus slot must not be one already owned — bus.2 is `cc_pd`'s virtio-serial, bus.8 is `virtio_blk`, bus.16 is `net_pd`. Picking an occupied slot silently steals another driver's device. → Task 2.
3. **The descriptor and the manifest disagree.** Adding a PD to one and not the other either silently never starts it or fails at ELF load. → Task 2.
4. **Blocking forever on an absent device.** If QEMU is started without the RNG device, the driver must report that and continue, not spin — a driver that hangs takes the boot with it. → Task 2.
5. **Overclaiming randomness.** See the section above. → Task 3.

---

### Task 1: Entropy contract and host-testable request logic

**Files:**
- Create: `kernel/agentos-root-task/include/contracts/entropy_contract.h`
- Create: `services/entropy-service/entropy_proto.c`
- Test: `tests/test_entropy_contract.c`

**Interfaces:**
- Produces: `AOS_ENTROPY_VERSION`; `AOS_ENTROPY_MAX_BYTES` (64); `MSG_ENTROPY_GET`; `aos_entropy_req_t`; `aos_entropy_reply_t`; `int aos_entropy_validate_req(const aos_entropy_req_t *)`; error codes `AOS_ENTROPY_OK`, `AOS_ENTROPY_ERR_VERSION`, `AOS_ENTROPY_ERR_RANGE`, `AOS_ENTROPY_ERR_UNAVAILABLE`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_entropy_contract.c`:

```c
/* Host test: entropy request contract validation. No seL4; logic only. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "contracts/entropy_contract.h"

int main(void)
{
    aos_entropy_req_t r;

    /* A well-formed request is accepted. */
    memset(&r, 0, sizeof(r));
    r.version = AOS_ENTROPY_VERSION;
    r.length  = 32u;
    assert(aos_entropy_validate_req(&r) == AOS_ENTROPY_OK);

    /* Boundary: the maximum is accepted, one past it is not. */
    r.length = AOS_ENTROPY_MAX_BYTES;
    assert(aos_entropy_validate_req(&r) == AOS_ENTROPY_OK);
    r.length = AOS_ENTROPY_MAX_BYTES + 1u;
    assert(aos_entropy_validate_req(&r) == AOS_ENTROPY_ERR_RANGE);

    /* Zero length is a range error, not a silently-empty success. */
    r.length = 0u;
    assert(aos_entropy_validate_req(&r) == AOS_ENTROPY_ERR_RANGE);

    /* A version mismatch is rejected before the length is considered. */
    r.version = AOS_ENTROPY_VERSION + 1u;
    r.length  = 32u;
    assert(aos_entropy_validate_req(&r) == AOS_ENTROPY_ERR_VERSION);

    /* NULL is rejected rather than dereferenced. */
    assert(aos_entropy_validate_req(NULL) == AOS_ENTROPY_ERR_VERSION);

    /* Reserved fields must be zero: a caller setting them is rejected, so the
     * field stays available for a later version without an ABI break. */
    memset(&r, 0, sizeof(r));
    r.version  = AOS_ENTROPY_VERSION;
    r.length   = 16u;
    r.reserved = 1u;
    assert(aos_entropy_validate_req(&r) == AOS_ENTROPY_ERR_RANGE);

    printf("test_entropy_contract: PASS\n");
    return 0;
}
```

- [ ] **Step 2: Run it to make sure it fails**

Run: `clang -std=gnu11 -DAGENTOS_TEST_HOST -I kernel/agentos-root-task/include -o /tmp/t_ent tests/test_entropy_contract.c services/entropy-service/entropy_proto.c && /tmp/t_ent`
Expected: FAIL — `'contracts/entropy_contract.h' file not found`

- [ ] **Step 3: Create the contract header**

Create `kernel/agentos-root-task/include/contracts/entropy_contract.h`:

```c
/*
 * Entropy service IPC contract
 *
 * entropy_pd is a DRIVER protection domain: it owns one virtio-rng device and
 * nothing else. It is not a virtualizer, because there is no multiplexing
 * decision to make -- every client receives independent output, and no client's
 * request affects another's.
 *
 * SCOPE OF WHAT THIS PROVIDES. The service returns bytes produced by the
 * virtio-rng device it owns. It makes NO claim about statistical quality,
 * entropy estimation, or cryptographic suitability of that source: under QEMU
 * the backing source is the host's RNG, which establishes nothing about a real
 * board. Callers needing a qualified source must state that requirement
 * separately; this contract only promises that bytes came from the device.
 *
 * Opcode: MSG_ENTROPY_GET. Request and reply are the packed structs below;
 * the reply payload travels in the message registers.
 */

#pragma once
#include <stdint.h>

#define AOS_ENTROPY_VERSION    1u
#define AOS_ENTROPY_MAX_BYTES  64u

#ifndef MSG_ENTROPY_GET
#define MSG_ENTROPY_GET        0x2700u
#endif

#define AOS_ENTROPY_OK             0
#define AOS_ENTROPY_ERR_VERSION    1
#define AOS_ENTROPY_ERR_RANGE      2
#define AOS_ENTROPY_ERR_UNAVAILABLE 3

typedef struct __attribute__((packed)) aos_entropy_req {
    uint32_t version;
    uint32_t length;    /* 1..AOS_ENTROPY_MAX_BYTES */
    uint32_t reserved;  /* must be zero */
} aos_entropy_req_t;

typedef struct __attribute__((packed)) aos_entropy_reply {
    uint32_t status;
    uint32_t length;    /* bytes valid in data[] */
    uint8_t  data[AOS_ENTROPY_MAX_BYTES];
} aos_entropy_reply_t;

int aos_entropy_validate_req(const aos_entropy_req_t *req);
```

- [ ] **Step 4: Implement the validator**

Create `services/entropy-service/entropy_proto.c` implementing `aos_entropy_validate_req` to satisfy the test exactly: NULL and version mismatch return `AOS_ENTROPY_ERR_VERSION`; a nonzero `reserved`, a zero `length`, or a `length` above `AOS_ENTROPY_MAX_BYTES` return `AOS_ENTROPY_ERR_RANGE`; otherwise `AOS_ENTROPY_OK`. Check version before length, as the test requires.

- [ ] **Step 5: Run the test and make sure it passes**

Expected: PASS — `test_entropy_contract: PASS`

- [ ] **Step 6: Wire into the host suite**

Add a `test-entropy-host` target to `Makefile`, modelled on `test-remoteos-client-host` (around `Makefile:921-928`), and a `test-host: test-entropy-host` line.

- [ ] **Step 7: Run the host suite**

Run: `make test-host` — expected PASS including `test_entropy_contract`.

- [ ] **Step 8: Commit**

```bash
git add kernel/agentos-root-task/include/contracts/entropy_contract.h \
        services/entropy-service/entropy_proto.c tests/test_entropy_contract.c Makefile
git commit -m "contracts: add the entropy service request contract

A driver contract, not a virtualizer one: every client receives independent
output. States explicitly that it promises bytes from the device and makes no
claim about randomness quality. Contract only; no driver yet."
```

---

### Task 2: The driver protection domain

**Files:**
- Rewrite: `services/entropy-service/entropy_svc.c` (currently a `for(;;)` stub)
- Create: `platform/include/platform/entropy_host_layout.h`
- Modify: `kernel/agentos-root-task/src/system_desc_aarch64.c` (new PD row)
- Modify: `kernel/agentos-root-task/agentos.toml` (matching bundle entry)
- Modify: `kernel/agentos-root-task/Makefile` (build `entropy_pd.elf`)
- Modify: `xtask/src/cmd_test.rs` (add the QEMU device)

**Interfaces:**
- Consumes: the Task 1 contract.
- Produces: a booted `entropy_pd` serving `MSG_ENTROPY_GET`.

- [ ] **Step 1: Pick the bus slot and write the layout header**

QEMU `virt` places virtio-mmio devices at `0x0a000000 + slot * 0x200`, with IRQ `48 + slot`. Occupied slots: **2** (`cc_pd` virtio-serial, `0x0a000400`, IRQ 50), **8** (`virtio_blk`, `0x0a001000`), **16** (`net_pd`, `0x0a002000`, IRQ 64). Verify these against `xtask/src/cmd_test.rs` before proceeding — if any has moved, re-derive rather than trusting this list.

Use slot **4**: `0x0a000800`. Create `platform/include/platform/entropy_host_layout.h` defining `AGENTOS_HOST_ENTROPY_MMIO_PA 0x0A000800UL` and a VA, following the shape of `platform/include/platform/net_host_layout.h`. Document that only `entropy_pd` receives this mapping.

> **Superseded (R19/R20).** Slot 4 collides with the root task's own virtio-mmio probe frame and fails retype; the whole bus is in fact fully subscribed (see `docs/TCB.md`). A RAM-backed substitute frame was tried next (R19) and dropped (R20) because it isn't an honest device frame. The shipped design provisions no MMIO frame for entropy_pd at all on QEMU virt — this section is the historical record of what was tried, not the current design.

- [ ] **Step 2: Write the driver**

Replace `services/entropy-service/entropy_svc.c` entirely. It must:
- initialise the virtio-mmio RNG device through `virtio_host_transport` — read how `services/block-driver/virtio_blk.c` uses that shared transport and follow it rather than writing raw MMIO;
- **poll** rather than take an IRQ, so no IRQ provisioning is needed;
- serve `MSG_ENTROPY_GET` by validating with `aos_entropy_validate_req`, filling `length` bytes, and replying;
- when the device is absent or never becomes ready, log once and reply `AOS_ENTROPY_ERR_UNAVAILABLE` to every request. **It must not spin or block forever** — a driver that hangs takes the boot with it.

- [ ] **Step 3: Add the descriptor row**

Add `entropy_pd` to `system_desc_aarch64.c` following the `serial_pd` row's shape (`system_desc_aarch64.c:158-178`): `init_eps` for nameserver and log_drain, `irq_count = 0u`, and `device_frame_count = 1u` with `.paddr = AGENTOS_HOST_ENTROPY_MMIO_PA`, `.size_bits = 12u`, a free `cnode_slot`, and `.name = "virtio-rng-mmio"`. Choose a priority below the drivers that carry latency requirements; entropy has none.

> **Superseded (R19/R20).** The shipped descriptor row declares no `device_frame_count` at all; `AGENTOS_HOST_ENTROPY_MMIO_PA` does not exist in the final design. See the note after Step 1 and `docs/TCB.md`.

- [ ] **Step 4: Add the matching manifest entry**

Add the same name to `kernel/agentos-root-task/agentos.toml`. The descriptor and the manifest must agree — a bundle entry with no descriptor row never starts, and a descriptor row with no bundle entry fails at ELF load.

- [ ] **Step 5: Add the QEMU device**

In `xtask/src/cmd_test.rs`, add `-device virtio-rng-device,bus=virtio-mmio-bus.4` beside the existing `virtio-serial-device` / `virtio-net-device` arguments. Confirm `qemu-system-aarch64 -device help` lists `virtio-rng-device` before relying on it.

- [ ] **Step 6: Build and boot**

Run: `make test-host && make test TARGET_ARCH=aarch64 GUEST_OS=none SEL4_SDK_VERSION=2.1.0`
Expected: both PASS; boot reaches `agentOS boot complete`.

If boot hangs, the driver is spinning on a device that never became ready — fix the driver, do not extend the timeout.

- [ ] **Step 7: Update the booted-PD count**

`docs/TCB.md` and `README.md` both state how many PDs the default image boots, and `docs/TCB.md` lists them. Update both. A stale count in the binding document is a defect.

- [ ] **Step 8: Commit**

```bash
git add services/entropy-service/entropy_svc.c \
        platform/include/platform/entropy_host_layout.h \
        kernel/agentos-root-task/src/system_desc_aarch64.c \
        kernel/agentos-root-task/agentos.toml \
        kernel/agentos-root-task/Makefile xtask/src/cmd_test.rs \
        docs/TCB.md README.md
git commit -m "services: entropy_pd drives a virtio-rng device

Replaces the for(;;) stub. A driver PD owning one virtio-mmio RNG frame on
slot 4 and nothing else, polling rather than taking an IRQ. Reports
AOS_ENTROPY_ERR_UNAVAILABLE rather than spinning when the device is absent."
```

---

### Task 3: Target proof

**Files:**
- Modify: `xtask/src/lib.rs` (probe argument), `xtask/src/cmd_test.rs` (oracle)
- Modify: `Makefile` (`test-entropy` target, add to `gate`)
- Modify: `.github/workflows/ci.yml` (gate step)
- Modify: `docs/TCB.md`

- [ ] **Step 1: Add a native test client**

The proof needs a client that calls the service on target. Follow the existing native-client pattern — read how `tests/platform/` test PDs are built and added to a variant image, and mirror it. The client requests entropy twice and reports both results.

- [ ] **Step 2: The oracle**

Assert: the first request succeeds with the requested length; the second succeeds; and **the two results differ**. The difference assertion is the one that matters — a driver returning a fixed buffer or a `memset` would satisfy "bytes arrived" and fail this.

Also assert a request with `length` above `AOS_ENTROPY_MAX_BYTES` returns `AOS_ENTROPY_ERR_RANGE`, so the validator is exercised on target rather than only on the host.

Do **not** assert anything about the distribution, entropy content, or quality of the bytes. Two samples cannot establish that and the claim would be false.

- [ ] **Step 3: Add the Makefile target and CI step**

```make
test-entropy:
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os none --timeout-secs $(QEMU_TEST_TIMEOUT) --entropy-probe 1
```

Add `test-entropy` to `gate`, **and** add a `GATE — make test-entropy` step to the `os-claim-gate` job in `.github/workflows/ci.yml` beside the existing `test-cc-envelope` and `test-authority` steps. No CI job invokes `make gate`, so a proof reachable only through that target never runs.

- [ ] **Step 4: Prove the oracle is not vacuous**

Temporarily make the driver return a constant buffer instead of device bytes, rebuild, and confirm the probe **FAILS** on the difference assertion. Restore and confirm it passes. Paste all three outputs in your report.

- [ ] **Step 5: Update `docs/TCB.md`**

State what was proven and its limits:

```markdown
`entropy_pd` owns one virtio-mmio RNG frame (slot 4) and no IRQ; it polls the
device and serves bounded `MSG_ENTROPY_GET` requests. `make test-entropy`
verifies on target that two successive requests both succeed and return
different bytes, and that an over-length request is refused. This establishes
that the service draws from the device it owns rather than returning a constant;
it establishes NOTHING about statistical quality, entropy estimation, or
cryptographic suitability of the source, which under QEMU is the host's RNG and
says nothing about a real board. A caller needing a qualified source must state
that requirement separately.
```

- [ ] **Step 6: Full verification**

`make test-host`, `make policy-check`, `make test TARGET_ARCH=aarch64 GUEST_OS=none SEL4_SDK_VERSION=2.1.0`, `make test-entropy SEL4_SDK_VERSION=2.1.0`, `make test-inspect SEL4_SDK_VERSION=2.1.0`, `make test-authority SEL4_SDK_VERSION=2.1.0`.

`test-authority` matters: adding a PD changes the authority page's row count, so its oracle's `pd_count` assertion will need updating. That is expected — update it to the new count and say so.

- [ ] **Step 7: Commit**

```bash
git add xtask/src/lib.rs xtask/src/cmd_test.rs Makefile \
        .github/workflows/ci.yml docs/TCB.md
git commit -m "test: target proof for the entropy service

Two successive requests return different bytes, demonstrated non-vacuous by
temporarily returning a constant. Claims device provenance only, not quality."
```

---

## Notes for the implementer

**Adding a PD has blast radius.** The authority page from T4 counts protection domains, and its oracle asserts an exact `pd_count`. Adding `entropy_pd` changes that number. Update the assertion; do not weaken it to a range.

**The honesty constraint here is about randomness.** It is tempting to describe an RNG driver as providing "secure" or "cryptographic" randomness. It provides bytes from a device. Under QEMU that device is backed by the host, which tells you nothing about the board this will eventually run on. Say what was observed.
