# agentOS CC Consumer Pattern

This document describes how external tools — remote desktop clients, web UIs,
mobile apps, monitoring dashboards — interact with the agentOS
Command-and-Control Protection Domain (`cc_pd`).

**Target audience:** Developers building on top of agentOS who need to observe
or control guest VMs, subscribe to framebuffer output, inject input, or query
system state.  You do not need to read any kernel source to implement a
compliant consumer.

---

## Architecture Overview

```
External Consumer
    │
    │  (mesh agent protocol or agentctl bridge)
    ▼
cc_pd  (CH_CC_PD = 72)     ← your single entry point
    │
    ├── vibe_engine         (guest lifecycle)
    ├── guest_pd            (input injection)
    ├── framebuffer_pd      (frame data)
    ├── serial_pd           (serial device status)
    ├── net_pd              (network device status)
    ├── block_pd            (block device status)
    ├── usb_pd              (USB device status)
    ├── agent_pool          (polecat pool status)
    └── log_drain           (log stream)
```

`cc_pd` is a **pure relay** — it contains no policy and no state beyond session
tracking.  Every MSG_CC_* call is dispatched to the correct service PD and the
result is returned to you.  You never call service PDs directly.

---

## 1. Channel Discovery and Transport

### Channel ID

`cc_pd` listens on channel **72** (`CH_CC_PD`).  This is a fixed assignment in
the agentOS system manifest; it does not change at runtime.

### Transport

`cc_pd` speaks seL4 Protected Procedure Calls (PPCs) via the Microkit runtime.
External processes that are not native seL4 PDs access `cc_pd` through one of
two paths:

| Access path | When to use |
|---|---|
| **agentctl** bridge | Command-line tools, scripts, off-host clients (SSH, web UIs) |
| **Mesh agent** PPC | On-host agents running inside the seL4 capability system |

Both paths expose identical IPC semantics: the same opcodes, Message Register
(MR) layout, and shared memory protocol described in this document.  The
transport wrapper (agentctl socket vs. raw PPC) is transparent.

### Message Register Layout

Microkit PPCs pass arguments in Message Registers (MR0–MR3):

- **MR0** always contains the opcode on the way in; on return it contains the
  status (CC_OK or a CC_ERR_* code) unless the opcode documents otherwise.
- **MR1–MR3** carry per-opcode parameters and return values as documented in
  the message catalog below.

### Shared Memory

`cc_pd` maps one caller-facing shared memory region (`cc_shmem`).  Its address
is not visible to external consumers — the agentctl bridge or mesh runtime
handles it transparently.  What matters to you:

- **Before calling MSG_CC_SEND**: write your command payload into `cc_shmem`,
  starting at offset 0.  Maximum size: **4096 bytes** (`CC_MAX_CMD_BYTES`).
- **After calling MSG_CC_RECV**: read the response from `cc_shmem` offset 0.
  Maximum size: **4096 bytes** (`CC_MAX_RESP_BYTES`).
- **After calling MSG_CC_LIST_GUESTS / LIST_DEVICES / LIST**: the response
  array is in `cc_shmem` immediately — no RECV step needed.
- **After calling MSG_CC_GUEST_STATUS / DEVICE_STATUS**: the status struct is
  in `cc_shmem` immediately.

---

## 2. Session Management

Every external caller must establish a session before issuing relay calls.
Sessions isolate callers and carry inactivity timers.

### Limits

| Constant | Value | Meaning |
|---|---|---|
| `CC_MAX_SESSIONS` | 8 | Maximum concurrent sessions |
| `CC_SESSION_TIMEOUT_TICKS` | 5000 | Idle expiry (~50 s at 100 Hz) |

### Session States

| Constant | Value | Meaning |
|---|---|---|
| `CC_SESSION_STATE_CONNECTED` | 0 | Active session |
| `CC_SESSION_STATE_IDLE` | 1 | Connected but quiescent |
| `CC_SESSION_STATE_BUSY` | 2 | Processing a command |
| `CC_SESSION_STATE_EXPIRED` | 3 | Timed out; must reconnect |

### MSG_CC_CONNECT — Open a Session

```
Opcode:  0x2601  (MSG_CC_CONNECT)
MR0 in:  MSG_CC_CONNECT
MR1 in:  client_badge   — caller's seL4 badge / capability identifier
         flags           — CC_CONNECT_FLAG_JSON (1<<0) or CC_CONNECT_FLAG_BINARY (1<<1)
                           (pack badge in low 16 bits, flags in high 16 bits, or use
                            cc_req_connect struct layout: badge first, flags second)

MR0 out: CC_OK (0) on success; CC_ERR_NO_SESSIONS (1) if limit reached
MR1 out: session_id (0–7) on success
```

