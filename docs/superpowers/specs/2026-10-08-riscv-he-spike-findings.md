# RISC-V H-extension spike: findings

**Date:** 2026-10-08
**Status:** throwaway experiment, complete. **The spike directory is to be deleted on 2026-10-22**
(the throwaway date set in its charter *before* the work started, per
`2026-10-06-riscv-hypervisor-fork-feasibility.md` §6 "What would make it a bad spike").
**This document is the only artefact that leaves the scratch directory.** No agentOS code,
no `tools/sdk/`, no `.github/workflows/`, no `docs/TCB.md`, no PD, no root task, no system
descriptor was touched. Nothing here licenses any agentOS claim.

Executes §6 of `docs/superpowers/specs/2026-10-06-riscv-hypervisor-fork-feasibility.md`.
Full log with raw output:
`<scratch>/riscv-he-spike/SPIKE-LOG.md` (35 build and boot logs beside it).

---

## 1. What was proven, and what was not

### Proven

1. **The fork stack builds from source in 2026.** `Ivan-Velickovic/seL4@microkit_riscv_he`
   (`6c4ffa2de`) + `Ivan-Velickovic/microkit@riscv_he` (`883a115a`), at their pinned SHAs,
   unrebased, via `build_sdk.py --boards qemu_virt_riscv64`: **exit 0 in 52 seconds with
   zero patches**, on a 2026 toolchain (`riscv64-unknown-elf-gcc` 14.2.0, cmake 3.31.6,
   Python 3.13.5). `CONFIG_RISCV_HYPERVISOR_SUPPORT 1` in the generated kernel config.

2. **Linux boots as a guest on riscv64 under seL4/Microkit on QEMU, to a shell prompt.**
   `au-ts/libvmm@riscv` `examples/simple`, `MICROKIT_BOARD=qemu_virt_riscv64`, QEMU 11.1.1.
   Marker line, verbatim from `logs/08-boot-simple-buildroot2.log`:

   ```
   Welcome to Buildroot
   buildroot login: root
   # uname -a
   Linux buildroot 6.15.0 #1 SMP Thu Oct  8 09:47:13 UTC 2026 riscv64 GNU/Linux
   # id
   uid=0(root) gid=0(root) groups=0(root),10(wheel)
   ```

   Interactive, with the supplied `buildroot_config` rootfs. Reproduced independently with
   a second, hand-built BusyBox rootfs (`~ #`).

3. **`examples/virtio` boots and all three libvmm-emulated devices work**, after two
   one-line patches (§3). From `logs/06-boot-virtio-busybox2.log` and
   `logs/09-boot-virtio-buildroot.log`:

   ```
   virtio0 -> ../../../devices/platform/soc/130000.virtio-console/virtio0
   virtio1 -> ../../../devices/platform/soc/150000.virtio-blk/virtio1
   virtio2 -> ../../../devices/platform/soc/160000.virtio-net/virtio2
   ```

   Those are the guest-DTS addresses of `src/virtio/{console,block,net}.c` — emulated, not
   passthrough. `MON|ERROR` count on that boot: **0**. And past *boots* to *works*:

   ```
   # ping -c 3 10.0.2.2
   3 packets transmitted, 3 packets received, 0% packet loss
   # echo "SPIKE-BLOCK-WRITE-MARKER-12345" | dd of=/dev/vda bs=512 seek=100 conv=notrunc
   # dd if=/dev/vda bs=512 skip=100 count=1 | head -c 40
   SPIKE-BLOCK-WRITE-MARKER-12345
   ```

4. **§1.1, §1.2 and §2.2's measurements are exactly right.** Independently reproduced:
   `37 files changed, 1578 insertions(+), 25 deletions(-)`; 2,340 patch lines; and the
   rebase onto `seL4/seL4` master `6df0b6ee6` (2026-10-04) gives **6 files with conflicts,
   7 conflict hunks, the same six files**. §1.3(b) (no `vmid_pool_cap` reference outside
   the `.bf`), §1.3(c) (unconditional `return 14`) and §1.3(e) (commented-out kernel-abort
   check, still on the tip) all confirmed by direct reading of the tree.

