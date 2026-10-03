# T4 — Read-only Authority Observer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Publish the authority relation the root task established at boot — which protection domain holds how much of each kind of capability — through a read-only page that no client can write, so the system's intended authority is inspectable instead of invisible.

**Architecture:** The root task already records every capability it hands out in `cap_accounting.c` (`cap_acct_entry_t { cap, obj_type, pd_index, name }`, up to 1024 entries), and already publishes an immutable boot observation as a read-only page via `aos_inspect_snapshot_t`. This adds a second, parallel read-only page carrying a per-domain × per-kind count matrix, built from the accounting table, mapped with the same read-only rights into the same readers.

**Tech Stack:** C11 freestanding (seL4/Microkit), host tests under `-DAGENTOS_TEST_HOST`, `cargo xtask qemu-test --<name>-probe N` for target proof. No new dependencies.

**Spec:** [`docs/superpowers/specs/2026-10-03-trust-delegation-design.md`](../specs/2026-10-03-trust-delegation-design.md) — section "T4 — Read-only authority observer".

## What this is NOT — read before writing any comment or document

**seL4 provides no capability-enumeration syscall.** `seL4_DebugCapIdentify` is the only introspection primitive, it is `CONFIG_DEBUG_BUILD`-only, and `kernel/gen_config.h` shows that disabled in the release kernel agentOS ships. There is no CDT walk. A domain cannot enumerate even its own CSpace. Obtaining one would mean modifying seL4, which `CLAUDE.md` forbids absolutely.

So this is a **ledger of what root granted**, not a reader of kernel state. Three consequences bind every comment, contract header, commit message and `docs/TCB.md` sentence produced by this plan:

1. It reports the **intended** authority relation as root established it. It cannot detect a divergence between that record and actual kernel state.
2. It covers the **boot-time static set only**. Runtime delegation (T5, T6) does not exist yet and, when it does, will have to be *reported* by the delegating domain — the kernel will not reveal it.
3. **It does not verify the subsetting invariant.** That invariant is enforced by the kernel unconditionally, because a domain cannot mint from a capability it does not hold. This page supplies visibility, not verification.

Any text claiming this "enumerates", "verifies", "audits the capability tree", or "proves" anything about live kernel state is false and is a defect.

## Global Constraints

- Language policy: C, Rust, or Assembly only, including tests and tooling. `make policy-check` enforces it.
- Host tests under `-DAGENTOS_TEST_HOST` are a compile/logic pre-filter only (`tests/TARGET_TESTS.md`). Only a booted image asserted by an automated test proves target behavior.
- **Target verification in this environment requires `SEL4_SDK_VERSION=2.1.0`.** The default pin (`2.3.1-agentos-e60776ac-cr2`) is built locally from upstream clones absent here. Results under 2.1.0 are genuine boot evidence but are **not** the release-qualified configuration; any `docs/TCB.md` claim must name the SDK that produced it.
- The new ABI header must be **host-testable: no seL4 headers**, matching `platform/include/platform/inspect.h`. seL4 object-type constants are mapped to an agentOS-local enum inside the root task, which has those headers.
- Do not extend museum components. `cap_broker`, CapStore, `auth_server` are quarantined: "Do not extend these. Do not add opcodes. Do not 'finish' them."
- Do not modify anything under `services/legacy-pds/`.
- The page must be **read-only to every consumer**. Root retains the writable capability; consumers receive a copy minted with read rights only.
- Any component that lands updates `docs/TCB.md` in the same change with its qualification boundary stated.

## Review Focus

Five failure modes no single task's happy path exercises.

1. **The page is writable.** The whole security property is that a consumer cannot alter the record. If the rights copy is wrong, the page is a mutable shared buffer and the feature is worse than useless. → Task 3 target fault probe.
2. **Counts silently truncate.** `cap_acct` holds up to 1024 entries and the page is 4 KiB. If a domain's counts saturate a `uint16_t`, or a domain index exceeds the table, a reader sees a plausible-but-wrong number with no indication. Saturation must be explicit. → Task 1 and Task 2.
3. **Boot regresses under memory pressure.** Root allocates one more frame and maps it into every reader. Allocation or mapping failure must refuse startup the way the inspect page does, never boot partially with a stale or absent page. → Task 2.
4. **The matrix disagrees with the descriptor.** A count that does not match `system_desc_aarch64.c` means either the mapping from seL4 object types is wrong or the accounting table is incomplete. This is the content check that makes the page worth publishing. → Task 3.
5. **An unmapped seL4 object type is silently dropped.** Object types not in the agentOS enum must land in an explicit `OTHER` bucket, not vanish, or the totals will not reconcile. → Task 1 and Task 2.