The `session_id` returned here must accompany all subsequent calls.

### MSG_CC_DISCONNECT — Close a Session

```
Opcode:  0x2602  (MSG_CC_DISCONNECT)
MR1 in:  session_id

MR0 out: CC_OK or CC_ERR_BAD_SESSION
```

Always disconnect when done.  Abandoned sessions expire after
`CC_SESSION_TIMEOUT_TICKS` of inactivity, consuming a slot.

### MSG_CC_STATUS — Query Session State

```
Opcode:  0x2605  (MSG_CC_STATUS)
MR1 in:  session_id

MR0 out: CC_OK or CC_ERR_BAD_SESSION
MR1 out: state  (CC_SESSION_STATE_*)
MR2 out: pending_responses  (always 0 in relay model)
MR3 out: ticks_since_active
```

### MSG_CC_LIST — List All Sessions

```
Opcode:  0x2606  (MSG_CC_LIST)
(no MR arguments)

MR0 out: count of active sessions
shmem:   cc_session_info_t[count] — see cc_contract.h
```

The `cc_session_info_t` struct written to shmem:

```c
typedef struct __attribute__((packed)) {
    uint32_t session_id;
    uint32_t state;              /* CC_SESSION_STATE_* */
    uint32_t client_badge;
    uint32_t ticks_since_active;
} cc_session_info_t;
```

---

## 3. Message Catalog

### Query: Guest List

#### MSG_CC_LIST_GUESTS — Enumerate Guest VMs

```
Opcode:  0x2607  (MSG_CC_LIST_GUESTS)
MR1 in:  max_entries  — upper bound on results

MR0 out: count of guest entries written to shmem
shmem:   cc_guest_info_t[count]
```

```c
typedef struct __attribute__((packed)) {
    uint32_t guest_handle;   /* vibeos handle — use in subsequent calls */
    uint32_t state;          /* GUEST_STATE_* from guest_contract.h */
    uint32_t os_type;        /* VIBEOS_TYPE_* from vibeos_contract.h */
    uint32_t arch;           /* VIBEOS_ARCH_* (may be 0 if not reported) */
} cc_guest_info_t;
```

#### MSG_CC_GUEST_STATUS — Detailed Status of One Guest

```
Opcode:  0x260A  (MSG_CC_GUEST_STATUS)
MR1 in:  guest_handle

MR0 out: CC_OK or CC_ERR_RELAY_FAULT
shmem:   cc_guest_status_t  (on CC_OK)
```

```c
typedef struct __attribute__((packed)) {
    uint32_t guest_handle;
    uint32_t state;          /* GUEST_STATE_* */
    uint32_t os_type;
    uint32_t arch;           /* may be 0 */
    uint32_t device_flags;   /* VIBEOS_DEV_* bitmask of bound devices (may be 0) */
    uint32_t _reserved[3];
} cc_guest_status_t;
```

### Query: Device List

#### MSG_CC_LIST_DEVICES — Enumerate Devices by Type

```
Opcode:  0x2608  (MSG_CC_LIST_DEVICES)
MR1 in:  dev_type  (CC_DEV_TYPE_*)
MR2 in:  max_entries

MR0 out: count on success; CC_ERR_BAD_DEV_TYPE if dev_type invalid
shmem:   cc_device_info_t[count]
```

Device type constants:

| Constant | Value | Routes to |
|---|---|---|
| `CC_DEV_TYPE_SERIAL` | 0 | serial_pd |
| `CC_DEV_TYPE_NET` | 1 | net_pd |
| `CC_DEV_TYPE_BLOCK` | 2 | block_pd |
| `CC_DEV_TYPE_USB` | 3 | usb_pd |
| `CC_DEV_TYPE_FB` | 4 | (no bulk list; returns count=0) |

```c
typedef struct __attribute__((packed)) {
    uint32_t dev_type;    /* CC_DEV_TYPE_* */
    uint32_t dev_handle;  /* opaque handle, use in MSG_CC_DEVICE_STATUS */
    uint32_t state;       /* device-specific state word */
    uint32_t _reserved;
} cc_device_info_t;
```

#### MSG_CC_DEVICE_STATUS — Status of One Device

```
Opcode:  0x260B  (MSG_CC_DEVICE_STATUS)
MR1 in:  dev_type   (CC_DEV_TYPE_*)
MR2 in:  dev_handle (from cc_device_info_t.dev_handle)

MR0 out: CC_OK or CC_ERR_BAD_DEV_TYPE / CC_ERR_RELAY_FAULT
shmem:   raw status words from the device PD (uint32_t[3])
```

