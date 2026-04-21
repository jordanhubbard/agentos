# NetServer — Network Stack Service Contract

## Overview

NetServer is the network isolation layer for agentOS.  All TCP/IP traffic
from agent workloads is mediated through this passive PD.  Agents never hold
raw network capabilities; instead they request vNICs and send/receive packets
through the server's shared memory ring buffers.

Core features:
- Up to 16 virtual NICs (vNICs), each with a 15KB packet ring in shmem
- Per-vNIC ACL flags (OUTBOUND, INBOUND, INTERNET) enforced on every send
- Locally-administered MAC addresses: `02:00:00:00:00:NN`
- Loopback routing (127.x.x.x) between local vNICs without leaving the PD
- smoltcp TCP/IP integration pending (all CONNECT/RECV stubs return NET_ERR_STUB)
- High-level HTTP POST proxy (`NET_OP_HTTP_POST`) for agent→model inference

## Protection Domain

NetServer is a passive PD (priority 160) defined in the kernel system
description.  It maps `net_packet_shmem` (256KB) for vNIC ring buffers and
`vibe_staging` for the HTTP POST proxy.

Channel IDs from NetServer's own perspective:

| Source PD | Channel ID |
|-----------|-----------|
| controller | NET_CH_CONTROLLER = 0 (pp=true) |
| init_agent | NET_CH_INIT_AGENT = 1 (pp=true) |
| worker_0   | NET_CH_WORKER_0 = 2 (pp=true) |
| worker_1..7 | NET_CH_WORKER_1..7 = 3..9 |
| app_manager | NET_CH_APP_MANAGER = 10 |
| timer      | NET_CH_TIMER = 12 |

## Shared Memory Layout

`net_packet_shmem` (256KB) is partitioned into 16 slots of 16KB each:

```
Slot N: byte offset N * 16384 (0x4000)
  [0 .. 1023]    net_vnic_ring_t header (magic, TX/RX head/tail, drop counters)
  [1024 .. 16383] packet data ring (15KB)
```

Callers use `shmem_slot_offset` returned by `NET_OP_VNIC_CREATE` to locate
their slot, then read/write packets at offsets within that slot.

## Operations

| Opcode | Value | Description |
|--------|-------|-------------|
| `NET_OP_VNIC_CREATE`  | 0xB0  | Create a vNIC (returns slot offset) |
| `NET_OP_VNIC_DESTROY` | 0xB1  | Destroy vNIC, free slot |
| `NET_OP_VNIC_SEND`    | 0xB2  | Transmit packet |
| `NET_OP_VNIC_RECV`    | 0xB3  | Receive packet (stub) |
| `NET_OP_BIND`         | 0xB4  | Bind port to vNIC |
| `NET_OP_CONNECT`      | 0xB5  | Connect to remote (stub) |
| `NET_OP_STATUS`       | 0xB6  | Query vNIC or global status |
| `NET_OP_SET_ACL`      | 0xB7  | Update vNIC ACL flags |
| `NET_OP_HEALTH`       | 0xB8  | Liveness probe |
| `NET_OP_CONN_STATE`   | 0xB9  | Query lwIP connection state |
| `NET_OP_TCP_CLOSE`    | 0xBA  | Close lwIP TCP connection |
| `NET_OP_HTTP_POST`    | 0x500 | HTTP POST proxy to agentOS bridge |

## Error Codes

| Code | Value | Meaning |
|------|-------|---------|
| `NET_ERR_OK`       | 0 | Success |
| `NET_ERR_NO_VNICS` | 1 | vNIC table full (max 16) |
| `NET_ERR_NOT_FOUND`| 2 | vnic_id not registered |
| `NET_ERR_PERM`     | 3 | Capability or ACL denied |
| `NET_ERR_STUB`     | 4 | Feature not yet implemented (smoltcp pending) |
| `NET_ERR_INVAL`    | 5 | Invalid argument |

## HTTP POST Proxy

`NET_OP_HTTP_POST` provides a high-level HTTP client used by `aos_http_post()`
in libagent.  It connects to `10.0.2.2:8790` (QEMU user-networking host) and
posts the request body.  The URL and body live in the `vibe_staging` MR shared
between the VibeEngine PD and NetServer.  This path is used by ModelSvc to
forward inference requests to the host-side bridge.

## smoltcp Integration Points

All TCP socket operations (`NET_OP_CONNECT`, `NET_OP_VNIC_RECV`) are stubs.
To wire up smoltcp, implement the `EthernetInterface` callbacks at every
`SMOLTCP_INTEGRATION_POINT` comment in `net_server.c`.

## Source Files

- `kernel/agentos-root-task/include/net_server.h` — canonical opcode and
  struct definitions (the authoritative source for OP_NET_* values)
- `kernel/agentos-root-task/src/` — net_server.c implementation
