/*
 * Command-and-Control (CC) PD IPC Contract — Phase 5a
 *
 * The CC PD is a pure IPC relay and multiplexer.  External agents (via mesh
 * or agentctl) use MSG_CC_* opcodes to query and control the agentOS stack.
 * cc_pd routes each call to the appropriate service PD (serial_pd, net_pd,
 * block_pd, usb_pd, framebuffer_pd, vibe_engine, guest_pd, agent_pool,
 * log_drain) and forwards the result.  cc_pd contains ZERO policy.
 *
 * Channel: CH_CC_PD (see agentos.h)
 * Opcodes: MSG_CC_* (see agentos.h)
 *
 * The CC PD is NOT a UI.  It emits structured responses (binary or
 * JSON-encoded) that external tools parse.  No interactive sessions,
 * no terminal semantics.
 *
 * Session protocol (session management):
 *   - MSG_CC_CONNECT assigns a session_id; subsequent calls use it.
 *   - MSG_CC_SEND validates and accepts an opaque command in cc_shmem.
 *   - MSG_CC_RECV retrieves the queued response from cc_shmem.
 *   - Current implementation queues an empty ACK response; service routing
 *     uses the direct relay API below.
 *   - Sessions expire after CC_SESSION_TIMEOUT_TICKS of inactivity.
 *   - Multiple concurrent sessions are supported (CC_MAX_SESSIONS).
 *   - The CC PD does not initiate communication; it only responds to PPCs.
 *
 * Direct relay API (Phase 5a):
 *   MSG_CC_LIST_GUESTS       → vibe_engine (MSG_VIBEOS_LIST)
 *   MSG_CC_LIST_DEVICES      → device PD selected by dev_type
 *   MSG_CC_LIST_POLECATS     → agent_pool (MSG_AGENTPOOL_STATUS)
 *   MSG_CC_GUEST_STATUS      → vibe_engine (MSG_VIBEOS_STATUS)
 *   MSG_CC_DEVICE_STATUS     → device PD selected by dev_type
 *   MSG_CC_ATTACH_FRAMEBUFFER→ framebuffer_pd (MSG_FB_FLIP handle validation)
 *   MSG_CC_SEND_INPUT        → guest_pd (MSG_GUEST_SEND_INPUT)
 *   MSG_CC_SNAPSHOT          → vibe_engine (MSG_VIBEOS_SNAPSHOT)
 *   MSG_CC_RESTORE           → vibe_engine (MSG_VIBEOS_RESTORE)
 *   MSG_CC_LOG_STREAM        → log_drain (OP_LOG_WRITE)
 *   MSG_CC_CREATE_GUEST      → vibe_engine (MSG_VIBEOS_CREATE)
 *   MSG_CC_FAULT_INJECT      → fault_inject (OP_FAULT_INJECT)
 *   MSG_CC_SUSPEND_GUEST     → vibe_engine (MSG_VIBEOS_SUSPEND) or boot guest_pd
 *   MSG_CC_RESUME_GUEST      → vibe_engine (MSG_VIBEOS_RESUME) or boot guest_pd
 *   MSG_CC_DESTROY_GUEST     → vibe_engine (MSG_VIBEOS_DESTROY) or boot guest_pd
 *   MSG_CC_TRACE_START       → trace_recorder (OP_TRACE_START)
 *   MSG_CC_TRACE_STOP        → trace_recorder (OP_TRACE_STOP)
 *   MSG_CC_TRACE_QUERY       → trace_recorder (OP_TRACE_QUERY)
 *   MSG_CC_TRACE_DUMP        → trace_recorder (OP_TRACE_DUMP)
 *
 * Invariants:
 *   - cc_pd relays MR arguments verbatim; it does not interpret payload.
 *   - Shmem data is copied between caller shmem and downstream PD shmem.
 *   - All routing decisions are purely based on the opcode and dev_type field.
 *   - cc_pd returns CC_ERR_RELAY_FAULT if the downstream PPC fails.
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs ────────────────────────────────────────────────────────── */
#define CC_PD_CH_CONTROLLER  CH_CC_PD

