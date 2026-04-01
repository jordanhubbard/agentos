# agentOS Priority Inheritance for Passive PD PPCs

**Status:** Implemented (v0.1.0)  
**Author:** Peabody (horde-dgxc)  
**Date:** 2026-03-31  
**Reference:** seL4 Reference Manual §6.3; natasha-idea wq-API-1774942815700

---

## Problem

In seL4 Microkit, passive PDs run on the **caller's** scheduling context.
A caller's scheduling context carries the caller's *priority*.  A passive server
PD therefore executes at the caller's priority for the duration of the PPC.

This creates **priority inversion** in the agentOS PD graph:

| Caller | Caller prio | Passive server | Server prio | Inversion window |
|--------|-------------|----------------|-------------|------------------|
| controller | 50 | event_bus | 200 | Any worker (80) can preempt event_bus while controller calls it |
| controller | 50 | agentfs | 150 | Workers (80) can preempt agentfs call |
| controller | 50 | console_mux | 160 | Same |
| controller | 50 | vibe_engine | 140 | Same |
| controller | 50 | swap_slot_N | 75 | Workers (80) can preempt health check |
| init_agent | 100 | event_bus | 200 | Workers (80) cannot preempt (100 > 80), but still sub-optimal |
| worker_N | 80 | mem_profiler | 108 | No inversion here (caller < server = fine) |

The **worst case** is controller (prio 50) calling event_bus (prio 200) while
any worker (prio 80) is runnable: the event_bus PPC runs at 50, any worker
preempts mid-call.  This adds 1–N worker scheduling quanta to every event
publish / subscribe latency — unacceptable on the GPU interrupt path.

---

## Solution: Explicit Priority Donation

Before entering a PPC into a higher-priority passive PD, the calling thread
temporarily **raises its own scheduling context priority** to the server's
declared priority.  The server then runs at that elevated priority for the
duration of the call.  Priority is restored immediately on PPC return.

seL4 mechanism: `seL4_TCB_SetSchedParams(MICROKIT_TCB_CAP, ...)`.  When
`MICROKIT_TCB_CAP` is not available (non-MCS kernel), the call degrades
gracefully to a plain `microkit_ppcall`.

---

## Implementation

### New header: `include/prio_inherit.h`

Provides:

- `prio_donate_begin(caller_prio, server_prio)` — boosts self priority to
  `server_prio` if `server_prio > caller_prio`; returns saved priority.
- `prio_donate_end(caller_prio, saved_prio)` — restores priority.
- `ppcall_with_prio(ch, msg, caller_prio, server_prio)` — inline wrapper.
- `PPCALL_DONATE(ch, msg, caller_prio, server_prio)` — convenience macro,
  drop-in replacement for `microkit_ppcall`.
- PD priority constants (`PRIO_CONTROLLER`, `PRIO_WORKER`, etc.).

### Updated call sites

| File | Change |
|------|--------|
| `src/monitor.c` | 5 PPC calls to `event_bus` and `agentfs` → `PPCALL_DONATE` |
| `src/init_agent.c` | 2 PPC calls to `event_bus` → `PPCALL_DONATE` |
| `src/vibe_swap.c` | Health check PPC to `swap_slot_N` → `PPCALL_DONATE` |
| `src/mem_profiler.c` | Leak alert PPC to `controller` → `PPCALL_DONATE` (cosmetic) |

### Before / After (controller → event_bus)

```c
// Before — priority inversion window:
microkit_ppcall(CH_EVENTBUS, msg);
// event_bus runs at prio 50, workers (80) can preempt

// After — no inversion:
PPCALL_DONATE(CH_EVENTBUS, msg, PRIO_CONTROLLER, PRIO_EVENTBUS);
// controller boosts self to 200 → PPC runs at 200 → nothing preempts
// priority restored to 50 on return
```

---

## Priority Table (agentos.system)

| PD | Priority | Type |
|----|----------|------|
| controller | 50 | active |
| swap_slot_N | 75 | active |
| worker_0..7 | 80 | active |
| init_agent | 100 | active |
| mem_profiler | 108 | passive |
| vibe_engine | 140 | passive |
| agentfs | 150 | passive |
| console_mux | 160 | passive |
| event_bus | 200 | passive |
| monitor | 254 | active |

---

## Correctness Properties

1. **No deadlock:** Priority donation is one-level only (no chain).
   The calling PD is the only thread holding the boosted SC.

2. **No starvation:** Lower-priority threads (workers) remain runnable
   during donation — they just cannot preempt the PPC window.

3. **Bounded donation:** Priority is restored unconditionally on PPC return
   (seL4 PPC is synchronous; no async paths in Microkit).

4. **MCS kernel required for effect:** On standard seL4, `MICROKIT_TCB_CAP`
   is undefined; the header compiles to plain `microkit_ppcall` — no
   correctness change, just no inversion fix.

---

## Testing

- QEMU RISC-V: `make BOARD=qemu_virt_riscv64`
- Expected: `[controller] Waking EventBus via PPC...` succeeds without
  latency spikes from worker preemption in single-core QEMU (scheduling
  is deterministic).
- Multi-core: monitor for missing `PPCALL_DONATE` in new PPC call sites
  when adding passive PDs.

---

## Limitations (v0.1.0)

1. **seL4 standard kernel:** `seL4_TCB_SetSchedParams` requires MCS.
   On non-MCS kernels, donation is a no-op.
2. **No automatic enforcement:** New PPC call sites must manually use
   `PPCALL_DONATE`.  A static analyser pass (grep/clang-tidy) is recommended
   for CI to catch bare `microkit_ppcall` calls into passive servers.
3. **Single-level donation:** Does not handle chains (A→B→C where each is
   passive).  Sufficient for the current two-tier PD graph.