The shmem layout for device status is three `uint32_t` words from the
downstream PD response (MR0–MR2 of the PD's reply).  Interpretation is
device-specific; consult the relevant device contract header.

### Query: Agent Pool

#### MSG_CC_LIST_POLECATS — Worker Pool Counts

```
Opcode:  0x2609  (MSG_CC_LIST_POLECATS)
(no MR arguments beyond opcode)

MR0 out: CC_OK
MR1 out: total  — total polecats configured
MR2 out: busy   — polecats currently processing work
MR3 out: idle   — polecats waiting for work
```

### Action: Framebuffer

#### MSG_CC_ATTACH_FRAMEBUFFER — Subscribe to Frame Events

See Section 4 for the full subscription model.

```
Opcode:  0x260C  (MSG_CC_ATTACH_FRAMEBUFFER)
MR1 in:  guest_handle  — the guest that owns the framebuffer
MR2 in:  fb_handle     — framebuffer handle (from MSG_FB_CREATE)

MR0 out: CC_OK or CC_ERR_RELAY_FAULT (if fb_handle is invalid)
MR1 out: frame_seq     — most recent frame sequence number (on CC_OK)
```

### Action: Input Injection

#### MSG_CC_SEND_INPUT — Inject a HID Event

See Section 5 for the full input event format.

```
Opcode:  0x260D  (MSG_CC_SEND_INPUT)
MR1 in:  guest_handle  — target guest VM
shmem:   cc_input_event_t  (caller writes before the call)

MR0 out: CC_OK or CC_ERR_RELAY_FAULT
```

### Action: Snapshots

#### MSG_CC_SNAPSHOT — Snapshot a Guest VM

```
Opcode:  0x260E  (MSG_CC_SNAPSHOT)
MR1 in:  guest_handle

MR0 out: CC_OK or CC_ERR_RELAY_FAULT
MR1 out: snap_lo  — snapshot ID low 32 bits
MR2 out: snap_hi  — snapshot ID high 32 bits
```

Snapshot IDs are 64-bit values split across two MRs.  Store both halves to
pass them to MSG_CC_RESTORE.

#### MSG_CC_RESTORE — Restore a Guest VM from Snapshot

```
Opcode:  0x260F  (MSG_CC_RESTORE)
MR1 in:  guest_handle
MR2 in:  snap_lo  — snapshot ID low 32 bits
MR3 in:  snap_hi  — snapshot ID high 32 bits

MR0 out: CC_OK or CC_ERR_RELAY_FAULT
```

### Action: Log Streaming

#### MSG_CC_LOG_STREAM — Flush a Log Ring

```
Opcode:  0x2610  (MSG_CC_LOG_STREAM)
MR1 in:  slot   — log ring slot index (0 .. MAX_LOG_RINGS-1)
MR2 in:  pd_id  — TRACE_PD_* identifier for the source PD

MR0 out: CC_OK or CC_ERR_RELAY_FAULT
MR1 out: bytes_drained  — bytes consumed from the ring
```

---

## 4. Framebuffer Subscription Model

Subscribing to framebuffer output is a three-phase protocol:

```
Phase 1: Attach
  Consumer calls MSG_CC_ATTACH_FRAMEBUFFER(guest_handle, fb_handle)
  → cc_pd probes framebuffer_pd to validate fb_handle
  → Returns CC_OK + frame_seq (latest frame sequence number)
  → Consumer is registered for EVENT_FB_FRAME_READY via EventBus

Phase 2: Receive frame events (repeated)
  framebuffer_pd flips a new frame
  → EventBus delivers EVENT_FB_FRAME_READY to all subscribers
       MR1 = fb_handle
       MR2 = frame_seq  (monotonically increasing)
  → Consumer reads pixel data from the shared fb_shmem region

Phase 3: Process pixels
  Consumer reads the raw framebuffer from fb_shmem.
  Format, width, and height are fixed at fb_handle creation time.
  frame_seq lets the consumer detect dropped frames (gap in sequence).
```

### Getting a Framebuffer Handle

Framebuffer handles are created by `framebuffer_pd` via `MSG_FB_CREATE`.
Consumers that do not create framebuffers themselves obtain the handle by:

1. Calling `MSG_CC_LIST_GUESTS` to find a guest with a bound framebuffer
   (check `device_flags` in `cc_guest_status_t` for `VIBEOS_DEV_FB`).
2. Calling `MSG_CC_GUEST_STATUS` for the guest handle.
3. Using the `fb_handle` from the guest status, or obtaining it via a
   higher-level API (e.g., agentctl `fb list`).

### Frame Event Constants

```c
#define MSG_FB_FRAME_READY   0x2506  /* EventBus event kind */
#define EVENT_FB_FRAME_READY 0x40u   /* MR1=handle, MR2=frame_seq */
```

The EventBus notification arrives in your `notified()` handler (or via the
agentctl push channel if you are an off-host client).

### Shared Memory Access

After receiving `EVENT_FB_FRAME_READY`, read pixel data from the
`fb_shmem` region that was mapped to your process when you subscribed.
The region is read-only from the consumer's perspective.  Do not hold a
reference to the pixel data across the next `EVENT_FB_FRAME_READY` — the
region may be overwritten by the next flip.

---

## 5. Input Injection

To inject input into a guest VM, write a `cc_input_event_t` into `cc_shmem`
and call `MSG_CC_SEND_INPUT`.

### cc_input_event_t Layout

```c
typedef struct __attribute__((packed)) {
    uint32_t event_type;   /* CC_INPUT_* constant */
    uint32_t keycode;      /* HID usage code (key events; 0 for mouse events) */
    int32_t  dx;           /* relative X delta (CC_INPUT_MOUSE_MOVE only) */
    int32_t  dy;           /* relative Y delta (CC_INPUT_MOUSE_MOVE only) */
    uint32_t btn_mask;     /* button bitmask (CC_INPUT_MOUSE_BTN only) */
    uint32_t _reserved;    /* must be 0 */
} cc_input_event_t;
```

### Event Types

| Constant | Value | Meaning | Active fields |
|---|---|---|---|
| `CC_INPUT_KEY_DOWN` | 0x01 | Key pressed | `keycode` |
| `CC_INPUT_KEY_UP` | 0x02 | Key released | `keycode` |
| `CC_INPUT_MOUSE_MOVE` | 0x03 | Relative mouse movement | `dx`, `dy` |
| `CC_INPUT_MOUSE_BTN` | 0x04 | Mouse button press/release | `btn_mask` |

`keycode` follows USB HID Usage Table values (Usage Page 0x07 for keyboard).

### Input Sequencing

- One event per call.  For smooth mouse movement, send one `CC_INPUT_MOUSE_MOVE`
  per polling cycle rather than accumulating deltas.
- Key repeat is the consumer's responsibility.  Send `CC_INPUT_KEY_DOWN` on
  press, `CC_INPUT_KEY_UP` on release; do not hold down without repeating.
- `dx` and `dy` are signed 32-bit integers in device units.  No scaling is
  applied by `cc_pd` or `guest_pd`.

---

## 6. Error Codes

All MSG_CC_* calls return one of these values in MR0:

| Constant | Value | Meaning |
|---|---|---|
| `CC_OK` | 0 | Success |
| `CC_ERR_NO_SESSIONS` | 1 | Session limit reached (`CC_MAX_SESSIONS = 8`) |
| `CC_ERR_BAD_SESSION` | 2 | `session_id` is invalid or not owned by caller |
| `CC_ERR_EXPIRED` | 3 | Session timed out; call MSG_CC_CONNECT again |
| `CC_ERR_CMD_TOO_LARGE` | 4 | Command payload exceeds `CC_MAX_CMD_BYTES` (4096) |
| `CC_ERR_NO_RESPONSE` | 5 | MSG_CC_RECV called with no pending response |
| `CC_ERR_BAD_HANDLE` | 6 | `guest_handle` or `dev_handle` does not exist |
| `CC_ERR_BAD_DEV_TYPE` | 7 | `dev_type` is not a valid `CC_DEV_TYPE_*` value |
| `CC_ERR_RELAY_FAULT` | 8 | Downstream service PD returned an error |

On `CC_ERR_RELAY_FAULT` the downstream PD's raw error is not surfaced.
Retry once; if the fault persists, log the opcode and `guest_handle`/`dev_handle`
and treat it as a transient service failure.

---

## 7. Worked Example: Minimal Remote Desktop Client

The following pseudocode implements a complete, spec-compliant remote desktop
consumer.  It connects, enumerates guests, attaches to the first guest's
framebuffer, streams frames, and injects keyboard input.

```pseudocode
# ── Setup ───────────────────────────────────────────────────────────────────

conn = cc_open_channel(CH_CC_PD)          # obtain IPC channel to cc_pd

# Establish session
reply = ppc(conn, MSG_CC_CONNECT, badge=MY_BADGE, flags=CC_CONNECT_FLAG_BINARY)
assert reply.MR0 == CC_OK, "no sessions available"
session_id = reply.MR1

# ── Enumerate guests ─────────────────────────────────────────────────────────

reply = ppc(conn, MSG_CC_LIST_GUESTS, max_entries=16)
count = reply.MR0
guests = read_shmem(cc_guest_info_t, count)   # array of cc_guest_info_t from shmem

assert count > 0, "no guests"
target = guests[0]   # pick first guest; filter by .state == GUEST_STATE_RUNNING

# ── Attach framebuffer ───────────────────────────────────────────────────────

# Get detailed status to obtain fb_handle
reply = ppc(conn, MSG_CC_GUEST_STATUS, guest_handle=target.guest_handle)
assert reply.MR0 == CC_OK
status = read_shmem_struct(cc_guest_status_t)    # cc_guest_status_t from shmem
assert status.device_flags & VIBEOS_DEV_FB, "guest has no framebuffer"

fb_handle = status.fb_handle    # (sourced from guest status or higher-level API)

reply = ppc(conn, MSG_CC_ATTACH_FRAMEBUFFER,
            guest_handle=target.guest_handle, fb_handle=fb_handle)
assert reply.MR0 == CC_OK, "invalid framebuffer handle"
last_frame_seq = reply.MR1       # most recent frame seq; track for drop detection

# ── Main loop ────────────────────────────────────────────────────────────────

while running:

    # Wait for EVENT_FB_FRAME_READY from EventBus
    event = wait_event(EVENT_FB_FRAME_READY)    # MR1=handle, MR2=frame_seq

    if event.MR1 != fb_handle:
        continue                  # not our framebuffer

    frame_seq = event.MR2
    if frame_seq != last_frame_seq + 1:
        log_warning(f"dropped {frame_seq - last_frame_seq - 1} frame(s)")
    last_frame_seq = frame_seq

    # Read pixel data from shared fb_shmem — valid until next flip
    pixels = read_fb_shmem()
    render_frame(pixels)

    # Inject any pending input
    for ev in pending_input_events():
        write_shmem(cc_input_event_t(
            event_type = ev.type,      # CC_INPUT_KEY_DOWN / UP / MOUSE_MOVE / BTN
            keycode    = ev.keycode,   # HID usage code (keys); 0 for mouse events
            dx         = ev.dx,        # relative X (mouse move only)
            dy         = ev.dy,        # relative Y (mouse move only)
            btn_mask   = ev.btns,      # button bitmask (mouse btn only)
            _reserved  = 0,
        ))
        reply = ppc(conn, MSG_CC_SEND_INPUT, guest_handle=target.guest_handle)
        assert reply.MR0 == CC_OK

# ── Teardown ─────────────────────────────────────────────────────────────────

ppc(conn, MSG_CC_DISCONNECT, session_id=session_id)
cc_close_channel(conn)
```

---

## 8. What MUST NOT Be Done

These patterns violate the agentOS security model and will break across
future versions without notice.

### No Direct Service PD Access

External consumers **must not** call service PDs directly.  All access goes
through `cc_pd`:

```
WRONG:  ppc(CH_VIBE_ENGINE, MSG_VIBEOS_LIST, ...)  ← capability violation
RIGHT:  ppc(CH_CC_PD,       MSG_CC_LIST_GUESTS, ...)
```

`cc_pd` holds the capabilities to service PDs.  External callers cannot
obtain those capabilities without violating the seL4 capability model.

### No kernel/ Code in Consumer Projects

Consumer projects **must not** include or import headers from
`kernel/agentos-root-task/`.  The only public API surface is:

- `cc_contract.h` — opcodes, request/reply structs, error codes
- The IPC protocol described in this document

Internal headers (`vibeos_contract.h`, `guest_contract.h`,
`framebuffer_contract.h`, etc.) are kernel-private.  Their layouts may
change in any release.  If a constant from those headers is needed in a
consumer (e.g., `GUEST_STATE_*`), it must be re-declared in the consumer
with a comment referencing the source, and a compatibility test must be
written.

### No Persistent Shmem References

Do not hold pointers into `cc_shmem` or `fb_shmem` across IPC calls.  The
shared memory regions are valid only within the current IPC turn.  After the
next call to `cc_pd`, the content may have changed.

### No Session Sharing Across Threads

Each session is bound to the seL4 channel (badge) that created it.
`cc_pd` validates ownership on every call (`valid_session` checks both the
session ID and the calling channel).  Do not pass `session_id` across threads
or processes; create one session per independent caller.

### No Polling for Frame Events

Do not poll `MSG_CC_STATUS` or a framebuffer handle in a tight loop to detect
new frames.  Subscribe via `MSG_CC_ATTACH_FRAMEBUFFER` and wait for
`EVENT_FB_FRAME_READY`.  Polling burns IPC budget and degrades system
throughput.