/* ─── Configuration ──────────────────────────────────────────────────────── */
#define CC_MAX_SESSIONS         8u
#define CC_SESSION_TIMEOUT_TICKS  5000u  /* ~50 seconds at 100 Hz */
#define CC_MAX_CMD_BYTES        4096u
#define CC_MAX_RESP_BYTES       4096u

/*
 * Root-task → cc_pd startup ABI for the host VirtIO-serial transport.
 * Device-visible physical addresses and cc_pd CPU virtual addresses are
 * deliberately distinct; DMA addresses must never be dereferenced as pointers.
 */
#define CC_VIRTIO_STARTUP_MAGIC   0x43435651u /* "CCVQ" */
#define CC_VIRTIO_STARTUP_VERSION 1u
#define CC_VIRTIO_MMIO_VA         0x10002000UL
#define CC_VIRTIO_STARTUP_VA      0x10003000UL
#define CC_VIRTIO_QUEUE_VA        0x10006000UL
#define CC_VIRTIO_TX_BUFFER_VA    0x10007000UL
#define CC_VIRTIO_RX_BUFFER_VA    0x10008000UL

typedef struct __attribute__((packed)) cc_virtio_startup {
    uint32_t magic;
    uint32_t version;
    uint64_t queue_pa;
    uint64_t tx_buffer_pa;
    uint64_t rx_buffer_pa;
} cc_virtio_startup_t;

/* ─── Command types ──────────────────────────────────────────────────────── */

#define CC_CMD_TYPE_QUERY   0x01u   /* read-only status query */
#define CC_CMD_TYPE_ACTION  0x02u   /* mutating action */
#define CC_CMD_TYPE_STREAM  0x03u   /* subscribe to event stream (future) */

/* ─── Session state ──────────────────────────────────────────────────────── */

#define CC_SESSION_STATE_CONNECTED  0
#define CC_SESSION_STATE_IDLE       1
#define CC_SESSION_STATE_BUSY       2
#define CC_SESSION_STATE_EXPIRED    3

/* ─── Request structs ────────────────────────────────────────────────────── */

struct cc_req_connect {
    uint32_t client_badge;      /* caller's seL4 badge / capability identifier */
    uint32_t flags;             /* CC_CONNECT_FLAG_* */
};

#define CC_CONNECT_FLAG_JSON   (1u << 0)  /* prefer JSON-encoded responses */
#define CC_CONNECT_FLAG_BINARY (1u << 1)  /* prefer binary-encoded responses */

struct cc_req_disconnect {
    uint32_t session_id;
};

struct cc_req_send {
    uint32_t session_id;
    uint32_t cmd_type;          /* CC_CMD_TYPE_* */
    uint32_t len;               /* command bytes in cc_shmem */
};

struct cc_req_recv {
    uint32_t session_id;
    uint32_t max;               /* max response bytes to return */
};

struct cc_req_status {
    uint32_t session_id;
};

struct cc_req_list {
    uint32_t max_entries;
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

struct cc_reply_connect {
    uint32_t ok;
    uint32_t session_id;
};

struct cc_reply_disconnect {
    uint32_t ok;
};

struct cc_reply_send {
    uint32_t ok;
    uint32_t resp_pending;      /* 1 = response ready for MSG_CC_RECV */
};

struct cc_reply_recv {
    uint32_t ok;
    uint32_t len;               /* response bytes in cc_shmem */
};

struct cc_reply_status {
    uint32_t ok;
    uint32_t state;             /* CC_SESSION_STATE_* */
    uint32_t pending_responses; /* number of queued responses */
    uint32_t ticks_since_active;
};

struct cc_reply_list {
    uint32_t count;             /* cc_session_info_t entries written to shmem */
};

/* ─── Shmem layout: session info entry ──────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t session_id;
    uint32_t state;
    uint32_t client_badge;
    uint32_t ticks_since_active;
} cc_session_info_t;

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum cc_error {
    CC_OK                   = 0,
    CC_ERR_NO_SESSIONS      = 1,  /* CC_MAX_SESSIONS reached */
    CC_ERR_BAD_SESSION      = 2,
    CC_ERR_EXPIRED          = 3,
    CC_ERR_CMD_TOO_LARGE    = 4,
    CC_ERR_NO_RESPONSE      = 5,  /* RECV with no pending response */
    CC_ERR_BAD_HANDLE       = 6,  /* guest_handle / dev_handle invalid */
    CC_ERR_BAD_DEV_TYPE     = 7,  /* dev_type not one of CC_DEV_TYPE_* */
    CC_ERR_RELAY_FAULT      = 8,  /* downstream PPC returned error */
};