---

### Task 1: Authority ABI and host-side builder

**Files:**
- Create: `platform/include/platform/authority.h`
- Create: `platform/inspect/authority.c`
- Test: `tests/test_authority_snapshot.c`

**Interfaces:**
- Consumes: nothing from earlier tasks.
- Produces: `AOS_AUTHORITY_VERSION`; `AOS_AUTHORITY_MAX_PDS` (32); `AOS_AUTHORITY_KIND_COUNT` (11); `aos_authority_kind_t`; `aos_authority_pd_t`; `aos_authority_snapshot_t`; `AOS_AUTHORITY_BOOT_VA`; `void aos_authority_init(aos_authority_snapshot_t *)`; `int aos_authority_add(aos_authority_snapshot_t *, uint32_t pd_index, const char *name, uint32_t kind)`; `int aos_authority_validate(const aos_authority_snapshot_t *)`; `int aos_authority_format(const aos_authority_snapshot_t *, char *buf, size_t buflen)`.

- [ ] **Step 1: Write the failing test**

Create `tests/test_authority_snapshot.c`:

```c
/* Host test: authority snapshot ABI and builder. No seL4; logic only. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "platform/authority.h"

int main(void)
{
    aos_authority_snapshot_t s;
    aos_authority_init(&s);
    assert(s.version == AOS_AUTHORITY_VERSION);
    assert(s.pd_count == 0u);
    assert(s.truncated_pds == 0u);
    assert(aos_authority_validate(&s) == AOS_AUTHORITY_OK);

    /* First add creates the domain row. */
    assert(aos_authority_add(&s, 3u, "serial_pd", AOS_AUTHORITY_KIND_FRAME) == AOS_AUTHORITY_OK);
    assert(s.pd_count == 1u);
    assert(s.pds[0].pd_index == 3u);
    assert(strcmp((const char *)s.pds[0].name, "serial_pd") == 0);
    assert(s.pds[0].counts[AOS_AUTHORITY_KIND_FRAME] == 1u);

    /* Same domain, same kind: increments, no new row. */
    assert(aos_authority_add(&s, 3u, "serial_pd", AOS_AUTHORITY_KIND_FRAME) == AOS_AUTHORITY_OK);
    assert(s.pd_count == 1u);
    assert(s.pds[0].counts[AOS_AUTHORITY_KIND_FRAME] == 2u);

    /* Same domain, different kind. */
    assert(aos_authority_add(&s, 3u, "serial_pd", AOS_AUTHORITY_KIND_IRQ_HANDLER) == AOS_AUTHORITY_OK);
    assert(s.pds[0].counts[AOS_AUTHORITY_KIND_IRQ_HANDLER] == 1u);
    assert(s.pd_count == 1u);

    /* A second domain gets its own row. */
    assert(aos_authority_add(&s, 7u, "net_virt", AOS_AUTHORITY_KIND_ENDPOINT) == AOS_AUTHORITY_OK);
    assert(s.pd_count == 2u);
    assert(s.pds[1].pd_index == 7u);

    /* An unknown kind is bucketed as OTHER, never dropped. */
    assert(aos_authority_add(&s, 7u, "net_virt", 0xBEEFu) == AOS_AUTHORITY_OK);
    assert(s.pds[1].counts[AOS_AUTHORITY_KIND_OTHER] == 1u);

    /* Totals reconcile: every add is counted exactly once somewhere. */
    uint32_t sum = 0u;
    for (uint32_t i = 0u; i < s.pd_count; i++)
        for (uint32_t k = 0u; k < AOS_AUTHORITY_KIND_COUNT; k++)
            sum += s.pds[i].counts[k];
    assert(sum == 5u);
    assert(s.total_recorded == 5u);

    /* Overflowing the domain table is reported, not silently dropped. */
    {
        aos_authority_snapshot_t o;
        aos_authority_init(&o);
        for (uint32_t i = 0u; i < AOS_AUTHORITY_MAX_PDS + 4u; i++) {
            char nm[8];
            nm[0] = 'p'; nm[1] = (char)('0' + (int)(i % 10u)); nm[2] = '\0';
            (void)aos_authority_add(&o, i, nm, AOS_AUTHORITY_KIND_TCB);
        }
        assert(o.pd_count == AOS_AUTHORITY_MAX_PDS);
        assert(o.truncated_pds == 4u);
        assert(aos_authority_validate(&o) == AOS_AUTHORITY_OK);
    }

    /* A count that saturates uint16 is flagged, not wrapped. */
    {
        aos_authority_snapshot_t t;
        aos_authority_init(&t);
        for (uint32_t i = 0u; i < 70000u; i++)
            (void)aos_authority_add(&t, 1u, "busy", AOS_AUTHORITY_KIND_FRAME);
        assert(t.pds[0].counts[AOS_AUTHORITY_KIND_FRAME] == 0xFFFFu);
        assert(t.saturated != 0u);
    }

    /* validate rejects a bad version and an impossible pd_count. */
    {
        aos_authority_snapshot_t b = s;
        b.version = AOS_AUTHORITY_VERSION + 1u;
        assert(aos_authority_validate(&b) == AOS_AUTHORITY_ERR_VERSION);
        aos_authority_snapshot_t c = s;
        c.pd_count = AOS_AUTHORITY_MAX_PDS + 1u;
        assert(aos_authority_validate(&c) == AOS_AUTHORITY_ERR_INVALID);
    }

    /* format writes key=value lines and never overruns. */
    {
        char buf[2048];
        int n = aos_authority_format(&s, buf, sizeof(buf));
        assert(n > 0);
        assert(strstr(buf, "serial_pd") != NULL);
        assert(strstr(buf, "frame=2") != NULL);
        char tiny[8];
        assert(aos_authority_format(&s, tiny, sizeof(tiny)) == AOS_AUTHORITY_ERR_TRUNC);
    }

    printf("test_authority_snapshot: PASS\n");
    return 0;
}
```