5. **The rebased hypervisor kernel compiles and boots.** With the 7 conflicts resolved
   (~10 minutes), `build_sdk.py` against today's master exits 0, and the resulting kernel
   gets through `seL4 configured as hypervisor` → `Bootstrapping kernel` →
   `Booting all finished, dropped to user space`.

### Not proven

1. **That the rebased system runs.** It dies immediately after the kernel hands off:

   ```
   MON|INFO: Microkit Bootstrap
   ...
   FAIL: MON|ERROR: found mismatch between boot info and untyped info
   ```

   The fork's Microkit (399 commits behind) and upstream seL4's untyped/boot-info layout
   no longer agree. This is a Microkit-side failure, upstream of the hypervisor code, so
   the spike **cannot say** whether the hypervisor code itself survived the rebase. §2.2's
   *textual* claim is now an observation; its *semantic* claim is not. I hit the first hard
   semantic failure within ten minutes of the rebased kernel's first boot — in exactly the
   place §2.2 said to look.

2. **Anything about hardware.** QEMU only.

3. **Anything about correctness.** A Linux boot, a ping and a `dd` round-trip are smoke
   tests. Every boot above is single-hart (`smp: Brought up 1 node, 1 CPU`). VMID reuse,
   TLB shootdown across harts, guest FPU state across preemption and every isolation
   property remain untested. **The VMID aliasing in §1.3(b) is still unimplemented and
   would not have shown up in any of this.**

4. **Anything about capDL or rust-sel4.** The fork's Microkit is confirmed pre-capDL
   (`tool/microkit/Cargo.toml` depends only on `roxmltree`/`serde`/`serde_json`). Option
   (a)'s two largest unknowns are exactly as unvalidated as they were on 2026-10-06.

5. **Anything about maintenance.** One build, one rebase attempt.

---

## 2. The finding that changes an assumption: the fork stack is not pinnable

Option (a)'s maintenance line reads: *"**~0** if frozen. Pin exact SHAs, never rebase, let
it rot deliberately."*

**That is not currently possible.** `au-ts/libvmm@riscv` tip `8b7011e9` — whose entire
content is "pin sddf submodule to branch with P550 patches" — points `dep/sddf` at
`e145d311d08a6f16c3e8cbf6791ae1acd7ae26d9`, and that object **no longer exists on
`au-ts/sddf`**:

```
$ git fetch origin e145d311d08a6f16c3e8cbf6791ae1acd7ae26d9
fatal: remote error: upload-pack: not our ref e145d311d08a6f16c3e8cbf6791ae1acd7ae26d9

$ git fetch origin 5febd61c5be1e719d3c51bddba63e258a433ac05      # the previous pin
 * branch            5febd61c... -> FETCH_HEAD
```

A working branch was deleted or force-pushed, taking the only published copy of the tip's
dependency with it. Thirteen months after the branch was last touched, **the head of the
fork stack is already unbuildable from upstream sources.** I used `5febd61c` (the pin from
the immediately preceding commit) instead; since the tip's only change is P550-specific and
this spike is QEMU-only, that is the closest faithful reproduction available.

This is not a disaster — a vendored mirror fixes it — but it changes the shape of option
(a). "Freeze and forget" is not free: it requires **mirroring all four repositories plus
the sDDF submodule into storage this project controls, now, before more history
disappears**, and it means any later attempt to reproduce the authors' exact result is
already impossible for anyone who did not clone before the deletion. Add roughly a day of
one-time work and a standing mirror to option (a), and treat the "pin by SHA from the
canonical repository" discipline that `sdk-candidate.yml:42-47` relies on as **unavailable
for this stack**.

A second, smaller instance of the same class: `examples/virtio/meta.py` begins

```python
assert version('sdfgen').split(".")[1] == "26", "Unexpected sdfgen version"
```

and `sdfgen==0.26.0` ships **no linux-aarch64 wheel and no sdist**. On an arm64 Linux
builder — which is what a CI runner for this would plausibly be — the virtio example cannot
be built at all without building `au-ts/microkit_sdf_gen` from source or shimming the
metaprogram onto another host (which is what I did).

---

## 3. What had to be patched

**Nothing in seL4. Nothing in Microkit.** Both built first try. Everything below is libvmm
or sDDF.

### Source patches — 2, both one-liners