/* ─── Device type constants (mirrors GUEST_DEV_* from guest_contract.h) ─── */

#define CC_DEV_TYPE_SERIAL  0u
#define CC_DEV_TYPE_NET     1u
#define CC_DEV_TYPE_BLOCK   2u
#define CC_DEV_TYPE_USB     3u
#define CC_DEV_TYPE_FB      4u
#define CC_DEV_TYPE_COUNT   5u

/* ─── Input event types ──────────────────────────────────────────────────── */

#define CC_INPUT_KEY_DOWN   0x01u  /* key pressed */
#define CC_INPUT_KEY_UP     0x02u  /* key released */
#define CC_INPUT_MOUSE_MOVE 0x03u  /* relative mouse movement */
#define CC_INPUT_MOUSE_BTN  0x04u  /* mouse button press/release */
#define CC_INPUT_TEXT       0x05u  /* keycode=length; UTF-8 bytes follow event */
#define CC_INPUT_TEXT_MAX   20u   /* fits every 48-byte relay: handle + event + text */

/* ─── Shmem layout: guest info entry (MSG_CC_LIST_GUESTS) ───────────────── */

typedef struct __attribute__((packed)) {
    uint32_t guest_handle;     /* vibeos handle */
    uint32_t state;            /* GUEST_STATE_* from guest_contract.h */
    uint32_t os_type;          /* VIBEOS_TYPE_* from vibeos_contract.h */
    uint32_t arch;             /* VIBEOS_ARCH_* from vibeos_contract.h */
} cc_guest_info_t;

/* ─── Shmem layout: guest status (MSG_CC_GUEST_STATUS) ──────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t guest_handle;
    uint32_t state;            /* GUEST_STATE_* */
    uint32_t os_type;
    uint32_t arch;
    uint32_t device_flags;     /* VIBEOS_DEV_* bitmask of bound devices */
    uint32_t _reserved[3];
} cc_guest_status_t;

/* ─── Shmem layout: device info entry (MSG_CC_LIST_DEVICES) ─────────────── */

typedef struct __attribute__((packed)) {
    uint32_t dev_type;         /* CC_DEV_TYPE_* */
    uint32_t dev_handle;       /* opaque handle from device PD */
    uint32_t state;            /* device-specific state word */
    uint32_t _reserved;
} cc_device_info_t;

/* ─── Shmem layout: input event (MSG_CC_SEND_INPUT / MSG_GUEST_SEND_INPUT) */

typedef struct __attribute__((packed)) {
    uint32_t event_type;       /* CC_INPUT_* */
    uint32_t keycode;          /* HID usage code, or text byte length */
    int32_t  dx;               /* relative X (mouse move) */
    int32_t  dy;               /* relative Y (mouse move) */
    uint32_t btn_mask;         /* button bitmask (mouse button event) */
    uint32_t _reserved;
} cc_input_event_t;
/* CC_INPUT_TEXT appends keycode bytes immediately after cc_input_event_t. */

/* ─── MSG_CC_LIST_GUESTS ─────────────────────────────────────────────────── */

struct cc_req_list_guests {
    uint32_t max_entries;
};

struct cc_reply_list_guests {
    uint32_t count;            /* cc_guest_info_t entries written to shmem */
};

/* ─── MSG_CC_LIST_DEVICES ────────────────────────────────────────────────── */

struct cc_req_list_devices {
    uint32_t dev_type;         /* CC_DEV_TYPE_* */
    uint32_t max_entries;
};

