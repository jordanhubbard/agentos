/*
 * agentOS trace_recorder — seL4 IPC trace capture for integration testing + QEMU replay
 *
 * Passive PD, priority 90 (lowest of all PDs, observe-only).
 *
 * Captures inter-PD message sequences (src/dst/label/MRs) into a 512KB packed
 * shadow buffer.  A 1MB output region holds the serialized JSONL dump for host
 * extraction (QEMU virtio-mmio, GDB memread, or integration test harness).
 *
 * Protocol (controller PPCs in, MR0 = op code):
 *   OP_TRACE_START (0x80) — begin recording; reset buffer; MR0→0x00 on OK
 *   OP_TRACE_STOP  (0x81) — stop recording; finalize header; MR0→0x00 on OK
 *   OP_TRACE_QUERY (0x82) — query state; MR0=event_count, MR1=bytes_used
 *   OP_TRACE_DUMP  (0x83) — serialize buffer to JSONL in trace_out region
 *                            MR0→0x00 OK, MR1=bytes_written
 *
 * Trace event injection (controller notifies on CH_NOTIFY, id=1):
 *   Before notifying, controller sets:
 *     MR0 = (src_pd << 24) | (dst_pd << 16) | label[15:0]
 *     MR1 = mr0 value from the original message
 *     MR2 = mr1 value from the original message
 *
 * Shadow buffer layout (trace_buf, 512KB):
 *   [0..3]   uint32  magic   = 0xA6E71ACE
 *   [4..7]   uint32  version = 1
 *   [8..11]  uint32  count   — number of valid events
 *   [12..15] uint32  flags   — TRACE_FLAG_RECORDING (bit 0), TRACE_FLAG_OVERFLOW (bit 1)
 *   [64..]   TraceEvent[]    — packed 20-byte records
 *
 * TraceEvent (20 bytes, packed):
 *   uint64_t tick     — global monotonic event counter
 *   uint8_t  src_pd   — source PD numeric ID
 *   uint8_t  dst_pd   — destination PD numeric ID
 *   uint16_t label    — IPC message label / opcode
 *   uint32_t mr0      — message register 0
 *   uint32_t mr1      — message register 1
 *
 * Buffer capacity: (512*1024 - 64) / 20 = 26,195 events before overflow flag.
 *
 * JSONL output format (one line per event):
 *   {"tick":N,"src":"controller","dst":"watchdog_pd","label":"0x51","mr0":0,"mr1":0}
 *
 * Channels (from trace_recorder perspective):
 *   id=0  CH_IN:     controller PPCs in (OP_TRACE_START/STOP/QUERY/DUMP)
 *   id=1  CH_NOTIFY: controller notifies with packed event data in MRs
 *
 * Shared memory:
 *   trace_buf (512KB) — packed binary event buffer, this PD rw
 *   trace_out (1MB)   — JSONL output region, written on OP_TRACE_DUMP
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"

/* ── Opcodes ──────────────────────────────────────────────────────────── */
#define OP_TRACE_START  0x80u
#define OP_TRACE_STOP   0x81u
#define OP_TRACE_QUERY  0x82u
#define OP_TRACE_DUMP   0x83u

/* ── Channel local IDs ───────────────────────────────────────────────── */
#define CH_IN      0   /* controller PPCs in for control ops */
#define CH_NOTIFY  1   /* controller notifies with event data in MRs */

/* ── Buffer layout constants ─────────────────────────────────────────── */
#define TRACE_BUF_SIZE    0x80000u  /* 512KB shadow buffer */
#define TRACE_OUT_SIZE    0x100000u /* 1MB JSONL output region */
#define TRACE_HDR_SIZE    64u       /* reserved header bytes */

#define TRACE_MAGIC       0xA6E71ACEu
#define TRACE_VERSION     1u

#define TRACE_FLAG_RECORDING  (1u << 0)
#define TRACE_FLAG_OVERFLOW   (1u << 1)

/* ── TraceEvent: 20 bytes packed ─────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint64_t tick;     /* monotonic event counter */
    uint8_t  src_pd;   /* source PD numeric ID */
    uint8_t  dst_pd;   /* destination PD numeric ID */
    uint16_t label;    /* IPC label / opcode */
    uint32_t mr0;      /* message register 0 */
    uint32_t mr1;      /* message register 1 */
} TraceEvent;

#define TRACE_EVENT_SIZE   ((uint32_t)sizeof(TraceEvent))  /* 20 */
#define TRACE_MAX_EVENTS   ((TRACE_BUF_SIZE - TRACE_HDR_SIZE) / TRACE_EVENT_SIZE)

/* ── Shared memory base addresses (patched by Microkit setvar) ───────── */
uintptr_t trace_buf_vaddr;
uintptr_t trace_out_vaddr;