| # | File | Change | Why |
| --- | --- | --- | --- |
| P1 | `examples/virtio/client_vm/riscv64/linux.dts` | delete `status = "disabled"` from the guest `virtio-blk` node | Left over from `e65e8c1f wip: getting virtio example working on p550, without block support`. A follow-up commit re-enabled only *net* (`60a29a30`). QEMU and P550 share one DTS, so virtio-blk has been shipped disabled on the tip for four months. |
| P2 | `dep/sddf/drivers/network/virtio/ethernet.c` | `RX_COUNT`/`TX_COUNT` 512 → 256 | The driver hard-asserts `regs->QueueNumMax >= RX_COUNT`. QEMU's virtio-mmio `virtio-net-device` refuses anything else: `Invalid tx_queue_size (= 512), must be a power of 2 between 256 and 256`. The shipped driver **can never** initialise against QEMU. Already fixed on sDDF `main`, where the constant became `QUEUE_SIZE_MAX` and is negotiated. |

### Build-configuration changes — no source edited

- `make SDDF_CUSTOM_LIBC=1` for `examples/simple`, which otherwise fails to link:
  `ld.lld: error: undefined symbol: memset` / `memcpy`. `simple.mk` adds the `custom_libc`
  include path but never sets the flag that compiles those objects; `virtio.mk` does.
- `LINUX=` and `INITRD=` passed explicitly. For `qemu_virt_riscv64`, `simple.mk` defines
  neither; `LINUX` falls through to the **AArch64** default image and `INITRD` is empty,
  producing `package_guest_images.S:43:9: error: Could not find incbin file ''`.
- QEMU `-device virtio-blk-device,…,queue-size=1024` (same root cause as P2, satisfiable
  from the command line for block).

### Supply chain — no guest images exist for riscv64

`examples/simple/board/qemu_virt_riscv64/` ships `linux_config` and `buildroot_config` and
no images, and there is no download rule. `README.md` on the `riscv` branch still lists
only `qemu_virt_aarch64`, `odroidc4`, `maaxboard`. So "boot the supplied buildroot Linux"
means: build Linux 6.15 from the supplied kconfig (**7 min**) and run buildroot 2025.05.1
with its own internal toolchain from the supplied kconfig (**37 min**). Both worked. This
is undocumented and the study did not surface it; budget half a day for a human doing it
cold, and note that every future rebase re-opens the question of whether those configs
still apply.

### Bug found, left unpatched — and it is in code agentOS depends on

`src/virtio/console.c:187`, `virtio_console_handle_rx`:

```c
    struct virtio_queue_handler *vq = &console->virtio_device.vqs[RX_QUEUE];
    while (vq->last_idx != vq->virtq.avail->idx && ...)
```

No readiness check. If console input arrives before the guest has programmed the RX queue,
`vq->virtq.avail` is NULL and the VMM takes a data fault:

```
MON|ERROR: faulting PD: CLIENT_VMM
MON|ERROR: VMFault: ip=0x00000000002030e2  fault_addr=0x0000000000000002  fsr=0x0000000000000001  (data fault)
```

(disassembly confirms `lhu a1, 0x2(a5)` with `a5 == 0` inside `virtio_console_handle_rx`).
libvmm `main` has the guard:

```c
    if (!vq->ready) {
        /* It is valid for RX from the real device before the guest has started, ... */
```

---

## 4. What this says about the study's own measurements

