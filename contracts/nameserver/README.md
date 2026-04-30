# nameserver — Capability Registry Contract

**Version:** 1

## Overview

The nameserver is a Ring-2 system service PD that maintains the authoritative
registry of service endpoint capabilities. It is the single point through
which protection domains discover each other at runtime without hardcoded
capability slot addresses.

## Boot-Time Registration (OP_NS_REGISTER)

During system initialisation each service PD sends `OP_NS_REGISTER` to the
nameserver, passing its human-readable `service_name` and the root-task-
relative cap slot of its IPC endpoint. The nameserver:

1. Validates the caller's badge (`client_id` must match a slot authorised by
   the root task to perform registration).
2. Copies the endpoint capability into its own CSpace.
3. Assigns a unique `service_id` (16-bit, monotonically increasing).
4. Returns `ns_reply_t` with `error = SEL4_ERR_OK` and the assigned
   `service_id`.

## Runtime Lookup (OP_NS_LOOKUP)

A client PD that needs to call a service sends `OP_NS_LOOKUP` with the target
`service_name`. The nameserver:

1. Locates the registered endpoint for that name.
2. Mints a new badged copy of the endpoint capability with the caller's
   `client_id` in the badge high bits.
3. Copies the minted cap into a pre-agreed slot in the caller's CSpace (the
   slot index is returned in `ns_reply_t.ep_cap_slot`).
4. Returns the `service_id` so the caller can construct correct badges for
   subsequent calls.

## Revocation (OP_NS_REVOKE)

A service may revoke all minted copies of its endpoint (e.g., before
shutdown) by sending `OP_NS_REVOKE`. The nameserver calls `seL4_CNode_Revoke`
on the stored capability, invalidating every derived copy atomically.

## ACL Model (Future Extension)

The current version grants any registered PD the right to look up any service.
A future version will allow services to supply an allowlist of `client_id`
values permitted to call `OP_NS_LOOKUP` against them. The `ns_request_t`
struct has sufficient reserved space to carry this information without a wire-
format break; a version bump will be issued when the ACL extension lands.

## Wire Format

All messages use `sel4_msg_t` from `contracts/sel4-ipc/interface.h`.
`ns_request_t` and `ns_reply_t` are placed in the `data[48]` payload field.
