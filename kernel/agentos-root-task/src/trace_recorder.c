/*
 * agentOS Trace Recorder Protection Domain
 *
 * Receives inter-PD dispatch events from the controller via fast
 * microkit_notify() signals (CH_TRACE_NOTIFY, local id=1).  PPC channel
 * (local id=0) handles START/STOP/QUERY/DUMP ops.
 *
 * Circular buffer stores up to TRACE_BUF_SIZE packed trace entries.
 * Each entry records: timestamp_ns, from_pd, to_pd, channel, opcode.
 *
 * Controller packs the notify word as:
 *   MR0 = (from_pd << 24) | (to_pd << 16) | label[15:0]
 *
 * On OP_TRACE_DUMP the recorder serialises all buffered entries into
 * the trace_out shared-memory region (trace_out_vaddr) as a flat array
 * of TraceEntry structs.  The caller reads MR1 = byte count written.
 *
 * Channel assignments (from trace_recorder's perspective):
 *   id=0: receives PPC from controller (START/STOP/QUERY/DUMP)
 *   id=1: receives notify from controller (one per inter-PD dispatch)
 *
 * Priority: 128 (interactive) — below real-time PDs, above workers.
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"

/* ── Channel IDs (trace_recorder's local view) ───────────────────────────── */
#define CH_CTRL_PPC    0   /* controller PPCs in for START/STOP/QUERY/DUMP */
#define CH_CTRL_NOTIFY 1   /* controller notifies on each dispatch event   */

/* ── Configuration ───────────────────────────────────────────────────────── */
#define TRACE_BUF_SIZE  512u   /* max entries in circular buffer */

/* ── Shared-memory output region (set by Microkit linker) ────────────────── */
uintptr_t trace_out_vaddr;   /* mapped output region for OP_TRACE_DUMP      */
#define TRACE_OUT_SIZE  0x10000u  /* 64KB — matches agentos.system MR size  */

/* ── Trace entry (on-wire layout written to shmem) ───────────────────────── */
typedef struct __attribute__((packed)) {
    uint64_t timestamp_ns;  /* approximate ns since boot (tick counter × 1000) */
    uint8_t  from_pd;       /* source PD numeric ID (TRACE_PD_*) */
    uint8_t  to_pd;         /* destination PD numeric ID (TRACE_PD_*) */
    uint8_t  channel;       /* microkit channel number (low byte of label) */
    uint8_t  _pad;
    uint16_t opcode;        /* IPC label / opcode */
    uint16_t seq_lo;        /* low 16 bits of entry sequence number */
} TraceEntry;               /* 16 bytes */

_Static_assert(sizeof(TraceEntry) == 16, "TraceEntry must be 16 bytes");

/* ── Circular buffer ─────────────────────────────────────────────────────── */
static TraceEntry  trace_buf[TRACE_BUF_SIZE];
static uint32_t    buf_head   = 0;   /* next write index (wraps mod TRACE_BUF_SIZE) */
static uint32_t    buf_count  = 0;   /* number of valid entries (capped at TRACE_BUF_SIZE) */
static uint64_t    seq        = 0;   /* monotonic event sequence number */
static uint64_t    tick_ns    = 0;   /* approximate timestamp (ns); incremented each notify */
static bool        recording  = false;
static uint32_t    overflow_count = 0;  /* events dropped while buf_count == TRACE_BUF_SIZE */

/* ── Helpers ─────────────────────────────────────────────────────────────── */

/* Append one entry to the circular buffer. */
static void buf_append(uint8_t from_pd, uint8_t to_pd,
                        uint8_t channel,  uint16_t opcode)
{
    if (buf_count == TRACE_BUF_SIZE) {
        /* Buffer full — overwrite oldest entry (true ring) */
        overflow_count++;
    } else {
        buf_count++;
    }

    TraceEntry *e = &trace_buf[buf_head % TRACE_BUF_SIZE];
    e->timestamp_ns = tick_ns;
    e->from_pd      = from_pd;
    e->to_pd        = to_pd;
    e->channel      = channel;
    e->_pad         = 0;
    e->opcode       = opcode;
    e->seq_lo       = (uint16_t)(seq & 0xFFFFu);

    buf_head = (buf_head + 1u) % TRACE_BUF_SIZE;
    seq++;
}

/*
 * Read entries from the circular buffer in chronological order into dst.
 * Returns number of entries copied (min of buf_count and max_out).
 */
static uint32_t buf_read_ordered(TraceEntry *dst, uint32_t max_out)
{
    uint32_t n = buf_count < max_out ? buf_count : max_out;
    /* Oldest entry is at (buf_head - buf_count + TRACE_BUF_SIZE) % TRACE_BUF_SIZE */
    uint32_t start = (buf_head + TRACE_BUF_SIZE - buf_count) % TRACE_BUF_SIZE;

    for (uint32_t i = 0; i < n; i++) {
        dst[i] = trace_buf[(start + i) % TRACE_BUF_SIZE];
    }
    return n;
}