- [ ] **Step 2: Run it to make sure it fails**

Run: `clang -std=gnu11 -DAGENTOS_TEST_HOST -I platform/include -o /tmp/t_auth tests/test_authority_snapshot.c platform/inspect/authority.c && /tmp/t_auth`
Expected: FAIL — `'platform/authority.h' file not found`

- [ ] **Step 3: Create the ABI header**

Create `platform/include/platform/authority.h`:

```c
/*
 * agentOS authority snapshot ABI
 *
 * Read-only record of the authority the root task established at boot: for
 * each protection domain, how many capabilities of each kind it was granted.
 * Host-testable: no seL4 headers. Root publishes this as an immutable boot
 * observation mapped read-only into its readers.
 *
 * WHAT THIS IS NOT. seL4 exposes no capability-enumeration syscall --
 * seL4_DebugCapIdentify is CONFIG_DEBUG_BUILD-only and that is disabled in the
 * shipped release kernel -- so this is a LEDGER OF WHAT ROOT GRANTED, not a
 * reader of kernel state. It reports the intended relation and cannot detect a
 * divergence between that record and the kernel. It covers the boot-time static
 * set; runtime delegation must be separately reported by the delegating domain.
 * It does NOT verify the subsetting invariant: that is enforced by the kernel
 * unconditionally, since a domain cannot mint from a capability it does not
 * hold. This supplies visibility, not verification.
 */

#ifndef AOS_PLATFORM_AUTHORITY_H
#define AOS_PLATFORM_AUTHORITY_H

#include <stddef.h>
#include <stdint.h>

#define AOS_AUTHORITY_VERSION     1u
#define AOS_AUTHORITY_MAX_PDS     32u
#define AOS_AUTHORITY_NAME_LEN    32u

/* One 4 KiB page. 0x10009000 is the inspect boot page and 0x1000a000 /
 * 0x1000b000 are the log config and client pages (platform/log_ring.h), so
 * this sits above them in the first otherwise-unused slot. */
#define AOS_AUTHORITY_BOOT_VA     0x1000C000UL

/* agentOS-local capability kinds. seL4 object-type constants are mapped onto
 * these inside the root task, which has the seL4 headers; this header stays
 * host-testable. Order is ABI -- append only, never reorder. */
typedef enum {
    AOS_AUTHORITY_KIND_UNTYPED       = 0,
    AOS_AUTHORITY_KIND_TCB           = 1,
    AOS_AUTHORITY_KIND_ENDPOINT      = 2,
    AOS_AUTHORITY_KIND_NOTIFICATION  = 3,
    AOS_AUTHORITY_KIND_CNODE         = 4,
    AOS_AUTHORITY_KIND_FRAME         = 5,
    AOS_AUTHORITY_KIND_VSPACE        = 6,
    AOS_AUTHORITY_KIND_IRQ_HANDLER   = 7,
    AOS_AUTHORITY_KIND_SCHED_CONTEXT = 8,
    AOS_AUTHORITY_KIND_REPLY         = 9,
    AOS_AUTHORITY_KIND_OTHER         = 10,
} aos_authority_kind_t;

#define AOS_AUTHORITY_KIND_COUNT 11u

#define AOS_AUTHORITY_OK            0
#define AOS_AUTHORITY_ERR_NULL     (-1)
#define AOS_AUTHORITY_ERR_VERSION  (-2)
#define AOS_AUTHORITY_ERR_TRUNC    (-3)
#define AOS_AUTHORITY_ERR_INVALID  (-6)

typedef struct __attribute__((packed)) aos_authority_pd {
    uint32_t pd_index;
    uint8_t  name[AOS_AUTHORITY_NAME_LEN];
    uint16_t counts[AOS_AUTHORITY_KIND_COUNT];
    uint16_t reserved;
} aos_authority_pd_t;

typedef struct __attribute__((packed)) aos_authority_snapshot {
    uint32_t version;
    uint32_t pd_count;        /* rows in use, <= AOS_AUTHORITY_MAX_PDS */
    uint32_t total_recorded;  /* every add, including saturated increments */
    uint32_t truncated_pds;   /* domains that did not fit a row */
    uint32_t saturated;       /* nonzero if any count hit UINT16_MAX */
    uint32_t reserved;
    aos_authority_pd_t pds[AOS_AUTHORITY_MAX_PDS];
} aos_authority_snapshot_t;

void aos_authority_init(aos_authority_snapshot_t *snap);
int  aos_authority_add(aos_authority_snapshot_t *snap, uint32_t pd_index,
                       const char *name, uint32_t kind);
int  aos_authority_validate(const aos_authority_snapshot_t *snap);
int  aos_authority_format(const aos_authority_snapshot_t *snap,
                          char *buf, size_t buflen);

#endif /* AOS_PLATFORM_AUTHORITY_H */
```

