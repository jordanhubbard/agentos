# MsgBus — Message Routing Service Contract

## Overview

MsgBus is the inter-agent communication backbone of agentOS.  It provides:

- Named publish/subscribe channels with configurable flags (broadcast, ordered,
  persistent, exclusive)
- Point-to-point direct messaging between agents
- Synchronous RPC call/reply through the bus
- System channels pre-created at boot

## Protection Domain

MsgBus is embedded in the `event_bus` PD (priority 200, passive) as defined
in `tools/topology.yaml`.  It is a passive server: it only runs when a client
Protection Domain makes a Protected Procedure Call (PPC) into it.

## IPC Endpoint

Clients reach MsgBus via the `event_bus` PD's primary endpoint.  The channel
IDs from the caller's perspective depend on which PD is calling:

| Caller PD        | Channel to event_bus |
|------------------|----------------------|
| controller       | 0 (ctrl_eventbus)    |
| init_agent       | 1 (eb_init, pp_b)    |
| worker_0..7      | 1 (eb_worker_N)      |
| agentfs          | 1 (agentfs_eb)       |

The IPC label field carries the opcode (`MSGBUS_OP_*`).

## Operations

| Opcode | Value | Description |
|--------|-------|-------------|
| `MSGBUS_OP_CREATE_CHANNEL` | 0x100 | Create a named channel |
| `MSGBUS_OP_DELETE_CHANNEL` | 0x101 | Delete a channel (owner or init only) |
| `MSGBUS_OP_SUBSCRIBE`      | 0x102 | Subscribe to a channel |
| `MSGBUS_OP_UNSUBSCRIBE`    | 0x103 | Remove subscription |
| `MSGBUS_OP_PUBLISH`        | 0x104 | Publish to a channel |
| `MSGBUS_OP_SEND_DIRECT`    | 0x105 | Send directly to an agent by badge |
| `MSGBUS_OP_RECV`           | 0x106 | Receive next pending message |
| `MSGBUS_OP_LIST_CHANNELS`  | 0x107 | Enumerate active channels |
| `MSGBUS_OP_CHANNEL_INFO`   | 0x108 | Inspect a single channel |
| `MSGBUS_OP_CALL_RPC`       | 0x109 | Synchronous RPC call |
| `MSGBUS_OP_REPLY_RPC`      | 0x10A | RPC reply (callee sends this) |
| `MSGBUS_OP_HEALTH`         | 0x10B | Liveness probe |

## Error Codes

| Code | Value | Meaning |
|------|-------|---------|
| `MSGBUS_ERR_OK`          | 0  | Success |
| `MSGBUS_ERR_INVALID_ARG` | 1  | Bad opcode, null pointer, or malformed request |
| `MSGBUS_ERR_NOT_FOUND`   | 2  | Channel or agent does not exist |
| `MSGBUS_ERR_EXISTS`      | 3  | Channel name already taken |
| `MSGBUS_ERR_DENIED`      | 4  | Caller lacks required capability |
| `MSGBUS_ERR_NOMEM`       | 5  | Channel table or queue is full |
| `MSGBUS_ERR_BUSY`        | 6  | Exclusive channel already subscribed |
| `MSGBUS_ERR_TIMEOUT`     | 7  | RPC call timed out |
| `MSGBUS_ERR_MSG_TOO_BIG` | 8  | Payload exceeds 64KB |
| `MSGBUS_ERR_INTERNAL`    | 99 | Unexpected server error |

## System Channels (created at boot)

| Name                 | Flags                          |
|----------------------|--------------------------------|
| `system.broadcast`   | BROADCAST \| SYSTEM            |
| `system.events`      | ORDERED \| SYSTEM              |
| `system.log`         | PERSISTENT \| SYSTEM           |
| `system.health`      | BROADCAST \| SYSTEM            |
| `tools.registry`     | ORDERED                        |
| `models.registry`    | ORDERED                        |

## Shared Memory Conventions

Payloads larger than what fits in seL4 message registers are passed via a
shared memory region.  The request struct encodes a `payload_shmem_offset`
relative to the start of the caller's MR (mapped by the Microkit system
description).  The server reads (for publish/send) or writes (for recv) the
payload before sending the reply.

## Source Files

- `services/msgbus/msgbus.h` — internal service header
- `services/msgbus/msgbus.c` — service implementation
- `services/msgbus/msgbus_seL4.c` — seL4-specific IPC dispatch loop
- `services/msgbus/MsgBus.camkes` — CAmkES component definition
