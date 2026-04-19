# TimerService — Timer and Clock Service Contract

## Overview

TimerService provides wall-clock reads, one-shot alarms, alarm cancellation,
and cooperative sleep primitives.  Guest OSes and VMMs must use this service
for all time-based operations; direct programming of ARM Generic Timer,
x86 APIC timer, or other hardware timers is prohibited.

Features:
- Monotonic nanosecond clock (64-bit, wraps after ~584 years)
- One-shot alarms with seL4 Notification delivery
- Alarm cancellation by alarm_id
- Cooperative sleep (blocks caller until duration elapses)
- Time partition budget queries (via time_partition PD)

## Status

**IMPLEMENTED.** Time policy is enforced by `time_partition.c` (passive PD,
priority 250); the controller's 10ms periodic tick drives `CH_TIMER`
notifications.

## Protection Domain

The `time_partition` PD (priority 250, passive) provides the time budget
enforcement layer.  Channel assignments from the caller's perspective:

| Caller | Channel to time_partition |
|--------|--------------------------|
| controller | ctrl_tp (id_a=56, pp_a=true) |
| worker_N   | worker_N_tp (pp_a=true) |
| init_agent | init_tp (id_a=5, pp_a=true) |

## Time Representation

All time values are in nanoseconds since system boot.  Values cross IPC as
a 64-bit quantity split across two 32-bit MRs: `(hi << 32 | lo)`.

## Operations

| Opcode | Description |
|--------|-------------|
| `TIMER_SVC_OP_GET_TIME`    | Read current monotonic clock |
| `TIMER_SVC_OP_SET_ALARM`   | Set one-shot alarm at absolute time |
| `TIMER_SVC_OP_CANCEL_ALARM`| Cancel a pending alarm |
| `TIMER_SVC_OP_SLEEP`       | Block caller for duration_ns |
| `TIMER_SVC_OP_STATUS`      | Active alarm count and clock info |

## Source Files

- `contracts/timer-service/interface.h` — canonical IPC contract
- `kernel/agentos-root-task/src/time_partition.c` — PD implementation
- `kernel/agentos-root-task/include/agentos.h` — CH_TIMER, ctrl_tp channel