- [ ] **Step 4: Implement the builder**

Create `platform/inspect/authority.c`. Implement the four functions to satisfy the test exactly:

- `aos_authority_init` zeroes the struct and sets `version`.
- `aos_authority_add` finds the row for `pd_index` or appends one (copying `name` NUL-padded, truncated to `AOS_AUTHORITY_NAME_LEN - 1`); when no row is free it increments `truncated_pds` and returns `AOS_AUTHORITY_OK` — a full table is a reported condition, not a failure. A `kind` outside the enum lands in `AOS_AUTHORITY_KIND_OTHER`. Every call increments `total_recorded`. A count already at `0xFFFF` stays there and sets `saturated`.
- `aos_authority_validate` returns `AOS_AUTHORITY_ERR_NULL` for NULL, `..._ERR_VERSION` on version mismatch, `..._ERR_INVALID` when `pd_count > AOS_AUTHORITY_MAX_PDS`, else OK.
- `aos_authority_format` emits one `pd=<name> index=<n> untyped=<n> tcb=<n> ...` line per domain plus a trailing summary line carrying `total`, `truncated_pds` and `saturated`, returning `AOS_AUTHORITY_ERR_TRUNC` if it would exceed `buflen`. Follow the style of `aos_inspect_format` in `platform/inspect/` — read it first and match it.

Add a static assertion beside the struct definition in the header or at the top of `authority.c`:

```c
_Static_assert(sizeof(aos_authority_snapshot_t) <= 4096,
               "authority snapshot fits one page");
```

- [ ] **Step 5: Run the test and make sure it passes**