#define TRACE_BUF  ((volatile uint8_t *)trace_buf_vaddr)
#define TRACE_OUT  ((volatile uint8_t *)trace_out_vaddr)

/* ── State ───────────────────────────────────────────────────────────── */
static uint64_t global_tick = 0;

/* ── Header accessors (little-endian 32-bit stores) ──────────────────── */
static void hdr_write_u32(uint32_t off, uint32_t v) {
    volatile uint8_t *b = TRACE_BUF + off;
    b[0] = (uint8_t)(v);
    b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16);
    b[3] = (uint8_t)(v >> 24);
}

static uint32_t hdr_read_u32(uint32_t off) {
    volatile uint8_t *b = TRACE_BUF + off;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

/* ── Append a TraceEvent to the binary buffer ────────────────────────── */
static void buf_append(uint8_t src_pd, uint8_t dst_pd, uint16_t label,
                        uint32_t mr0, uint32_t mr1) {
    uint32_t flags = hdr_read_u32(12);
    if (!(flags & TRACE_FLAG_RECORDING)) return;

    uint32_t count = hdr_read_u32(8);
    if (count >= TRACE_MAX_EVENTS) {
        /* Mark overflow; keep recording (wrap on overflow) */
        hdr_write_u32(12, flags | TRACE_FLAG_OVERFLOW);
        count = 0;  /* wrap around */
    }

    uint32_t offset = TRACE_HDR_SIZE + count * TRACE_EVENT_SIZE;
    volatile uint8_t *ev = TRACE_BUF + offset;

    uint64_t t = global_tick++;

    /* Write tick (8 bytes LE) */
    ev[0]  = (uint8_t)(t);
    ev[1]  = (uint8_t)(t >> 8);
    ev[2]  = (uint8_t)(t >> 16);
    ev[3]  = (uint8_t)(t >> 24);
    ev[4]  = (uint8_t)(t >> 32);
    ev[5]  = (uint8_t)(t >> 40);
    ev[6]  = (uint8_t)(t >> 48);
    ev[7]  = (uint8_t)(t >> 56);
    /* src_pd, dst_pd, label */
    ev[8]  = src_pd;
    ev[9]  = dst_pd;
    ev[10] = (uint8_t)(label);
    ev[11] = (uint8_t)(label >> 8);
    /* mr0 */
    ev[12] = (uint8_t)(mr0);
    ev[13] = (uint8_t)(mr0 >> 8);
    ev[14] = (uint8_t)(mr0 >> 16);
    ev[15] = (uint8_t)(mr0 >> 24);
    /* mr1 */
    ev[16] = (uint8_t)(mr1);
    ev[17] = (uint8_t)(mr1 >> 8);
    ev[18] = (uint8_t)(mr1 >> 16);
    ev[19] = (uint8_t)(mr1 >> 24);

    hdr_write_u32(8, count + 1);
}

/* ── PD name table ───────────────────────────────────────────────────── */
/*
 * Numeric PD IDs (packed into src_pd / dst_pd fields).
 * Must stay in sync with TRACE_PD_* constants in agentos.h.
 */
#define PDID_CONTROLLER    0
#define PDID_EVENT_BUS     1
#define PDID_INIT_AGENT    2
#define PDID_WORKER_0      3
#define PDID_WORKER_1      4
#define PDID_WORKER_2      5
#define PDID_WORKER_3      6
#define PDID_WORKER_4      7
#define PDID_WORKER_5      8
#define PDID_WORKER_6      9
#define PDID_WORKER_7     10
#define PDID_AGENTFS      11
#define PDID_VIBE_ENGINE  12
#define PDID_SWAP_SLOT_0  13
#define PDID_SWAP_SLOT_1  14
#define PDID_SWAP_SLOT_2  15
#define PDID_SWAP_SLOT_3  16
#define PDID_GPU_SCHED    17
#define PDID_MESH_AGENT   18
#define PDID_CAP_AUDIT    19
#define PDID_FAULT_HDL    20
#define PDID_DEBUG_BRIDGE 21
#define PDID_QUOTA_PD     22
#define PDID_MEM_PROFILER 23
#define PDID_WATCHDOG_PD  24
#define PDID_TRACE_REC    25
#define PDID_UNKNOWN      255

static const char *pd_name(uint8_t id) {
    switch (id) {
    case PDID_CONTROLLER:   return "controller";
    case PDID_EVENT_BUS:    return "event_bus";
    case PDID_INIT_AGENT:   return "init_agent";
    case PDID_WORKER_0:     return "worker_0";
    case PDID_WORKER_1:     return "worker_1";
    case PDID_WORKER_2:     return "worker_2";
    case PDID_WORKER_3:     return "worker_3";
    case PDID_WORKER_4:     return "worker_4";
    case PDID_WORKER_5:     return "worker_5";
    case PDID_WORKER_6:     return "worker_6";
    case PDID_WORKER_7:     return "worker_7";
    case PDID_AGENTFS:      return "agentfs";
    case PDID_VIBE_ENGINE:  return "vibe_engine";
    case PDID_SWAP_SLOT_0:  return "swap_slot_0";
    case PDID_SWAP_SLOT_1:  return "swap_slot_1";
    case PDID_SWAP_SLOT_2:  return "swap_slot_2";
    case PDID_SWAP_SLOT_3:  return "swap_slot_3";
    case PDID_GPU_SCHED:    return "gpu_sched";
    case PDID_MESH_AGENT:   return "mesh_agent";
    case PDID_CAP_AUDIT:    return "cap_audit_log";
    case PDID_FAULT_HDL:    return "fault_handler";
    case PDID_DEBUG_BRIDGE: return "debug_bridge";
    case PDID_QUOTA_PD:     return "quota_pd";
    case PDID_MEM_PROFILER: return "mem_profiler";
    case PDID_WATCHDOG_PD:  return "watchdog_pd";
    case PDID_TRACE_REC:    return "trace_recorder";
    default:                return "unknown";
    }
}

/* ── JSONL serialisation helpers ─────────────────────────────────────── */
static uint32_t out_put_str(uint32_t p, const char *s) {
    while (*s && p < TRACE_OUT_SIZE - 1)
        TRACE_OUT[p++] = (uint8_t)*s++;
    return p;
}

static uint32_t out_put_u64(uint32_t p, uint64_t v) {
    char tmp[22]; int i = 21;
    tmp[i] = '\0';
    if (v == 0) { tmp[--i] = '0'; }
    else { while (v && i > 0) { tmp[--i] = (char)('0' + (uint8_t)(v % 10)); v /= 10; } }
    return out_put_str(p, &tmp[i]);
}

static uint32_t out_put_u32(uint32_t p, uint32_t v) {
    return out_put_u64(p, (uint64_t)v);
}

/* Write "0xNN" hex for label */
static uint32_t out_put_hex16(uint32_t p, uint16_t v) {
    static const char h[] = "0123456789abcdef";
    char buf[7];
    buf[0] = '0'; buf[1] = 'x';
    buf[2] = h[(v >> 12) & 0xF];
    buf[3] = h[(v >>  8) & 0xF];
    buf[4] = h[(v >>  4) & 0xF];
    buf[5] = h[ v        & 0xF];
    buf[6] = '\0';
    return out_put_str(p, buf);
}

/* ── OP_TRACE_DUMP handler ───────────────────────────────────────────── */
static microkit_msginfo handle_trace_dump(void) {
    uint32_t count = hdr_read_u32(8);
    uint32_t p = 0;

    for (uint32_t i = 0; i < count && p < TRACE_OUT_SIZE - 128; i++) {
        uint32_t off = TRACE_HDR_SIZE + i * TRACE_EVENT_SIZE;
        volatile uint8_t *ev = TRACE_BUF + off;

        /* Read tick (LE 64-bit) */
        uint64_t tick =
            (uint64_t)ev[0] | ((uint64_t)ev[1] << 8)  |
            ((uint64_t)ev[2] << 16) | ((uint64_t)ev[3] << 24) |
            ((uint64_t)ev[4] << 32) | ((uint64_t)ev[5] << 40) |
            ((uint64_t)ev[6] << 48) | ((uint64_t)ev[7] << 56);

        uint8_t  src_pd = ev[8];
        uint8_t  dst_pd = ev[9];
        uint16_t label  = (uint16_t)ev[10] | ((uint16_t)ev[11] << 8);
        uint32_t mr0    = (uint32_t)ev[12] | ((uint32_t)ev[13] << 8)
                        | ((uint32_t)ev[14] << 16) | ((uint32_t)ev[15] << 24);
        uint32_t mr1    = (uint32_t)ev[16] | ((uint32_t)ev[17] << 8)
                        | ((uint32_t)ev[18] << 16) | ((uint32_t)ev[19] << 24);

        p = out_put_str(p, "{\"tick\":");
        p = out_put_u64(p, tick);
        p = out_put_str(p, ",\"src\":\"");
        p = out_put_str(p, pd_name(src_pd));
        p = out_put_str(p, "\",\"dst\":\"");
        p = out_put_str(p, pd_name(dst_pd));
        p = out_put_str(p, "\",\"label\":\"");
        p = out_put_hex16(p, label);
        p = out_put_str(p, "\",\"mr0\":");
        p = out_put_u32(p, mr0);
        p = out_put_str(p, ",\"mr1\":");
        p = out_put_u32(p, mr1);
        p = out_put_str(p, "}\n");
    }

    if (p < TRACE_OUT_SIZE) TRACE_OUT[p] = 0;

    microkit_dbg_puts("[trace_recorder] DUMP: wrote ");
    {
        char tmp[12]; int i = 11; tmp[i] = '\0';
        uint32_t v = p;
        if (v == 0) tmp[--i] = '0';
        else while (v && i > 0) { tmp[--i] = (char)('0' + (v % 10)); v /= 10; }
        microkit_dbg_puts(&tmp[i]);
    }
    microkit_dbg_puts(" bytes, ");
    {
        char tmp[12]; int i = 11; tmp[i] = '\0';
        uint32_t v = count;
        if (v == 0) tmp[--i] = '0';
        else while (v && i > 0) { tmp[--i] = (char)('0' + (v % 10)); v /= 10; }
        microkit_dbg_puts(&tmp[i]);
    }
    microkit_dbg_puts(" events\n");

    microkit_mr_set(0, 0u);
    microkit_mr_set(1, p);
    return microkit_msginfo_new(0, 2);
}

/* ── PPC handler (control ops) ───────────────────────────────────────── */
static microkit_msginfo handle_protected(microkit_msginfo msg) {
    (void)msg;
    uint32_t op = (uint32_t)microkit_mr_get(0);

    switch (op) {

    case OP_TRACE_START:
        /* Reset buffer and begin recording */
        hdr_write_u32(0, TRACE_MAGIC);
        hdr_write_u32(4, TRACE_VERSION);
        hdr_write_u32(8, 0u);               /* count = 0 */
        hdr_write_u32(12, TRACE_FLAG_RECORDING);
        global_tick = 0;
        microkit_dbg_puts("[trace_recorder] START — recording\n");
        microkit_mr_set(0, 0u);
        return microkit_msginfo_new(0, 1);

    case OP_TRACE_STOP: {
        uint32_t flags = hdr_read_u32(12);
        hdr_write_u32(12, flags & ~TRACE_FLAG_RECORDING);
        microkit_dbg_puts("[trace_recorder] STOP — finalized\n");
        microkit_mr_set(0, 0u);
        return microkit_msginfo_new(0, 1);
    }

    case OP_TRACE_QUERY: {
        uint32_t count = hdr_read_u32(8);
        uint32_t bytes = TRACE_HDR_SIZE + count * TRACE_EVENT_SIZE;
        microkit_mr_set(0, count);
        microkit_mr_set(1, bytes);
        return microkit_msginfo_new(0, 2);
    }

    case OP_TRACE_DUMP:
        return handle_trace_dump();

    default:
        microkit_dbg_puts("[trace_recorder] Unknown op\n");
        microkit_mr_set(0, 0xFFu);
        return microkit_msginfo_new(0, 1);
    }
}

/* ── Microkit entry points ───────────────────────────────────────────── */
void init(void) {
    /* Seed header — not recording until OP_TRACE_START */
    hdr_write_u32(0, TRACE_MAGIC);
    hdr_write_u32(4, TRACE_VERSION);
    hdr_write_u32(8, 0u);
    hdr_write_u32(12, 0u);   /* flags = 0, not recording */

    /* Zero output region header */
    TRACE_OUT[0] = 0;

    agentos_log_boot("trace_recorder");
    microkit_dbg_puts("[trace_recorder] ALIVE — priority 90, passive, "
                      "512KB buf (26195 events max), 1MB JSONL out\n");
    microkit_dbg_puts("[trace_recorder] Channels: 0=control PPC, 1=event notify\n");
    microkit_dbg_puts("[trace_recorder] Send OP_TRACE_START (0x80) to begin capture\n");
}

/*
 * notified — called when controller signals channel 71 (CH_NOTIFY, local id=1).
 * MR0 encodes: src_pd[31:24] | dst_pd[23:16] | label[15:0]
 * MR1 = mr0 of the original IPC
 * MR2 = mr1 of the original IPC
 */
void notified(microkit_channel ch) {
    if (ch == CH_NOTIFY) {
        uint32_t packed = (uint32_t)microkit_mr_get(0);
        uint8_t  src_pd = (uint8_t)((packed >> 24) & 0xFF);
        uint8_t  dst_pd = (uint8_t)((packed >> 16) & 0xFF);
        uint16_t label  = (uint16_t)(packed & 0xFFFF);
        uint32_t mr0    = (uint32_t)microkit_mr_get(1);
        uint32_t mr1    = (uint32_t)microkit_mr_get(2);
        buf_append(src_pd, dst_pd, label, mr0, mr1);
    }
    /* CH_IN notifications (if any) are ignored — control via PPC only */
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msg) {
    (void)ch;
    return handle_protected(msg);
}
