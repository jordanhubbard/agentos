# agentOS Console API Contract

## Architecture

```
Browser (xterm.js)
    │ HTTP + WebSocket (port 8080)
    ▼
Node.js Console Server  ←── agentos_console.mjs
    │ HTTP (port 8789)
    ▼
agentOS HTTP Bridge  ◄── NOT YET IMPLEMENTED (see §4)
    │ seL4 IPC / shared memory
    ▼
console_mux PD  ←── console_mux.c
    │ ring buffers (16 × 4KB in shared memory)
    ▼
Individual PDs (controller, event_bus, init_agent, …)
```

---

## §1  Browser ↔ Node.js Console Server

All traffic on `ws://localhost:8080` and `http://localhost:8080`.

### 1.1  HTTP

| Method | Path | Description |
|--------|------|-------------|
| `GET`  | `/` | Dashboard HTML |
| `GET`  | `/health` | Console server health |
| `GET`  | `/api/agentos/slots` | Proxy to agentOS, or mock fallback |
| `GET`  | `/api/debug` | Raw probe of agentOS API (for debugging) |
| `GET`  | `/api/debug/serial` | Tail of `/tmp/agentos-serial.log` |
| `GET`  | `/api/agentos/profiler/snapshot` | Mock profiler data |

### 1.2  WebSocket — Log Bus (`ws://host/`)

Client → server messages:
```jsonc
{ "action": "subscribe",   "slot": 2 }
{ "action": "unsubscribe", "slot": 2 }
{ "action": "attach",      "slot": 2 }   // also POSTs attach to agentOS
{ "action": "list" }                      // returns all slot metadata
```

Server → client messages:
```jsonc
{ "event": "subscribed",   "slot": 2, "name": "init_agent" }
{ "event": "unsubscribed", "slot": 2 }
{ "event": "attached",     "slot": 2 }
{ "event": "slots",        "data": [ { "slot": 0, "name": "controller", "lines": 42, "active": true, "subscribers": 1 }, … ] }
{ "event": "error",        "msg": "…" }
{ "slot": 2, "line": "some log line\n" }  // streamed log lines
```

### 1.3  WebSocket — Terminal (`ws://host/terminal/:slot`)

- **Server → Client**: raw UTF-8 VT100/xterm bytes (terminal output)
- **Client → Server**: raw UTF-8 keyboard input bytes

Connection lifecycle:
1. On upgrade: server calls `POST /api/agentos/console/attach/{slot}` (non-fatal)
2. Server polls `GET /api/agentos/console/{slot}?cursor={n}` every 50 ms
3. New lines joined with `\r\n` and forwarded to client
4. Client keystrokes forwarded via `POST /api/agentos/console/inject/{slot}`
5. On WS close: polling stops

---

## §2  Node.js Console Server ↔ agentOS HTTP Bridge

The console server connects to `http://${AGENTOS_HOST}:${AGENTOS_API_PORT}` (default `127.0.0.1:8789`) using bearer token `AGENTOS_TOKEN`.

### 2.1  Slots

```
GET /api/agentos/slots
Authorization: Bearer <token>

200 OK
{
  "slots": [
    { "id": 0, "name": "controller", "active": true,  "lines": 1042 },
    { "id": 1, "name": "event_bus",  "active": true,  "lines": 287  },
    { "id": 2, "name": "init_agent", "active": false, "lines": 0    },
    …  (up to 16 entries, id 0-15)
  ]
}
```

`active` = the console ring for this PD has been registered (magic = 0xC0DE4D55).  
`lines` = total lines drained so far (maps to `lines_total` in `console_session_t`).

### 2.2  Console Read

```
GET /api/agentos/console/{slot}?cursor={n}
Authorization: Bearer <token>

200 OK
{
  "slot": 0,
  "cursor": 1042,          // new cursor value (= old cursor + lines returned)
  "lines": [               // lines[cursor..cursor+N] from the scrollback
    "[controller] boot complete\n",
    "[controller] Ready for agents.\n"
  ]
}
```

- `slot`: integer 0–15
- `cursor`: client's last-seen line count; omit or 0 to get all available scrollback
- Response `lines` must be in order, one complete log line per element
- Empty `lines` array (not an error) means no new output since cursor
- Scrollback depth: 64 lines per slot (see `MAX_SCROLLBACK_LINES` in console_mux.c)

### 2.3  Attach

```
POST /api/agentos/console/attach/{slot}
Authorization: Bearer <token>
Content-Type: application/json
{}

200 OK
{ "slot": 0, "ok": true }

409 Conflict
{ "error": "slot already attached" }

404 Not Found
{ "error": "no session for slot 0" }
```

Maps to `OP_CONSOLE_ATTACH (0x80)` PPC into console_mux with `MR1 = pd_id`.

### 2.4  Inject

```
POST /api/agentos/console/inject/{slot}
Authorization: Bearer <token>
Content-Type: application/json
{ "data": "ls -la\n" }

200 OK
{ "ok": true, "bytes": 7 }

422 Unprocessable
{ "error": "data must be a non-empty string, max 16 bytes" }
```

