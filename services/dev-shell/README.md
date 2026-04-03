# dev-shell agentOS console service

agentOS console integration for the agentOS `dev_shell` PD.

## Overview

The `dev_shell` PD exposes an interactive debug REPL accessible from the agentOS console
dashboard over UART / serial ring buffer.  This service bridges the agentOS console HTTP
API to the dev_shell shared memory ring buffer inside QEMU.

Enable dev_shell at build time:

```sh
make dev-shell          # build with -DAGENTOS_DEV_SHELL
make demo               # launch QEMU
```

## Shared memory protocol

The `dev_shell_rings` memory region (4KB, mapped at `0xC000000` in the PD):

| Offset      | Field       | Description                              |
|-------------|-------------|------------------------------------------|
| `0x000`     | `in_buf`    | 256-byte null-terminated command string  |
| `0x100`     | `in_ready`  | `1` = new command pending, `0` = consumed |
| `0x200`     | `out_buf`   | 1024-byte null-terminated response       |
| `0x600`     | `out_ready` | `1` = response ready, `0` = consumed     |

**Write a command (controller side):**
1. Write null-terminated command string to `in_buf`
2. Set `in_ready = 1`
3. Call `microkit_notify(CH_DEV_SHELL)` (channel 80 from controller perspective)

**Read a response (controller side):**
1. Poll `out_ready` until `1`
2. Copy `out_buf`, print via `printf` / UART
3. Clear `out_ready = 0`

## agentOS console API endpoints (stub — not yet implemented)

### `GET /api/agentos/shell`

Server-Sent Events stream.  The agentOS console backend polls `out_ready` every 50 ms;
when set, it reads `out_buf`, emits a `data:` SSE event, and clears the flag.

```
Content-Type: text/event-stream
Cache-Control: no-cache

data: agentOS dev_shell v0.1 commands:\r\n  help\r\n  ...

data: PD list:\r\n  [0] controller prio=50\r\n  ...
```

### `POST /api/agentos/shell/cmd`

Send a command to dev_shell.

**Request body** (`application/json`):
```json
{ "cmd": "pd list" }
```

**Response** (`application/json`):
```json
{ "status": "ok" }
```

The backend writes `cmd` to `in_buf`, sets `in_ready = 1`, and sends a
`microkit_notify` to the dev_shell PD via the QEMU control socket.

## Supported commands

| Command                        | Description                                      |
|--------------------------------|--------------------------------------------------|
| `help`                         | List all commands                                |
| `pd list`                      | List known PD IDs and priorities                 |
| `pd stat <id>`                 | Query PD status                                  |
| `mem dump <offset> <len>`      | Hex dump of dev_shell_rings MR (max 256 bytes)   |
| `ipc send <ch> <op> <arg>`     | Send raw PPC to a channel (dev/test only)        |
| `trace dump`                   | Print last 8 trace events from local ring        |
| `fault inject <slot> <type>`   | Queue fault injection (type: `vm`, `cap`, `null`)|
| `version`                      | Print `agentOS dev_shell v0.1`                   |
| `quit`                         | No-op (shell stays running)                      |

## Implementation notes

- Full agentOS console backend integration requires a QEMU QMP / monitor socket bridge to
  write shared memory and trigger notifications.
- For local QEMU testing, the controller PD can be patched to read commands
  from its debug UART input and forward them via the ring buffer.
- Channel 80 in `agentos.system`: `controller(id=80) <-> dev_shell(id=0)`.