struct cc_reply_list_devices {
    uint32_t count;            /* cc_device_info_t entries written to shmem */
};

/* ─── MSG_CC_LIST_POLECATS ───────────────────────────────────────────────── */

struct cc_reply_list_polecats {
    uint32_t ok;
    uint32_t total;
    uint32_t busy;
    uint32_t idle;
};

/* ─── MSG_CC_GUEST_STATUS ────────────────────────────────────────────────── */

struct cc_req_guest_status {
    uint32_t guest_handle;
};

struct cc_reply_guest_status {
    uint32_t ok;               /* CC_OK on success; cc_guest_status_t in shmem */
};

/* ─── MSG_CC_DEVICE_STATUS ───────────────────────────────────────────────── */

struct cc_req_device_status {
    uint32_t dev_type;         /* CC_DEV_TYPE_* */
    uint32_t dev_handle;
};

struct cc_reply_device_status {
    uint32_t ok;               /* CC_OK on success; device-specific status in shmem */
};

/* ─── MSG_CC_ATTACH_FRAMEBUFFER ──────────────────────────────────────────── */

/*
 * Subscribe the calling session to MSG_FB_FRAME_READY events from the
 * framebuffer identified by fb_handle, which must belong to guest_handle.
 * cc_pd relays to framebuffer_pd to confirm the handle exists, then
 * registers the caller for EVENT_FB_FRAME_READY notifications via EventBus.
 */

struct cc_req_attach_framebuffer {
    uint32_t guest_handle;
    uint32_t fb_handle;        /* handle from MSG_FB_CREATE */
};

struct cc_reply_attach_framebuffer {
    uint32_t ok;               /* CC_OK on success */
    uint32_t frame_seq;        /* most recent frame sequence number */
};

/* ─── MSG_CC_SEND_INPUT ──────────────────────────────────────────────────── */

struct cc_req_send_input {
    uint32_t guest_handle;     /* target guest; cc_input_event_t in cc_shmem */
};

struct cc_reply_send_input {
    uint32_t ok;
};

/* ─── MSG_CC_SNAPSHOT ────────────────────────────────────────────────────── */

struct cc_req_snapshot {
    uint32_t guest_handle;     /* vibeos handle to snapshot */
};

struct cc_reply_snapshot {
    uint32_t ok;
    uint32_t snap_lo;          /* snapshot ID low 32 bits */
    uint32_t snap_hi;          /* snapshot ID high 32 bits */
};

/* ─── MSG_CC_RESTORE ─────────────────────────────────────────────────────── */

struct cc_req_restore {
    uint32_t guest_handle;
    uint32_t snap_lo;
    uint32_t snap_hi;
};

struct cc_reply_restore {
    uint32_t ok;
};

/* ─── MSG_CC_LOG_STREAM ──────────────────────────────────────────────────── */

/*
 * Drain a guest's serial output as ASCII bytes (agentos-vsi).  cc_pd resolves
 * (slot, pd_id) to a concrete guest console and returns the drained bytes in
 * cc_shmem with the byte count in bytes_drained.
 *
 * Slot model:
 *   - slot 0 + pd_id == TRACE_PD_CONTROLLER : boot guest serial stream.
 *   - pd_id == TRACE_PD_LINUX_VMM / TRACE_PD_FREEBSD_VMM : a vibe_engine guest.
 *     On first use, pass the vibe guest handle in `slot`; cc_pd assigns the
 *     guest its own log slot (1..N) and returns that slot in `log_slot`.
 *   - slot N>0 : re-address a previously assigned vibe guest stream directly.
 *
 * Each guest is independently addressable: the boot guest owns slot 0 and each
 * vibe guest owns a distinct slot for the lifetime of cc_pd's slot table.
 */

struct cc_req_log_stream {
    uint32_t slot;             /* 0 = boot guest; else vibe handle / assigned slot */
    uint32_t pd_id;            /* TRACE_PD_* identifier selecting the drain backend */
};