Maps to `OP_CONSOLE_INJECT (0x84)` PPC with up to 4 MRs of packed chars (16 bytes max per call).  
Larger inputs must be split into 16-byte chunks by the bridge.

### 2.5  Status

```
GET /api/agentos/console/status
Authorization: Bearer <token>

200 OK
{
  "active_pd": 0,           // currently attached PD (0xFFFFFFFF = none)
  "mode": 1,                // 0=single, 1=broadcast, 2=split
  "session_count": 5        // number of registered PD sessions
}
```

Maps to `OP_CONSOLE_STATUS (0x86)`.

### 2.6  Error Envelope

All errors:
```json
{ "error": "human-readable message" }
```

Standard HTTP status codes: 200, 400, 404, 409, 422, 500, 503.  
503 = agentOS is booting / console_mux not yet ready.

---

## §3  HTTP Bridge ↔ console_mux (seL4 IPC)

`console_mux` is a passive seL4 PD (priority 160). It exposes no network interface — only seL4 IPC.

### 3.1  Ring Buffer Layout (16 × 4KB = 64KB shared memory)

```
Offset 0 + (slot × 4096):
  [0..3]    magic  = 0xC0DE4D55  (ring is valid if this matches)
  [4..7]    pd_id  (which PD owns this ring)
  [8..11]   head   (write pointer, written by PD)
  [12..15]  tail   (read pointer, written by console_mux)
  [16..4095] circular byte buffer (4080 bytes)
```

The bridge must be mapped into the same shared memory region (`console_rings`) that console_mux sees. This requires the bridge PD (or Linux guest VM) to have a mapping of the `console_rings` memory region in `agentos.system`.

### 3.2  PPC Op Codes (MR0)

| Op | Code | MR args | MR return |
|----|------|---------|-----------|
| ATTACH   | 0x80 | MR1=pd_id | MR0=0 (ok) or 1 (err) |
| DETACH   | 0x81 | — | MR0=0 |
| LIST     | 0x82 | — | MR0=0, MR1=bitmask of active pd_ids |
| MODE     | 0x83 | MR1=mode (0/1/2) | MR0=0 or 1 |
| INJECT   | 0x84 | MR1..MR4=packed chars (≤16 bytes) | MR0=0 |
| SCROLL   | 0x85 | MR1=num_lines | MR0=0 |
| STATUS   | 0x86 | — | MR1=active_pd, MR2=mode, MR3=session_count |
| WRITE    | 0x87 | MR1=slot, MR2=pd_id | MR0=0 (flush ring) |

PPC channel: **60** (controller ↔ console_mux in `agentos.system`).  
The bridge must be granted PPC rights on the same channel, or a new dedicated channel.

---

## §4  Bridge Implementation Gap

**Current state (2026-04-03):**

- Port 8789 accepts TCP (QEMU NAT forwards it) but immediately resets — nothing listens in the Linux guest
- `controller` PD crashes in `init()` → first `console_log` call → `microkit_ppcall(CH_CONSOLE_MUX=60)` fails with "invalid channel" (see serial log)
- Crash PC `0x9095e29095e29095` is a repeating 4-byte pattern: stack return-address corruption from undefined PPC behavior on invalid channel

**Immediate workaround (implemented in agentos_console.mjs):**
- Serial log (`/tmp/agentos-serial.log`) contains all PD output via UART
- Parse tagged lines `[pd_name] text` to reconstruct per-slot streams
- Serve as slot API without needing the seL4 bridge

**Bridge implementation path (in priority order):**

1. **Fix controller crash first** — channel 60 definition in `agentos.system` must match the `CH_CONSOLE_MUX` constant in `monitor.c`/`agentos.h`. The first `console_log()` in `init()` should not PPC if console_mux isn't ready yet — gate it behind a `consolemux_ready` flag or use `microkit_dbg_puts` for early boot.

2. **Linux guest bridge** — Add a memory-mapped virtio shared-memory device to `linux_vmm` config that maps the `console_rings` region into the Linux guest address space. Run a small C program in Buildroot that:
   - Polls the rings (or uses eventfd/mmap)
   - Maintains per-slot scrollback buffers
   - Serves the HTTP API above on port 8789

3. **New agentOS system PD** (longer term) — A dedicated `http_bridge` PD that has both a PPC channel to `console_mux` and a virtio-net NIC, serving the HTTP API entirely from within seL4 without the Linux guest dependency.

---

## §5  Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `CONSOLE_PORT` | 8080 | Node.js console server port |
| `AGENTOS_HOST` | `127.0.0.1` | agentOS bridge host |
| `AGENTOS_API_PORT` | 8789 | agentOS bridge port |
| `AGENTOS_TOKEN` | (hardcoded dev token) | Bearer token for bridge auth |
| `SERIAL_LOG` | `/tmp/agentos-serial.log` | QEMU serial output path |
