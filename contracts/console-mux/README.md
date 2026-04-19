# ConsoleMux — Serial Console Multiplexer Service Contract

## Overview

ConsoleMux arbitrates access to the physical UART (serial console) among all
protection domains in the system.  Without it, concurrent `printf`-style output
from multiple PDs would interleave and produce garbled output.

Key features:
- Up to 8 concurrent client PDs, each with a named channel identifier
- Zero-copy output via the `console_ring` shared memory region (16KB)
- Per-client flags: ANSI color, line prefix (channel_id), timestamps
- Serialized UART output in priority order
- Read path for interactive console input (keyboard / serial RX)

## Status

**PARTIALLY IMPLEMENTED.** The `console_mux` PD is defined in
`tools/topology.yaml` (priority 160, passive, maps `console_ring` at
vaddr 0x6000000).  The channels `ctrl_console`, `init_console`, and
`eb_console` are wired in the topology.

The current implementation uses `seL4_DebugPutChar` for serial output
without IPC mediation.  The full ring-based protocol defined in this
contract is planned for the next milestone.  New callers should program
against this contract so their code will work without modification once
the ring-based implementation lands.

## Protection Domain

`console_mux` is a passive PD (priority 160, higher than agentfs at 150)
in `tools/topology.yaml`.

| Caller | Channel |
|--------|---------|
| controller | ctrl_console (id_a=60, pp_a=true) |
| init_agent | init_console (id_a=3, pp_a=true) |
| event_bus  | eb_console (id_a=11) |

## IPC Endpoint

Clients PPC into console_mux via their dedicated channel.  The channel ID
from each caller's perspective:

- `controller`: channel 60
- `init_agent`: channel 3
- `event_bus`: channel 11 (notify only, no PPC)

## Operations

| Opcode | Value | Description |
|--------|-------|-------------|
| `CONSOLE_MUX_OP_OPEN`      | 0x800 | Register a console channel |
| `CONSOLE_MUX_OP_CLOSE`     | 0x801 | Deregister a console channel |
| `CONSOLE_MUX_OP_WRITE`     | 0x802 | Write bytes to console |
| `CONSOLE_MUX_OP_READ`      | 0x803 | Read pending input |
| `CONSOLE_MUX_OP_FLUSH`     | 0x804 | Flush pending output |
| `CONSOLE_MUX_OP_STATUS`    | 0x805 | Ring buffer statistics |
| `CONSOLE_MUX_OP_SET_FLAGS` | 0x806 | Update display flags |
| `CONSOLE_MUX_OP_HEALTH`    | 0x807 | Liveness probe |

## Error Codes

| Code | Value | Meaning |
|------|-------|---------|
| `CONSOLE_MUX_ERR_OK`          | 0  | Success |
| `CONSOLE_MUX_ERR_INVALID_ARG` | 1  | Null data or zero length |
| `CONSOLE_MUX_ERR_NOT_FOUND`   | 2  | client_handle not registered |
| `CONSOLE_MUX_ERR_FULL`        | 3  | Ring full — retry |
| `CONSOLE_MUX_ERR_MAX_CLIENTS` | 4  | Already at 8-client limit |
| `CONSOLE_MUX_ERR_TOO_BIG`     | 5  | Write exceeds 4096-byte limit |
| `CONSOLE_MUX_ERR_INTERNAL`    | 99 | Unexpected server error |

## Shared Memory: console_ring

The `console_ring` MR (16KB) is mapped into console_mux at vaddr 0x6000000
(`console_ring_vaddr`).  Clients place output data in the ring at
`data_ring_offset` before calling `CONSOLE_MUX_OP_WRITE`.  ConsoleMux reads
from `ring_offset` and writes to UART.

## Source Files

- `tools/topology.yaml` — console_mux PD and channel definitions
- `kernel/agentos-root-task/src/console_mux.c` — planned implementation
  (currently uses seL4_DebugPutChar as a fallback)
