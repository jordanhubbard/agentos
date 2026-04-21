# NetService — Network Device Service Contract (Device Abstraction Layer)

## Overview

NetService is the device-abstraction-layer (DAL) contract for network access
in agentOS.  It complements the higher-level `net-server` contract by defining
the lower-level vNIC and ACL interface that all guest OSes and VMMs must use.
Direct access to physical or virtio-net NICs is prohibited.

The concrete implementation lives in `kernel/agentos-root-task/src/net_server.c`
with the lwIP shim in `lwip_shim.c`.

## Status

**IMPLEMENTED.**  This contract's opcodes are a subset of the `net-server`
contract; both cover the same PD.  Use `net-server/interface.h` for the packed
struct definitions; `net-service/interface.h` provides the DAL opcode listing
in plain MR-register style (no packed structs).

## Protection Domain

Same PD as `net-server`.  See `contracts/net-server/README.md` for channel
assignments and shared memory layout.

## Operations

| Opcode | Description |
|--------|-------------|
| `NET_SVC_OP_VNIC_CREATE`  | Create virtual NIC |
| `NET_SVC_OP_VNIC_DESTROY` | Destroy virtual NIC |
| `NET_SVC_OP_SEND`         | Transmit packet via shmem |
| `NET_SVC_OP_RECV`         | Receive packet into shmem |
| `NET_SVC_OP_BIND`         | Bind port to vNIC |
| `NET_SVC_OP_CONNECT`      | Connect to remote endpoint |
| `NET_SVC_OP_STATUS`       | vNIC and global status |
| `NET_SVC_OP_SET_ACL`      | Update ACL flags |
| `NET_SVC_OP_HTTP_POST`    | High-level HTTP POST proxy |

## Source Files

- `contracts/net-service/interface.h` — DAL-style opcode listing
- `contracts/net-server/interface.h` — packed struct wire protocol
- `kernel/agentos-root-task/include/net_server.h` — authoritative OP_NET_* values
- `kernel/agentos-root-task/src/net_server.c` — implementation