struct cc_reply_log_stream {
    uint32_t ok;
    uint32_t bytes_drained;    /* ASCII bytes written to cc_shmem */
    uint32_t log_slot;         /* slot this stream resolved to (echo / first-assign) */
};

/* ─── MSG_CC_CREATE_GUEST ───────────────────────────────────────────────── */

/*
 * Create a new guest OS instance.  The request shmem contains a
 * vibeos_create_req.  cc_pd relays it to VibeOS unchanged and returns the
 * created guest handle in MR1 when MR0 == CC_OK.
 */

struct cc_req_create_guest {
    uint32_t max_reply_bytes;   /* reserved; set to 0 for binary callers */
};

struct cc_reply_create_guest {
    uint32_t ok;
    uint32_t guest_handle;
};

/* ─── MSG_CC_FAULT_INJECT ───────────────────────────────────────────────── */

struct cc_req_fault_inject {
    uint32_t slot_id;
    uint32_t fault_kind;
    uint32_t flags;
};

struct cc_reply_fault_inject {
    uint32_t ok;
    uint32_t result;
    uint32_t ticks_to_recovery;
    uint32_t trace_event_id;
};

/* ─── MSG_CC_SUSPEND_GUEST / MSG_CC_RESUME_GUEST ───────────────────────── */

struct cc_req_suspend_guest {
    uint32_t guest_handle;
};

struct cc_reply_suspend_guest {
    uint32_t ok;
    uint32_t state;            /* GUEST_STATE_SUSPENDED on success */
};

struct cc_req_resume_guest {
    uint32_t guest_handle;
};

struct cc_reply_resume_guest {
    uint32_t ok;
    uint32_t state;            /* GUEST_STATE_RUNNING on success */
};

/* ─── MSG_CC_DESTROY_GUEST ──────────────────────────────────────────────── */

struct cc_req_destroy_guest {
    uint32_t guest_handle;
    uint32_t reason;           /* GUEST_DESTROY_* reason code */
};

struct cc_reply_destroy_guest {
    uint32_t ok;
};

/* ─── MSG_CC_TRACE_* ────────────────────────────────────────────────────── */

/*
 * Trace relay bridge for external consumers.  The stable CC wire surface
 * returns fixed-size trace entries in cc_shmem so host tools do not need
 * direct access to TraceRecorder's internal output mapping.
 */

#define CC_TRACE_FLAG_WRAP   (1u << 0)
#define CC_TRACE_FLAG_JSONL  (1u << 1)  /* accepted for compatibility */

typedef struct __attribute__((packed)) {
    uint64_t timestamp_ns;
    uint8_t  from_pd;          /* TRACE_PD_* source */
    uint8_t  to_pd;            /* TRACE_PD_* destination */
    uint8_t  channel;          /* low 8 bits of channel / transport */
    uint8_t  _pad;
    uint16_t opcode;           /* low 16 bits of message opcode */
    uint16_t seq_lo;           /* low 16 bits of recorder sequence */
} cc_trace_entry_t;

#define CC_TRACE_ENTRY_SIZE   16u
#define CC_TRACE_MAX_ENTRIES  (CC_MAX_RESP_BYTES / CC_TRACE_ENTRY_SIZE)

struct cc_req_trace_start {
    uint32_t flags;            /* CC_TRACE_FLAG_* */
};

struct cc_reply_trace_start {
    uint32_t ok;
};

struct cc_reply_trace_stop {
    uint32_t ok;
    uint32_t event_count;      /* total events observed since START */
};

struct cc_reply_trace_query {
    uint32_t ok;
    uint32_t event_count;      /* entries currently available */
    uint32_t bytes_used;       /* event_count * sizeof(cc_trace_entry_t) */
    uint32_t overflow_count;   /* events dropped due to ring overflow */
};

struct cc_req_trace_dump {
    uint32_t max_events;       /* 0 means as many as fit */
};

struct cc_reply_trace_dump {
    uint32_t ok;
    uint32_t events_written;   /* cc_trace_entry_t entries in cc_shmem */
    uint32_t bytes_written;
    uint32_t overflow_count;
};
