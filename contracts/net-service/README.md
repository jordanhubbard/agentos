# NetService — Network Device Service Contract (Device Abstraction Layer)

## Overview

NetService is the device-abstraction-layer (DAL) contract for network access
in agentOS. The canonical `net_pd` implementation owns host network hardware
and exposes raw Ethernet packet I/O to VMMs and native services. Direct access
to physical or host virtio-net transports from a VMM is prohibited.

The concrete implementation lives in `kernel/agentos-root-task/src/net_server.c`
with the lwIP shim in `lwip_shim.c`.

## Status

**IMPLEMENTED.** Version 2 documents the established `MSG_NET_*` raw-device
wire values and the shared-memory offset returned by `RAW_OPEN`. The legacy
vNIC/ACL opcodes remain reserved for compatibility.

## Protection Domain

`net_pd`, implemented by `services/net-service/net_pd.c`.

On QEMU AArch64, `net_pd` exclusively owns the page-isolated host virtio-net
transport on bus.16 and a private DMA frame. Linux VMMs share only the
agentOS packet staging frame and call `RAW_SEND`/`RAW_RECV`. FreeBSD uses a
separate slot through the same contract. Native agents are peers, not a second
network stack: `init_agent` opens its own slot through
`native_net_client_open()` and can issue the same raw send/receive operations
without a guest or VirtIO frontend.

## Operations

| Opcode | Description |
|--------|-------------|
| `NET_SVC_OP_RAW_OPEN`     | Open the canonical physical interface |
| `NET_SVC_OP_RAW_CLOSE`    | Close a raw interface handle |
| `NET_SVC_OP_RAW_SEND`     | Send an Ethernet frame from shared memory |
| `NET_SVC_OP_RAW_RECV`     | Receive an Ethernet frame into shared memory |
| `NET_SVC_OP_RAW_STATUS`   | Query link and packet counters |
| `NET_SVC_OP_RAW_CONFIGURE`| Configure MTU and flags |
| `NET_SVC_EVENT_RX_READY`  | One-way event asking a VMM to drain raw RX |
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
- `services/net-service/net_pd.c` — canonical device implementation