| Study claim | Verdict |
| --- | --- |
| §1.1/§1.2 delta: 37 files, +1,578/-25, 2,340 patch lines, no generated files | **Confirmed exactly.** |
| §2.2 rebase: 6 files with conflicts, 7 hunks, named files; `vcpu.c`/`machine.h`/`vspace.c`/`hardware.c`/`c_traps.c`/`.bf`/libsel4 clean | **Confirmed exactly.** |
| §2.2 "textual rebase is roughly a day" | **Confirmed, and generous.** ~10 minutes to resolve all seven. Four were pure CMake re-formatting. |
| §2.2 "what the experiment does not show is whether the result still boots" | **Confirmed as the real risk**, and now with a concrete instance: the rebased kernel boots and the fork's Microkit rejects its boot info. |
| §1.3(b) VMID aliasing, caps declared and never used | **Confirmed.** `git grep vmid_pool_cap\|vmid_control_cap -- '*.c' '*.h'` is empty. |
| §1.3(c) unconditional `return 14` for `seL4_RISCV_PageTableObject` | **Confirmed**, not inside any `#ifdef`. |
| §1.3(e) commented-out `KERNEL ABORT` check still on the tip | **Confirmed.** |
| §2.1 "libvmm@riscv tip `8b7011e9`" | Correct, but **incomplete and now partly moot**: that commit's sDDF pin is unfetchable (§2 above). |
| §1.5 "The agentOS-relevant virtio devices (`src/virtio/{net,block,console}.c`) are arch-independent and **unaffected**" | **Materially misleading, and this is the one to correct.** True of the *diff*. False of the *result*: the `riscv` branch is 424 commits behind `main` and therefore ships an **older** copy of those arch-independent files, containing at least one null-pointer dereference that `main` has since fixed (§3), and the riscv64 example ships with guest virtio-blk disabled. Adopting the fork means adopting a stale snapshot of the arch-independent virtio stack, not just the RISC-V additions. |
| Option (a) maintenance "~0 if frozen; pin exact SHAs, never rebase" | **Invalidated as written.** You cannot pin what upstream has deleted. Add a mandatory vendored mirror of all four repositories plus sDDF. |
| §6's "Day 1 toolchain" / "Days 2–3 build" | **Both overestimates** given the Foundation's Docker image: 3 minutes and 2 minutes respectively, zero patches. The real cost in days 4–8 is libvmm/sDDF integration and the missing guest images. |

Nothing found contradicts §4 (the verification analysis) or §3.1 (what forking does to
`sdk-candidate`). Those arguments stand untouched.

---

## 5. Wall-clock effort

One session, 02:30–03:40 PDT, 2026-10-08.

| Phase | §6 budget | Wall | Active | Blockers |
| --- | --- | ---: | ---: | ---: |
| Verify environment | — | 2 min | 2 min | 0 |
| Toolchain + confirm QEMU `h` | day 1 | 3 min | 3 min | 0 |
| seL4 + Microkit SDK at pinned SHAs | days 2–3 | 2 min | 2 min | **0** |
| `examples/simple` → shell prompt | days 4–6 | 42 min | ~25 min | 3 |
| `examples/virtio` → 3 devices + I/O | days 7–8 | 36 min | ~36 min | 4 |
| Optional rebase | +1 week | 12 min | 12 min | 1, unfixed |
| **Total** | **~10 d + 1 w** | **~70 min** | **~80 min** | **8** |

Read those numbers with care. A fast machine, a native-arm64 prebuilt toolchain image and
an agent that does not tire compress the clock enormously; the buildroot run (37 min) was
background latency, not labour. Actual debugging across all eight blockers was perhaps 30
minutes. The honest statement is: **every blocker was shallow.** None required
understanding VS-mode CSR save/restore, G-stage translation, or the vtimer multiplexer. The
deepest thing I had to do was disassemble a VMM fault PC to identify a null dereference —
fifteen minutes.

---

## 6. Calibrated opinion on option (b)'s 16–26 person-weeks

**The estimate is credible as a floor and I would not reduce it. I would widen the top of
the range to about 30 weeks, and I now think the risk is distributed differently than the
study assumed.**

Three things the spike actually changes:

**(a) The "getting it working" half of option (b) is cheaper than the study implies, and
option (a)'s 3–6 weeks is generous.** The study budgets "Toolchain + build (3–5 d)" for
option (a). Reality, with the seL4 Foundation's Docker image: five minutes to a booting
SDK, zero patches. The fork's kernel and Microkit are *well-behaved as build artefacts*.
If someone tells you a riscv64 Linux guest on seL4 is weeks away from a demo, they are
wrong; it is an afternoon.

**(b) Every one of the eight blockers I hit was in integration glue, not kernel code — and
that is exactly why the estimate should not come down.** The study's §5(b) line items are
`de-contaminate 167 unguarded lines (2w)`, `proper VMID pool + TLB shootdown (3–4w)`,
`real stage-2 root PT object (2–3w)`, `guest FPU (1–2w)`, `htinst/hlvx (1–2w)`,
`sel4test RISC-V vCPU suite (2–3w)`, `SMP and multi-vCPU validation (2–3w)`. **The spike
touched none of them.** It could not: a single-hart Linux boot does not exercise VMID
reuse, cross-hart TLB shootdown, FPU state across preemption, or any of the fault-decode
corner cases where `htinst` reads zero. Those are the 16–26 weeks. My 80 minutes bought the
first 3–6-week item on the list and nothing else.