Run: `clang -std=gnu11 -DAGENTOS_TEST_HOST -I platform/include -o /tmp/t_auth tests/test_authority_snapshot.c platform/inspect/authority.c && /tmp/t_auth`
Expected: PASS — `test_authority_snapshot: PASS`

- [ ] **Step 6: Wire into the host suite**

Add a `test-authority-host` target to `Makefile`, modelled exactly on `test-remoteos-client-host` (around `Makefile:921-928`), and add `test-host: test-authority-host` beside the others.

- [ ] **Step 7: Run the host suite**

Run: `make test-host`
Expected: PASS including `test_authority_snapshot`.

- [ ] **Step 8: Commit**

```bash
git add platform/include/platform/authority.h platform/inspect/authority.c \
        tests/test_authority_snapshot.c Makefile
git commit -m "platform: add the authority snapshot ABI and builder

Per-domain counts of each capability kind the root task granted. A ledger of
what root recorded, not a reader of kernel state -- seL4 exposes no capability
enumeration. Truncation and uint16 saturation are reported explicitly rather
than silently losing counts. Contract only; nothing publishes it yet."
```

---

### Task 2: Root fills and publishes the page

**Files:**
- Modify: `kernel/agentos-root-task/src/main.c` (beside the inspect publication, around `:4195-4235`)
- Modify: `kernel/agentos-root-task/Makefile` (link `authority.o` into the root task if needed)
- Test: `tests/test_authority_kindmap.c`

**Interfaces:**
- Consumes: everything Task 1 produces; `cap_acct_count()`, `cap_acct_get()`, `cap_acct_entry_t` from `kernel/agentos-root-task/include/cap_accounting.h`.
- Produces: `uint32_t aos_authority_kind_from_sel4(uint32_t obj_type)` — declared in a root-task header, defined in `main.c` or a small new file, mapping seL4 object-type constants onto `aos_authority_kind_t`.

- [ ] **Step 1: Write the failing test for the kind mapping**

Create `tests/test_authority_kindmap.c`. The mapping is the piece most likely to be silently wrong, so it gets its own host test. Because the seL4 constants differ by architecture, the test asserts the *mapping function's* behavior against locally-defined expected values rather than importing seL4 headers:

```c
/* Host test: seL4 object type -> agentOS authority kind mapping. */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include "platform/authority.h"

uint32_t aos_authority_kind_from_sel4(uint32_t obj_type);

/* Mirrors of the seL4 constants the root task maps. The implementation must
 * use the real seL4 enum; these values are supplied by the test harness via
 * -D so the two cannot drift silently. */
int main(void)
{
    assert(aos_authority_kind_from_sel4(AOSTEST_SEL4_UNTYPED)   == AOS_AUTHORITY_KIND_UNTYPED);
    assert(aos_authority_kind_from_sel4(AOSTEST_SEL4_TCB)       == AOS_AUTHORITY_KIND_TCB);
    assert(aos_authority_kind_from_sel4(AOSTEST_SEL4_ENDPOINT)  == AOS_AUTHORITY_KIND_ENDPOINT);
    assert(aos_authority_kind_from_sel4(AOSTEST_SEL4_NOTIFICATION) == AOS_AUTHORITY_KIND_NOTIFICATION);
    assert(aos_authority_kind_from_sel4(AOSTEST_SEL4_CNODE)     == AOS_AUTHORITY_KIND_CNODE);

    /* Anything unmapped must bucket as OTHER, never vanish. */
    assert(aos_authority_kind_from_sel4(0xFFFFu) == AOS_AUTHORITY_KIND_OTHER);

    printf("test_authority_kindmap: PASS\n");
    return 0;
}
```

Read the real seL4 object-type enum in the SDK (`$SEL4_SDK/board/qemu_virt_aarch64/release/include/sel4/...`, the generated `seL4_ObjectType`) and pass the five constants above as `-D` flags from the Makefile target so the test and the implementation cannot drift. If a cleaner way to pin them without importing seL4 headers into a host test exists in this repo, use that instead and say so in your report.

- [ ] **Step 2: Run it to make sure it fails**

Expected: FAIL — undefined reference to `aos_authority_kind_from_sel4`.

- [ ] **Step 3: Implement the mapping**

