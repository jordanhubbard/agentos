# sel4-ipc — seL4 IPC Wire Format Contract

**Version:** 1

## Overview

This contract defines the low-level IPC wire format used by every agentOS
service. All protection domains communicate using this layout so callers and
callees agree on byte positions without negotiation.

## Badge Encoding

A seL4 badge is a single 64-bit word minted into an endpoint capability at
the time the root task (or capability broker) grants it. agentOS packs three
fields into that word:

| Bits  | Field        | Purpose                                      |
|-------|--------------|----------------------------------------------|
| 63:48 | service_id   | Identifies which service owns the endpoint   |
| 47:32 | client_id    | Identifies which client PD holds this copy   |
| 31:0  | op_token     | Per-call nonce / operation context           |

The high bits carry the routing information so the kernel's badge delivery is
sufficient to authenticate the caller — no additional handshake is required.

## Message Layout (`sel4_msg_t`)

Each message is exactly 56 bytes: a 4-byte opcode, a 4-byte payload length,
and 48 bytes of payload (six 64-bit message registers). 56 bytes fits
comfortably inside seL4's IPC buffer, leaving room for capability transfer
metadata in the remaining MRs.

## Error Codes

Error codes are returned in the `opcode` field of a reply. `SEL4_ERR_OK`
(0) means success. Values 1–7 are generic agentOS errors. Service-specific
error codes begin at `0x0100`.

## Compatibility

All agentOS service contracts include this header and build on `sel4_msg_t`.
Version increments to this contract require updating every dependent contract.