/* ── Microkit entry points ───────────────────────────────────────────────── */

void init(void)
{
    buf_head      = 0;
    buf_count     = 0;
    seq           = 0;
    tick_ns       = 0;
    recording     = true;   /* begin recording immediately */
    overflow_count = 0;

    microkit_dbg_puts("[trace_recorder] init: 512-entry ring, output region ");
    microkit_dbg_puts(trace_out_vaddr ? "mapped\n" : "NOT MAPPED\n");
}

/*
 * notified() — one notify per inter-PD dispatch from controller.
 * MR0 = (from_pd << 24) | (to_pd << 16) | label[15:0]
 */
void notified(microkit_channel ch)
{
    /* Advance approximate timestamp regardless of recording state */
    tick_ns += 1000u;   /* 1 µs per notify tick (rough approximation) */

    if (ch == CH_CTRL_NOTIFY && recording) {
        uint32_t word    = (uint32_t)microkit_mr_get(0);
        uint8_t  from_pd = (uint8_t)((word >> 24) & 0xFFu);
        uint8_t  to_pd   = (uint8_t)((word >> 16) & 0xFFu);
        uint16_t label   = (uint16_t)(word & 0xFFFFu);
        uint8_t  channel = (uint8_t)(label & 0xFFu);

        buf_append(from_pd, to_pd, channel, label);
    }
}

/*
 * protected() — handles START/STOP/QUERY/DUMP from controller.
 */
microkit_msginfo protected(microkit_channel ch, microkit_msginfo msg)
{
    (void)ch;
    uint32_t op = (uint32_t)microkit_mr_get(0);

    switch (op) {

    /* ── OP_TRACE_START: begin/resume recording; reset buffer ───────────── */
    case OP_TRACE_START:
        buf_head      = 0;
        buf_count     = 0;
        seq           = 0;
        overflow_count = 0;
        recording     = true;
        microkit_dbg_puts("[trace_recorder] START: buffer reset, recording on\n");
        microkit_mr_set(0, 0u);
        return microkit_msginfo_new(0, 1);

    /* ── OP_TRACE_STOP: stop recording; buffer contents preserved ───────── */
    case OP_TRACE_STOP:
        recording = false;
        microkit_dbg_puts("[trace_recorder] STOP: recording paused\n");
        microkit_mr_set(0, 0u);
        microkit_mr_set(1, (uint32_t)(seq & 0xFFFFFFFFu));  /* total events seen */
        return microkit_msginfo_new(0, 2);

    /* ── OP_TRACE_QUERY: return live buffer statistics ───────────────────── */
    case OP_TRACE_QUERY:
        /*
         * MR0 = 0 (success)
         * MR1 = event_count (entries currently in buffer)
         * MR2 = bytes_used  (event_count × sizeof(TraceEntry))
         * MR3 = overflow_count (events dropped due to buffer full)
         * MR4 = recording state (1 = on, 0 = off)
         */
        microkit_mr_set(0, 0u);
        microkit_mr_set(1, buf_count);
        microkit_mr_set(2, buf_count * (uint32_t)sizeof(TraceEntry));
        microkit_mr_set(3, overflow_count);
        microkit_mr_set(4, recording ? 1u : 0u);
        return microkit_msginfo_new(0, 5);

    /* ── OP_TRACE_DUMP: serialise buffer to trace_out shmem region ───────── */
    case OP_TRACE_DUMP: {
        if (!trace_out_vaddr) {
            /* Output region not mapped — return error */
            microkit_mr_set(0, 0xFFu);
            microkit_mr_set(1, 0u);
            return microkit_msginfo_new(0, 2);
        }

        /* How many entries fit in the output region? */
        uint32_t max_entries = TRACE_OUT_SIZE / (uint32_t)sizeof(TraceEntry);
        TraceEntry *out = (TraceEntry *)(uintptr_t)trace_out_vaddr;

        uint32_t n = buf_read_ordered(out, max_entries);
        uint32_t bytes = n * (uint32_t)sizeof(TraceEntry);

        /*
         * MR0 = 0 (success)
         * MR1 = bytes written to trace_out region
         * MR2 = number of entries written
         * MR3 = overflow_count (informational: entries dropped since last start)
         */
        microkit_mr_set(0, 0u);
        microkit_mr_set(1, bytes);
        microkit_mr_set(2, n);
        microkit_mr_set(3, overflow_count);
        return microkit_msginfo_new(0, 4);
    }

    default:
        microkit_mr_set(0, 0xFFu);
        return microkit_msginfo_new(0, 1);
    }

    (void)msg;
}