Implement `aos_authority_kind_from_sel4` in the root task, mapping each `seL4_ObjectType` the accounting table can hold onto the matching `AOS_AUTHORITY_KIND_*`, with `default:` returning `AOS_AUTHORITY_KIND_OTHER`. Include the architecture-specific frame and vspace types for both AArch64 and x86_64 under the appropriate `#if defined(__aarch64__)` / `#elif defined(__x86_64__)` guards, following how `main.c` already handles arch differences.

- [ ] **Step 4: Run the test and make sure it passes**

Expected: PASS.

- [ ] **Step 5: Fill and publish the page**

Read `kernel/agentos-root-task/src/main.c` around lines 4195-4235 first — that is the inspect publication, and this must mirror it exactly. It allocates a frame, makes a read-only copy with `seL4_CNode_Copy(..., seL4_CapRights_new(0, 0, 1, 0))`, and maps that copy into each reader with `pd_vspace_map_device_frame(readers[i], reader, AOS_INSPECT_BOOT_VA)`.

Add the parallel publication: build an `aos_authority_snapshot_t` in scratch by walking `cap_acct_count()` / `cap_acct_get()`, calling `aos_authority_add(&snap, e->pd_index, e->name, aos_authority_kind_from_sel4(e->obj_type))` for every entry; then allocate a frame, copy it **read-only** exactly as above, and map it at `AOS_AUTHORITY_BOOT_VA` into the same readers.

Add beside the existing one:

```c
_Static_assert(sizeof(aos_authority_snapshot_t) <= 4096, "authority fits one page");
```

**Failure handling must match the inspect page: refuse startup.** On allocation or mapping failure, print a `[rt]` diagnostic and return without starting, exactly as the inspect path does with `"[rt] inspect reader mapping failed; refusing partial boot"`. Never boot with the page absent or stale — a consumer cannot distinguish a missing page from an empty one.

- [ ] **Step 6: Build and boot**

Run: `make test-host && make test TARGET_ARCH=aarch64 GUEST_OS=none SEL4_SDK_VERSION=2.1.0`
Expected: both PASS; boot reaches `agentOS boot complete`.

If boot now fails on memory, the extra frame is the cause — report it rather than shrinking the page or skipping the mapping.

- [ ] **Step 7: Commit**

```bash
git add kernel/agentos-root-task/src/main.c kernel/agentos-root-task/Makefile \
        tests/test_authority_kindmap.c
git commit -m "root: publish the boot authority relation as a read-only page

Built from the existing capability accounting table and mapped with read-only
rights into the same readers as the inspect page. Allocation or mapping failure
refuses startup rather than booting with an absent page."
```

---

### Task 3: agentctl reader and target proof

**Files:**
- Modify: `tools/agentctl/agentctl.c` (new `authority` subcommand)
- Modify: `xtask/src/lib.rs` (probe argument), `xtask/src/cmd_test.rs` (oracle)
- Modify: `Makefile` (`test-authority` target, add to `gate`)
- Modify: `docs/TCB.md`

**Interfaces:**
- Consumes: everything from Tasks 1 and 2.
- Produces: `agentctl authority`; `make test-authority`; xtask flag `--authority-probe N` for N in 1..=2.

- [ ] **Step 1: Add the agentctl subcommand**

Mirror however `agentctl inspect` reads the inspect page — read that command first and follow it exactly, including how it obtains the snapshot and emits structured output. Emit the authority snapshot through `aos_authority_format`.

- [ ] **Step 2: Probe 1 — content matches the descriptor**

The oracle asserts specific, descriptor-derived counts rather than merely that the page parses. From `kernel/agentos-root-task/src/system_desc_aarch64.c`: `serial_pd` has `irq_count = 1`, and `cc_pd` has `irq_count = 1`. Assert that each of those domains reports exactly 1 under `AOS_AUTHORITY_KIND_IRQ_HANDLER`, and that `net_virt` and `blk_virt` — which own no device frame and no IRQ — report **0** IRQ handlers.

That last assertion is the valuable one: it is the published page agreeing with TCB invariant 2.

Also assert `version == AOS_AUTHORITY_VERSION`, `pd_count > 0`, `truncated_pds == 0` and `saturated == 0` for the default image. A truncated or saturated default image means the sizing is wrong and must be reported, not accepted.

- [ ] **Step 3: Probe 2 — the page is read-only on target**