The study warned that "static reading systematically understates how long 'attempt to fix
guest FPU access' takes to finish." I can now confirm the *direction* but not the
magnitude, because I never got near it. What I can say first-hand is that the one place I
did have to make a semantic judgement during the rebase — resolving `include/machine/fpu.h`
between upstream's rewritten `switchLocalFpuOwner` and the fork's
`if (tcbVCPU != NULL) enableFpu()` — I resolved by *keeping both*, and **I have no idea
whether that is correct.** There is no test that would tell me. There is no upstream to
ask. That single hunk is a microcosm of option (b): the mechanical work is trivial and the
responsibility is not.

**(c) The code quality is exactly what the study described, and the character of it is
worse than the line count suggests.** Three independent instances of the same pattern, now
confirmed by hand:

- `src/arch/riscv/c_traps.c` — the kernel-abort safety check `//`-commented out
  "temporarily", fifteen months ago, still on the tip.
- `examples/virtio/client_vm/riscv64/linux.dts` — virtio-blk disabled "without block
  support" while debugging P550, four months ago, still on the tip, affecting QEMU too.
- `src/arch/riscv/object/objecttype.c` — `return 14` hardcoded globally rather than behind
  the config flag, with the correct `s2_root_page_table_cap` declared one file over and
  never used.

This is not sloppiness; it is a research prototype behaving exactly as a research prototype
should. But it means **you cannot bound the work by reading the diff**, because the diff
does not show you the things that were temporarily disabled and never re-enabled. I found
two more of those in 80 minutes without looking for them. The study's `de-contaminate the
167 unguarded lines (2w)` line item assumes you know which 167 lines; discovering the
*disabled* code is unbudgeted, and it is the kind of thing that only surfaces when a test
you did not write fails on hardware you do not have yet.

**Where I would widen the range.** Add to the top of the band:

- A vendored mirror of four repositories plus sDDF, because the pinned stack is already
  partly unfetchable (§2). ~1 week one-time, plus standing storage discipline.
- Re-doing the guest-image build (Linux kconfig + buildroot kconfig) as something
  reproducible rather than two undocumented kconfigs. ~0.5 week.
- Bringing the arch-independent libvmm/sDDF code forward onto `main` — which §1.5 treated
  as free because the diff does not touch it, and which is not free, because the branch is
  424 commits behind and the delta includes real bug fixes in exactly
  `src/virtio/{net,block,console}.c`. The study budgets 5 days for the libvmm rebase; after
  seeing one null-deref in the console path in a 70-minute session, I would say 1.5–2
  weeks, and I would expect to find more.

That pushes a realistic band to roughly **18–30 person-weeks for an engineer who already
knows seL4 and RISC-V privileged architecture** — i.e. the study's number, not materially
wrong, slightly optimistic at the top, and **right for the right reasons**: the kernel
delta genuinely is small and genuinely does rebase cleanly, and that genuinely is not where
the cost is.

**And the study's actual recommendation is unaffected.** The sentence *"the fork is cheap
to carry and expensive to be responsible for"* is, after doing it, the single most accurate
line in the document. I carried it in 80 minutes. I would not sign off on its correctness
after 80 weeks.

---

## 7. Recommendation to the reviewer

1. The spike succeeded. Both criteria met, with the markers above. Record it and move on.
2. **Do not let it become permanent.** Delete
   `<scratch>/riscv-he-spike/` on **2026-10-22**. Nothing in it is load-bearing and nothing
   in it should become so.
3. If option (a) is ever taken, **mirror the repositories first** — the stack is already
   decaying upstream (§2) and that is a one-way door.
4. The optional rebase week should be considered **attempted and inconclusive**, not
   skipped. If the semantic rebase question matters to the decision, it needs the Microkit
   re-do on current upstream first, which is the 3–5 days + 1–2 weeks of rust-sel4 work the
   study already costs — i.e. it is not a cheap follow-up.
5. Nothing here changes `docs/TCB.md`, the OS-claim gate, or any agentOS claim, and I have
   written nothing into this repository except this file.
