# SerialMux — Serial/UART Console Multiplexer Service Contract

## Overview

SerialMux is the canonical serial console multiplexer for agentOS.  It
arbitrates access to the debug UART (and optionally guest UART devices) so
that multiple PDs can share a single physical serial port without interleaving
output.  Every guest OS and VMM must use this service for console I/O rather
than driving UART hardware directly.

This contract covers the IPC protocol between PD clients and the serial-mux
protection domain.

## Status

**LEGACY.**  The old `console_mux` UI-facing service has been removed from
the active build.  New serial I/O work should use the generic serial PD
contract in `kernel/agentos-root-task/include/contracts/serial_contract.h`.

## Protection Domain

SerialMux no longer maps an active PD in the default system description.

## IPC Endpoint

Clients receive a PPC capability to the serial-mux endpoint at guest OS
creation time (via `vm_manager.c`).  The capability badge encodes the
assigned session slot ID.

| Caller | Channel |
|--------|---------|
| controller | ctrl_console (ch 60, pp_a=true) |
| init_agent | init_console (ch 3, pp_a=true) |
| event_bus  | eb_console (ch 11) |

## Operations

| Opcode | Value | Description |
|--------|-------|-------------|
| `SERIAL_MUX_OP_OPEN`     | 0x80 | Open a session; returns slot_id |
| `SERIAL_MUX_OP_CLOSE`    | 0x81 | Close session; flush pending output |
| `SERIAL_MUX_OP_WRITE`    | 0x82 | Write bytes from ring head |
| `SERIAL_MUX_OP_READ`     | 0x83 | Read pending input into ring |
| `SERIAL_MUX_OP_FLUSH`    | 0x84 | Wait for output ring to drain |
| `SERIAL_MUX_OP_SET_MODE` | 0x85 | Set display mode (single/broadcast/split) |
| `SERIAL_MUX_OP_STATUS`   | 0x86 | Ring and session statistics |

## Display Modes

| Constant | Value | Behaviour |
|----------|-------|-----------|
| `SERIAL_MUX_MODE_SINGLE`    | 0 | Show only the attached session |
| `SERIAL_MUX_MODE_BROADCAST` | 1 | Show all sessions, line-tagged |
| `SERIAL_MUX_MODE_SPLIT`     | 2 | Show up to 4 sessions, labelled |

## Session Ring Layout

Each session has a 4096-byte ring in the `console_ring` MR:

```
[0  .. 3 ]  magic   = SERIAL_MUX_RING_MAGIC (0xC0DE4D55)
[4  .. 7 ]  pd_id
[8  .. 11]  head    (write offset, updated by client)
[12 .. 15]  tail    (read offset, updated by serial-mux)
[16 .. 4095] character ring buffer (4080 bytes)
```

## Source Files

- `contracts/serial-mux/interface.h` — legacy contract reference
- `kernel/agentos-root-task/include/contracts/serial_contract.h` — active serial PD contract