A write to `AOS_AUTHORITY_BOOT_VA` from a consumer must fault. Model this on the existing inspect write probe: `main.c:134-140` defines `ROOT_PROBE_ADDRESS AOS_INSPECT_BOOT_VA` with `ROOT_PROBE_WRITE 1` under a build guard, and root emits a success marker only after matching the exact fault badge, address and direction. Add the parallel guard for `AOS_AUTHORITY_BOOT_VA` with its own marker.

Only root may emit the success marker, and only on an exact match — a timeout, an unrelated fault, or a normal boot must not satisfy the oracle.

- [ ] **Step 4: Add the Makefile target**

```make
test-authority:
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os none --timeout-secs $(QEMU_TEST_TIMEOUT) --authority-probe 1
	@cargo xtask qemu-test --board qemu_virt_aarch64 --guest-os none --timeout-secs $(QEMU_TEST_TIMEOUT) --authority-probe 2
```

Add `test-authority` to the `gate` prerequisites.

- [ ] **Step 5: Run both probes**

Run: `make test-authority SEL4_SDK_VERSION=2.1.0`
Expected: both PASS.

- [ ] **Step 6: Prove probe 2 is not vacuous**

Temporarily change the reader's rights copy in `main.c` from read-only (`seL4_CapRights_new(0, 0, 1, 0)`) to read-write, rebuild, and re-run probe 2. It must now **FAIL** — no write fault occurs. Restore the read-only rights and confirm it passes again. Paste all three outputs in your report.

A read-only page whose probe passes even when the page is writable proves nothing. This step is the reason the probe exists.

- [ ] **Step 7: Update `docs/TCB.md`**

Add, stating the boundary exactly and claiming nothing beyond it:

```markdown
Root publishes the boot authority relation as a second read-only page
(`platform/include/platform/authority.h`, `AOS_AUTHORITY_BOOT_VA`): per
protection domain, counts of each capability kind root granted it. It is built
from the root's own accounting table. seL4 exposes no capability-enumeration
syscall, so this is a record of what root granted, not a reading of kernel
state: it cannot detect divergence between that record and the kernel, it
covers only the boot-time static set, and it does not verify the subsetting
invariant — the kernel enforces that unconditionally, since a domain cannot
mint from a capability it does not hold. `make test-authority` checks the
published counts against the compiled descriptor, including that `net_virt` and
`blk_virt` hold no IRQ handler, and a target fault probe verifies a consumer
write to the page faults. Qualification boundary: obtained under Microkit SDK
2.1.0, not the pin in `tools/sdk/default-version`; release qualification must
re-run it.
```

- [ ] **Step 8: Full verification**

Run: `make test-host`, `make policy-check`, `make test TARGET_ARCH=aarch64 GUEST_OS=none SEL4_SDK_VERSION=2.1.0`, `make test-authority SEL4_SDK_VERSION=2.1.0`, `make test-inspect SEL4_SDK_VERSION=2.1.0`, `make test-operator-session SEL4_SDK_VERSION=2.1.0`.

`make test-inspect` matters: this task adds a second page beside the one it exercises, and a mistake in the mapping loop could disturb it.

Report that `make gate` was **not** run — it needs the qualified SDK and guest images. Do not claim it passes.

- [ ] **Step 9: Commit**

```bash
git add tools/agentctl/agentctl.c xtask/src/lib.rs xtask/src/cmd_test.rs \
        Makefile docs/TCB.md
git commit -m "test: target proof for the boot authority page

Probe 1 checks published counts against the compiled descriptor, including that
the virtualizers hold no IRQ handler. Probe 2 verifies a consumer write to the
page faults, demonstrated non-vacuous by temporarily granting write rights."
```

---

## Notes for the implementer

**The honesty constraint is the hard part of this task.** It is tempting to describe this as auditing or verifying capabilities. It does neither. If you find yourself writing a sentence that implies the page reflects live kernel state, stop and reread the "What this is NOT" section. `docs/TCB.md` is the repository's binding document and overclaiming there is the most serious error available here.

**If the counts do not match the descriptor,** that is a real finding about the accounting table's completeness, not a reason to loosen the oracle. Report it with the actual numbers rather than weakening the assertion to make a test pass.

**Do not add a new protection domain.** The data originates in the root task, and a separate observer PD would only re-publish what root already knows while adding TCB surface. This was decided as open question 4 in the spec.
