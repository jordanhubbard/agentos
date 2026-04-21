# EventBus — Event Routing Service Contract

## Overview

EventBus is the pub/sub event routing layer for agentOS.  Agents publish typed
events to named topics; the bus delivers them to all subscribers.  Key
properties:

- Topic ownership: only the creating PD may publish (prevents squatting)
- Backpressure: subscriber queues cap at 256 events; PUBLISH fails atomically
  if any subscriber is full
- Payloads are up to 512 bytes per event
- Subscriber IDs are opaque uint32 handles; the bus holds seL4 Notification
  caps to all subscribers

EventBus also serves as the transport layer for MsgBus system channels and
for all `MSG_EVENT_*` kernel lifecycle events (see `agentos.h`).

## Protection Domain

`event_bus` is a passive PD (priority 200) defined in `tools/topology.yaml`.
All PDs in the system have channels to event_bus (see topology channels
section).

## IPC Endpoint

The `event_bus` PD listens on its primary endpoint.  Each caller PD reaches
it via a dedicated channel:

| Caller | Channel |
|--------|---------|
| controller | ctrl_eventbus (id_a=0, pp_a=true) |
| init_agent | eb_init (id_b=1, pp_b=true) |
| worker_N   | eb_worker_N |
| agentfs    | agentfs_eb |
| console_mux| eb_console |
| snapshot_sched | snap_event_publish |

## Operations

| Opcode | Value | Description |
|--------|-------|-------------|
| `EVENTBUS_OP_CREATE_TOPIC` | 0x600 | Create and own a topic |
| `EVENTBUS_OP_SUBSCRIBE`    | 0x601 | Subscribe to a topic |
| `EVENTBUS_OP_UNSUBSCRIBE`  | 0x602 | Cancel subscription |
| `EVENTBUS_OP_PUBLISH`      | 0x603 | Publish event (owner only) |
| `EVENTBUS_OP_DRAIN`        | 0x604 | Retrieve pending events |
| `EVENTBUS_OP_STATUS`       | 0x605 | Topic statistics |
| `EVENTBUS_OP_HEALTH`       | 0x606 | Liveness probe |

### Legacy seL4 Label Opcodes (from agentos.h)

These opcodes appear as the seL4_MessageInfo label field in the lower-level
kernel IPC path:

| Constant | Value | Description |
|----------|-------|-------------|
| `MSG_EVENTBUS_INIT`          | 0x0001 | Init handshake |
| `MSG_EVENTBUS_SUBSCRIBE`     | 0x0002 | Subscribe (legacy) |
| `MSG_EVENTBUS_UNSUBSCRIBE`   | 0x0003 | Unsubscribe (legacy) |
| `MSG_EVENTBUS_STATUS`        | 0x0004 | Status query (legacy) |
| `MSG_EVENTBUS_PUBLISH_BATCH` | 0x0005 | Batch publish (up to 16 events) |

## Error Codes

| Code | Value | Meaning |
|------|-------|---------|
| `EVENTBUS_ERR_OK`           | 0  | Success |
| `EVENTBUS_ERR_INVALID_ARG`  | 1  | Null topic or bad opcode |
| `EVENTBUS_ERR_UNKNOWN_TOPIC`| 2  | Topic not registered |
| `EVENTBUS_ERR_UNKNOWN_SUB`  | 3  | Subscriber ID not valid |
| `EVENTBUS_ERR_UNAUTHORIZED` | 4  | Publisher is not topic owner |
| `EVENTBUS_ERR_PAYLOAD_BIG`  | 5  | Payload exceeds 512 bytes |
| `EVENTBUS_ERR_QUEUE_FULL`   | 6  | Subscriber queue at 256 limit |
| `EVENTBUS_ERR_OVERFLOW`     | 7  | Ring buffer full (seL4 IPC path) |
| `EVENTBUS_ERR_INTERNAL`     | 99 | Unexpected server error |

## Well-Known System Events

| Constant | Value | Published by |
|----------|-------|-------------|
| `EVT_OBJECT_CREATED`      | 0x0411 | agentfs |
| `EVT_OBJECT_DELETED`      | 0x0412 | agentfs |
| `EVT_OBJECT_EVICTED`      | 0x0413 | agentfs |
| `EVENT_SNAP_SCHED_DONE`   | 0x20   | snapshot_sched |
| `EVENT_POLICY_RELOADED`   | 0x30   | cap_broker |
| `MSG_GPU_COMPLETE`        | 0x0910 | gpu_sched |
| `MSG_GPU_FAILED`          | 0x0911 | gpu_sched |
| `MSG_MESH_PEER_DOWN`      | 0x0A08 | mesh_agent |

## Source Files

- `userspace/servers/event-bus/src/lib.rs` — Rust server implementation
- `kernel/agentos-root-task/src/monitor.c` — seL4 dispatch layer
- `kernel/agentos-root-task/include/agentos.h` — MSG_EVENTBUS_* constants
- `tools/topology.yaml` — event_bus PD and channel definitions
