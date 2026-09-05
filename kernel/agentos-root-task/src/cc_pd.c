/*
 * cc_pd.c — agentOS Command-and-Control Protection Domain
 *
 * Receives binary-framed requests from external callers (agentctl,
 * agentos_gui) via a VirtIO MMIO serial port, which QEMU bridges to
 * build/cc_pd.sock on the host.  Routes each call to the appropriate
 * service PD via seL4 IPC and returns the binary-framed reply.
 *
 * Transport:  VirtIO MMIO serial, virtio-mmio-bus.2 (PA 0x0A000400).
 *   QEMU args: -chardev socket,id=cc_pd_char,path=build/cc_pd.sock,...
 *              -device virtio-serial-device,bus=virtio-mmio-bus.2,id=vser0
 *              -device virtconsole,bus=vser0.0,chardev=cc_pd_char,name=cc.0
 *   Wire frame (both directions): 4112 bytes
 *     Request:  opcode(4) + mr[3](12) + shmem(4096) = 4112
 *     Reply:    mr[4](16) + shmem(4096) = 4112
 *
 * INVARIANT: cc_pd contains ZERO policy.  It is a relay only.
 * No routing logic beyond opcode dispatch and dev_type field lives here.
 *
 * Relay stubs: outbound seL4_Call paths to downstream PDs are implemented
 * as well-formed CC_OK stubs (empty data) until inter-PD endpoint wiring
 * is complete in Phase 5.
 *
 * Priority: 160
 * Mode: VirtIO polled loop; seL4_Yield while used ring empty to avoid starving PDs
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "contracts/cc_contract.h"
#include "contracts/vibeos_contract.h"
#include "contracts/guest_contract.h"
#include "contracts/framebuffer_contract.h"
#include "contracts/fault_inject_contract.h"
#include "contracts/log_drain_contract.h"
#include "contracts/agent_pool_contract.h"
#include "cc_retry_cache.h"
#include "sel4_ipc.h"
#include "serial_log.h"
#include "system_desc.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ─── VirtIO MMIO serial driver ──────────────────────────────────────────── */
/*
 * Transport: virtio-serial-device on QEMU virtio-mmio-bus.2 (PA 0x0A000400).
 * QEMU bridges the virtconsole named "cc.0" to build/cc_pd.sock.
 *
 * The root task allocates three 4K frames and identity-maps them (VA=PA) into
 * cc_pd's VSpace, then writes the three PAs into a shared startup record page
 * at CC_PD_STARTUP_VA before starting cc_pd.  The three pages are:
 *   [0] VQ struct page — descriptor tables, avail/used rings for TX+RX queues
 *   [1] TX data buffer
 *   [2] RX data buffer
 *
 * We use a single descriptor per queue (VQ_DEPTH=4 slots, one in flight at a
 * time) and poll the used ring with seL4_Yield so other PDs can run.
 *
 * Wire frame sizes (4112 bytes) exceed the 4096-byte buffer page, so TX and RX
 * loop in ≤4096-byte chunks.  The protocol is strictly sequential (one reply
 * per request), so no RX overflow can occur across frame boundaries.
 */

#define CC_PD_VIRTIO_VA   0x10002000UL   /* VirtIO MMIO page mapped by root task */
#define CC_PD_STARTUP_VA  0x10003000UL   /* Startup record: VQ PAs from root task */
#define VMMIO_SLOT_OFF    (2u * 0x200u)  /* bus.2 → offset +0x400 within the page */

/* ─── Diagnostics through the generic serial driver ────────────────────── */

static serial_log_t g_cc_log = {
    .ep = PD_CNODE_SLOT_SERIAL_EP,
};
static void cc_dbg_putc(char c)
{
    serial_log_putc(&g_cc_log, c);
}
static void cc_dbg_puts(const char *s)
{
    serial_log_puts(&g_cc_log, s);
}
static void cc_dbg_hex(uint64_t v)
{
    cc_dbg_puts("0x");
    for (int sh = 60; sh >= 0; sh -= 4) {
        uint32_t n = (uint32_t)((v >> (uint32_t)sh) & 0xFu);
        cc_dbg_putc(n < 10u ? (char)('0'+n) : (char)('a'+n-10u));
    }
}

/* VirtIO MMIO register offsets (relative to slot base) */
#define VMMIO_MAGIC           0x000u
#define VMMIO_VERSION         0x004u
#define VMMIO_DEVICE_ID       0x008u
#define VMMIO_DEV_FEAT        0x010u   /* DeviceFeatures (R): read current features word */
#define VMMIO_DEV_FEAT_SEL    0x014u   /* DeviceFeaturesSel (W): select features word to read */
#define VMMIO_DRV_FEAT        0x020u   /* DriverFeatures (W): write accepted features word */
#define VMMIO_DRV_FEAT_SEL    0x024u   /* DriverFeaturesSel (W): select features word to write */
#define VMMIO_QUEUE_SEL       0x030u
#define VMMIO_QUEUE_NUM_MAX   0x034u
#define VMMIO_QUEUE_NUM       0x038u
#define VMMIO_QUEUE_READY     0x044u
#define VMMIO_QUEUE_NOTIFY    0x050u
#define VMMIO_STATUS          0x070u
#define VMMIO_Q_DESC_LO       0x080u
#define VMMIO_Q_DESC_HI       0x084u
#define VMMIO_Q_AVAIL_LO      0x090u
#define VMMIO_Q_AVAIL_HI      0x094u
#define VMMIO_Q_USED_LO       0x0A0u
#define VMMIO_Q_USED_HI       0x0A4u

#define VSTATUS_ACK       1u
#define VSTATUS_DRIVER    2u
#define VSTATUS_FEAT_OK   8u
#define VSTATUS_DRIVER_OK 4u
#define VSTATUS_FAILED    128u
#define VIRTIO_MAGIC      0x74726976u
#define VIRTIO_ID_CONSOLE 3u
#define VQ_DEPTH          4u
#define CC_VIRTIO_WAIT_LIMIT 1000000u

typedef struct { uint64_t addr; uint32_t len; uint16_t flags; uint16_t next; }
    __attribute__((packed)) vq_desc_t;
typedef struct { uint16_t flags; uint16_t idx; uint16_t ring[VQ_DEPTH]; uint16_t used_event; }
    __attribute__((packed)) vq_avail_t;
typedef struct { uint32_t id; uint32_t len; } __attribute__((packed)) vq_used_elem_t;
typedef struct { uint16_t flags; uint16_t idx; vq_used_elem_t ring[VQ_DEPTH]; uint16_t avail_event; }
    __attribute__((packed)) vq_used_t;

/* Offsets within VQ struct page (all alignment requirements met) */
#define TX_DESC_OFF   0u    /* 4 × 16 B = 64 B; 16-byte aligned */
#define TX_AVAIL_OFF  128u  /* 14 B; 2-byte aligned */
#define TX_USED_OFF   256u  /* 38 B; 4-byte aligned */
#define RX_DESC_OFF   512u  /* 64 B; 16-byte aligned */
#define RX_AVAIL_OFF  640u  /* 14 B; 2-byte aligned */
#define RX_USED_OFF   768u  /* 38 B; 4-byte aligned */

static seL4_Word          g_vq_pa[3];       /* [0]=structs, [1]=TX buf, [2]=RX buf */
static volatile uint32_t *g_virtio;         /* VirtIO MMIO base at bus.2 slot */
static uint16_t           g_rx_used_last;   /* shadow of RX used ring consumer idx */

/* VQ struct page is identity-mapped: virtual address == physical address */
#define QP       ((uintptr_t)g_vq_pa[0])
#define TX_DESC  ((volatile vq_desc_t  *)(QP + TX_DESC_OFF))
#define TX_AVAIL ((volatile vq_avail_t *)(QP + TX_AVAIL_OFF))
#define TX_USED  ((volatile vq_used_t  *)(QP + TX_USED_OFF))
#define RX_DESC  ((volatile vq_desc_t  *)(QP + RX_DESC_OFF))
#define RX_AVAIL ((volatile vq_avail_t *)(QP + RX_AVAIL_OFF))
#define RX_USED  ((volatile vq_used_t  *)(QP + RX_USED_OFF))

static inline uint32_t vio_rd(uint32_t off)
{
    return *(volatile uint32_t *)((uintptr_t)g_virtio + off);
}
static inline void vio_wr(uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)((uintptr_t)g_virtio + off) = val;
}
#if defined(__aarch64__)
#define VQ_MB() __asm__ volatile("dsb sy" ::: "memory")
#elif defined(__riscv)
#define VQ_MB() __asm__ volatile("fence rw,rw" ::: "memory")
#elif defined(__x86_64__)
#define VQ_MB() __asm__ volatile("mfence" ::: "memory")
#else
#define VQ_MB() __asm__ volatile("" ::: "memory")
#endif

static void vio_queue_setup(uint32_t qidx,
                             seL4_Word desc_pa, seL4_Word avail_pa, seL4_Word used_pa)
{
    vio_wr(VMMIO_QUEUE_SEL,   qidx);
    vio_wr(VMMIO_QUEUE_NUM,   VQ_DEPTH);
    vio_wr(VMMIO_Q_DESC_LO,   (uint32_t)(desc_pa  & 0xFFFFFFFFu));
    vio_wr(VMMIO_Q_DESC_HI,   (uint32_t)(desc_pa  >> 32u));
    vio_wr(VMMIO_Q_AVAIL_LO,  (uint32_t)(avail_pa & 0xFFFFFFFFu));
    vio_wr(VMMIO_Q_AVAIL_HI,  (uint32_t)(avail_pa >> 32u));
    vio_wr(VMMIO_Q_USED_LO,   (uint32_t)(used_pa  & 0xFFFFFFFFu));
    vio_wr(VMMIO_Q_USED_HI,   (uint32_t)(used_pa  >> 32u));
    vio_wr(VMMIO_QUEUE_READY, 1u);
}

static bool virtio_serial_init(void)
{
    /* VQ PAs written by root task into startup record before cc_pd started */
    volatile seL4_Word *sp = (volatile seL4_Word *)CC_PD_STARTUP_VA;
    g_vq_pa[0] = sp[0];
    g_vq_pa[1] = sp[1];
    g_vq_pa[2] = sp[2];

    g_virtio = (volatile uint32_t *)(CC_PD_VIRTIO_VA + VMMIO_SLOT_OFF);

    uint32_t magic   = vio_rd(VMMIO_MAGIC);
    uint32_t version = vio_rd(VMMIO_VERSION);
    uint32_t devid   = vio_rd(VMMIO_DEVICE_ID);
    cc_dbg_puts("[cc_pd] VirtIO magic="); cc_dbg_hex(magic);
    cc_dbg_puts(" ver="); cc_dbg_hex(version);
    cc_dbg_puts(" devid="); cc_dbg_hex(devid);
    cc_dbg_puts("\n");

    if (magic != VIRTIO_MAGIC || devid != VIRTIO_ID_CONSOLE) {
        cc_dbg_puts("[cc_pd] VirtIO init FAILED: bad magic/devid\n");
        return false;
    }

    /* VirtIO 1.0 initialisation sequence */
    vio_wr(VMMIO_STATUS, 0u);
    vio_wr(VMMIO_STATUS, VSTATUS_ACK);
    vio_wr(VMMIO_STATUS, VSTATUS_ACK | VSTATUS_DRIVER);
    /* Negotiate features: read both 32-bit words, accept them with MULTIPORT
     * cleared (bit 1 of word 0) and VIRTIO_F_VERSION_1 set (bit 0 of word 1).
     * Without VIRTIO_F_VERSION_1 the device falls back to legacy mode where
     * QueueDescLow/High and QueueReady do not exist. */
    vio_wr(VMMIO_DEV_FEAT_SEL, 0u);
    uint32_t feat0 = vio_rd(VMMIO_DEV_FEAT);
    vio_wr(VMMIO_DEV_FEAT_SEL, 1u);
    uint32_t feat1 = vio_rd(VMMIO_DEV_FEAT);
    vio_wr(VMMIO_DRV_FEAT_SEL, 0u);
    vio_wr(VMMIO_DRV_FEAT, feat0 & ~(1u << 1u));  /* clear MULTIPORT */
    vio_wr(VMMIO_DRV_FEAT_SEL, 1u);
    vio_wr(VMMIO_DRV_FEAT, feat1);                /* accepts VIRTIO_F_VERSION_1 */
    vio_wr(VMMIO_STATUS, VSTATUS_ACK | VSTATUS_DRIVER | VSTATUS_FEAT_OK);
    uint32_t s_after = vio_rd(VMMIO_STATUS);
    cc_dbg_puts("[cc_pd] STATUS after FEAT_OK write="); cc_dbg_hex(s_after); cc_dbg_puts("\n");
    if (!(s_after & VSTATUS_FEAT_OK)) {
        cc_dbg_puts("[cc_pd] VirtIO FEAT_OK not set\n");
        vio_wr(VMMIO_STATUS, VSTATUS_FAILED);
        return false;
    }

    /* Device reset invalidates every in-flight descriptor. Start both rings
     * from a known epoch before making them ready again. */
    __builtin_memset((void *)g_vq_pa[0], 0, 4096u);
    VQ_MB();
    vio_queue_setup(0u,
        g_vq_pa[0] + RX_DESC_OFF, g_vq_pa[0] + RX_AVAIL_OFF, g_vq_pa[0] + RX_USED_OFF);
    vio_queue_setup(1u,
        g_vq_pa[0] + TX_DESC_OFF, g_vq_pa[0] + TX_AVAIL_OFF, g_vq_pa[0] + TX_USED_OFF);

    vio_wr(VMMIO_STATUS,
           VSTATUS_ACK | VSTATUS_DRIVER | VSTATUS_FEAT_OK | VSTATUS_DRIVER_OK);

    /* Pre-post RX descriptor so the device can buffer incoming bytes */
    RX_DESC[0].addr  = (uint64_t)g_vq_pa[2];
    RX_DESC[0].len   = 4096u;
    RX_DESC[0].flags = 2u;  /* VIRTQ_DESC_F_WRITE: device writes into this buf */
    RX_DESC[0].next  = 0u;
    VQ_MB();
    RX_AVAIL->ring[0] = 0u;
    VQ_MB();
    RX_AVAIL->idx = 1u;
    VQ_MB();
    vio_wr(VMMIO_QUEUE_NOTIFY, 0u);
    g_rx_used_last = 0u;

    cc_dbg_puts("[cc_pd] VirtIO serial ready\n");
    return true;
}

static void virtio_serial_recover_tx(void)
{
    cc_dbg_puts("[cc_pd] resetting VirtIO serial after incomplete reply\n");
    vio_wr(VMMIO_STATUS, VSTATUS_FAILED);
    VQ_MB();
    vio_wr(VMMIO_STATUS, 0u);
    VQ_MB();
    if (!virtio_serial_init()) {
        cc_dbg_puts("[cc_pd] VirtIO serial recovery failed\n");
    }
}

static bool vio_serial_write(const void *buf, uint32_t n)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (n > 0u) {
        uint32_t chunk = (n > 4096u) ? 4096u : n;
        __builtin_memcpy((void *)g_vq_pa[1], p, chunk);
        VQ_MB();
        TX_DESC[0].addr  = (uint64_t)g_vq_pa[1];
        TX_DESC[0].len   = chunk;
        TX_DESC[0].flags = 0u;
        TX_DESC[0].next  = 0u;
        VQ_MB();
        uint16_t old_used = TX_USED->idx;
        TX_AVAIL->ring[TX_AVAIL->idx & (uint16_t)(VQ_DEPTH - 1u)] = 0u;
        VQ_MB();
        TX_AVAIL->idx++;
        VQ_MB();
#ifdef CC_PD_TRACE_TX
        cc_dbg_puts("[cc_pd] TX notify n="); cc_dbg_hex(n);
        cc_dbg_puts(" old_used="); cc_dbg_hex(old_used);
        cc_dbg_puts(" avail_idx="); cc_dbg_hex(TX_AVAIL->idx);
        cc_dbg_puts(" desc_addr="); cc_dbg_hex(TX_DESC[0].addr);
        cc_dbg_puts("\n");
#endif
        vio_wr(VMMIO_QUEUE_NOTIFY, 1u);
#ifdef CC_PD_TRACE_TX
        uint16_t cur_used = TX_USED->idx;
        cc_dbg_puts("[cc_pd] TX post-notify used="); cc_dbg_hex(cur_used); cc_dbg_puts("\n");
#endif
#ifdef CC_PD_TRACE_TX
        uint32_t spin = 0u;
#endif
        uint32_t wait = 0u;
        while (TX_USED->idx == old_used) {
            VQ_MB();
            seL4_Yield();
            wait++;
            if (wait >= CC_VIRTIO_WAIT_LIMIT) {
                cc_dbg_puts("[cc_pd] TX timeout waiting for used ring\n");
                return false;
            }
#ifdef CC_PD_TRACE_TX
            spin++;
            if (spin <= 3u || (spin & 0xFFFFu) == 0u) {
                cc_dbg_puts("[cc_pd] TX yield spin="); cc_dbg_hex(spin);
                cc_dbg_puts(" used="); cc_dbg_hex(TX_USED->idx);
                cc_dbg_puts("\n");
            }
#endif
        }
#ifdef CC_PD_TRACE_TX
        cc_dbg_puts("[cc_pd] TX done spin="); cc_dbg_hex(spin); cc_dbg_puts("\n");
#endif
        p += chunk;
        n -= chunk;
    }
    return true;
}

static bool vio_serial_read(void *buf, uint32_t n)
{
    uint8_t *p = (uint8_t *)buf;
    while (n > 0u) {
        uint16_t cur;
        uint32_t wait = 0u;
        for (;;) {
            VQ_MB();
            cur = RX_USED->idx;
            if (cur != g_rx_used_last) { break; }
            seL4_Yield();
            wait++;
            if (wait >= CC_VIRTIO_WAIT_LIMIT) {
                cc_dbg_puts("[cc_pd] RX timeout waiting for used ring\n");
                return false;
            }
        }
        uint32_t got  = RX_USED->ring[g_rx_used_last & (uint16_t)(VQ_DEPTH - 1u)].len;
        VQ_MB();
        uint32_t take = (got > n) ? n : got;
        __builtin_memcpy(p, (const void *)g_vq_pa[2], take);
        p += take;
        n -= take;
        g_rx_used_last++;
        /* Re-post RX buffer */
        RX_DESC[0].addr  = (uint64_t)g_vq_pa[2];
        RX_DESC[0].len   = 4096u;
        RX_DESC[0].flags = 2u;
        RX_DESC[0].next  = 0u;
        VQ_MB();
        RX_AVAIL->ring[RX_AVAIL->idx & (uint16_t)(VQ_DEPTH - 1u)] = 0u;
        VQ_MB();
        RX_AVAIL->idx++;
        VQ_MB();
        vio_wr(VMMIO_QUEUE_NOTIFY, 0u);
    }
    return true;
}

/* ─── Wire frame types ───────────────────────────────────────────────────── */
/*
 * These match the layout used by agentctl-ng and agentos_gui exactly.
 * The static_asserts enforce the 4112-byte invariant at compile time.
 */

#define CC_WIRE_SHMEM_SIZE  4096u

typedef struct {
    uint32_t opcode;
    uint32_t mr[3];             /* MR1, MR2, MR3 from caller */
    uint8_t  shmem[CC_WIRE_SHMEM_SIZE];
} cc_req_wire_t;

typedef struct {
    uint32_t mr[4];             /* MR0 (status), MR1, MR2, MR3 */
    uint8_t  shmem[CC_WIRE_SHMEM_SIZE];
} cc_reply_wire_t;

_Static_assert(sizeof(cc_req_wire_t)   == 4112u, "cc_req_wire_t size");
_Static_assert(sizeof(cc_reply_wire_t) == 4112u, "cc_reply_wire_t size");
_Static_assert(sizeof(cc_trace_entry_t) == CC_TRACE_ENTRY_SIZE,
               "cc_trace_entry_t size");

/* ─── Session table ──────────────────────────────────────────────────────── */

typedef struct {
    bool     active;
    uint32_t client_badge;
    uint32_t state;
    uint32_t ticks_since_active;
    uint32_t resp_pending;
    uint32_t resp_len;
    uint8_t  resp[CC_MAX_RESP_BYTES];
} cc_session_t;

static cc_session_t g_sessions[CC_MAX_SESSIONS];

/* ─── Log-stream slot table (agentos-vsi) ───────────────────────────────────
 *
 * MSG_CC_LOG_STREAM exposes each guest's serial output as an addressable log
 * slot.  Slot 0 is permanently reserved for the boot guest (drained via the
 * guest_vmm console-drain path, keyed by pd_id == TRACE_PD_CONTROLLER).  Slots
 * 1..CC_LOG_SLOTS-1 are allocated on demand, one per vibe_engine guest, each
 * recording the vibe guest handle it streams.  A caller addresses a stream by
 * (slot, pd_id): pd_id selects the drain backend (boot guest vs. vibe guest)
 * and slot resolves to the concrete guest handle within that backend.
 */
#define CC_LOG_SLOTS          8u
#define CC_LOG_SLOT_BOOT      0u
#define CC_LOG_SLOT_INVALID   0xFFFFFFFFu

typedef struct {
    bool     in_use;
    uint32_t guest_handle;   /* vibe_engine handle this slot streams */
    uint32_t pd_id;          /* TRACE_PD_* backend tag for diagnostics */
} cc_log_slot_t;

static cc_log_slot_t g_log_slots[CC_LOG_SLOTS];

/* Allocate (or return existing) log slot for a vibe guest handle.  Returns the
 * slot index, or CC_LOG_SLOT_INVALID when the table is full.  Slot 0 is never
 * handed out here — it belongs to the boot guest. */
static uint32_t cc_log_slot_for_handle(uint32_t guest_handle, uint32_t pd_id)
{
    for (uint32_t i = 1u; i < CC_LOG_SLOTS; i++) {
        if (g_log_slots[i].in_use &&
            g_log_slots[i].guest_handle == guest_handle) {
            return i;
        }
    }
    for (uint32_t i = 1u; i < CC_LOG_SLOTS; i++) {
        if (!g_log_slots[i].in_use) {
            g_log_slots[i].in_use       = true;
            g_log_slots[i].guest_handle = guest_handle;
            g_log_slots[i].pd_id        = pd_id;
            return i;
        }
    }
    return CC_LOG_SLOT_INVALID;
}

/* ─── CC trace bridge ────────────────────────────────────────────────────── */

#define CC_TRACE_RING_ENTRIES  CC_TRACE_MAX_ENTRIES

static cc_trace_entry_t g_cc_trace_ring[CC_TRACE_RING_ENTRIES];
static uint32_t g_cc_trace_head;
static uint32_t g_cc_trace_count;
static uint32_t g_cc_trace_seq;
static uint32_t g_cc_trace_overflow;
static bool     g_cc_trace_recording;

static uint8_t cc_trace_target_pd(uint32_t opcode)
{
    switch (opcode) {
    case MSG_CC_LIST_GUESTS:
    case MSG_CC_GUEST_STATUS:
    case MSG_CC_CREATE_GUEST:
    case MSG_CC_SNAPSHOT:
    case MSG_CC_RESTORE:
        return (uint8_t)TRACE_PD_VIBE_ENGINE;
    case MSG_CC_SEND_INPUT:
    case MSG_CC_SUSPEND_GUEST:
    case MSG_CC_RESUME_GUEST:
    case MSG_CC_DESTROY_GUEST:
#if defined(AGENTOS_GUEST_FREEBSD)
        return (uint8_t)TRACE_PD_FREEBSD_VMM;
#elif defined(AGENTOS_GUEST_LINUX)
        return (uint8_t)TRACE_PD_LINUX_VMM;
#else
        return (uint8_t)TRACE_PD_CC_PD;
#endif
    case MSG_CC_TRACE_START:
    case MSG_CC_TRACE_STOP:
    case MSG_CC_TRACE_QUERY:
    case MSG_CC_TRACE_DUMP:
        return (uint8_t)TRACE_PD_TRACE_REC;
    case MSG_CC_FAULT_INJECT:
        return (uint8_t)TRACE_PD_FAULT_HDL;
    default:
        return (uint8_t)TRACE_PD_CC_PD;
    }
}

static uint8_t cc_trace_channel(uint32_t opcode)
{
    switch (opcode) {
    case MSG_CC_LIST_GUESTS:
    case MSG_CC_GUEST_STATUS:
    case MSG_CC_CREATE_GUEST:
    case MSG_CC_SNAPSHOT:
    case MSG_CC_RESTORE:
        return (uint8_t)(CH_VIBEOS_ENGINE & 0xffu);
    case MSG_CC_SEND_INPUT:
    case MSG_CC_SUSPEND_GUEST:
    case MSG_CC_RESUME_GUEST:
    case MSG_CC_DESTROY_GUEST:
        return (uint8_t)(CH_GUEST_PD & 0xffu);
    case MSG_CC_TRACE_START:
    case MSG_CC_TRACE_STOP:
    case MSG_CC_TRACE_QUERY:
    case MSG_CC_TRACE_DUMP:
        return (uint8_t)(CH_TRACE_CTRL & 0xffu);
    case MSG_CC_LOG_STREAM:
        return (uint8_t)(CH_LOG_DRAIN & 0xffu);
    default:
        return (uint8_t)(CH_CC_PD & 0xffu);
    }
}

static void cc_trace_reset(bool recording)
{
    g_cc_trace_head = 0u;
    g_cc_trace_count = 0u;
    g_cc_trace_seq = 0u;
    g_cc_trace_overflow = 0u;
    g_cc_trace_recording = recording;
}

static void cc_trace_record(uint32_t opcode)
{
    if (!g_cc_trace_recording) return;

    cc_trace_entry_t *e = &g_cc_trace_ring[g_cc_trace_head];
    e->timestamp_ns = ((uint64_t)g_cc_trace_seq + 1u) * 1000u;
    e->from_pd = (uint8_t)TRACE_PD_CC_PD;
    e->to_pd = cc_trace_target_pd(opcode);
    e->channel = cc_trace_channel(opcode);
    e->_pad = 0u;
    e->opcode = (uint16_t)(opcode & 0xffffu);
    e->seq_lo = (uint16_t)(g_cc_trace_seq & 0xffffu);

    g_cc_trace_seq++;
    g_cc_trace_head = (g_cc_trace_head + 1u) % CC_TRACE_RING_ENTRIES;
    if (g_cc_trace_count < CC_TRACE_RING_ENTRIES) {
        g_cc_trace_count++;
    } else {
        g_cc_trace_overflow++;
    }
}

/* ─── Boot guest inventory ─────────────────────────────────────────────────
 *
 * The default QEMU image starts one Unix guest VMM at boot when GUEST_OS is
 * set.  Full VibeOS relay wiring is still Phase 5 work, but the published CC
 * contract already requires LIST_GUESTS/GUEST_STATUS to expose the running
 * guest to external consumers.
 */

#if defined(AGENTOS_GUEST_LINUX) || defined(AGENTOS_GUEST_FREEBSD)
#define CC_BOOT_GUEST_HANDLE 0u

static bool     g_boot_guest_present = true;
static uint32_t g_boot_guest_state = GUEST_STATE_RUNNING;
#endif

/* Map VIBEOS_STATE_* (0..5) to GUEST_STATE_* (0..6).  vibe_engine reports
 * its own state taxonomy; the CC contract exposes guest_contract.h states. */
static uint32_t cc_vibeos_to_guest_state(uint32_t vos_state)
{
    /* vibeos: 0=CREATING 1=BOOTING 2=RUNNING 3=PAUSED 4=DEAD 5=MIGRATING
     * guest:  0=CREATING 1=BINDING 2=READY 3=BOOTING 4=RUNNING 5=SUSPENDED 6=DEAD */
    switch (vos_state) {
    case 0u: return GUEST_STATE_CREATING;
    case 1u: return GUEST_STATE_BOOTING;
    case 2u: return GUEST_STATE_RUNNING;
    case 3u: return GUEST_STATE_SUSPENDED;
    case 4u: return GUEST_STATE_DEAD;
    case 5u: return GUEST_STATE_BOOTING;  /* migrating ≈ booting on dest */
    default: return GUEST_STATE_DEAD;
    }
}

static void cc_msg_wr32(uint8_t *dst, uint32_t off, uint32_t value)
{
    dst[off + 0u] = (uint8_t)(value & 0xffu);
    dst[off + 1u] = (uint8_t)((value >> 8u) & 0xffu);
    dst[off + 2u] = (uint8_t)((value >> 16u) & 0xffu);
    dst[off + 3u] = (uint8_t)((value >> 24u) & 0xffu);
}

static uint32_t cc_msg_rd32(const uint8_t *src, uint32_t off)
{
    return (uint32_t)src[off + 0u]
         | ((uint32_t)src[off + 1u] << 8u)
         | ((uint32_t)src[off + 2u] << 16u)
         | ((uint32_t)src[off + 3u] << 24u);
}

#if defined(AGENTOS_GUEST_LINUX) || defined(AGENTOS_GUEST_FREEBSD)

static uint32_t cc_boot_guest_os_type(void)
{
#if defined(AGENTOS_GUEST_FREEBSD) && !defined(AGENTOS_GUEST_BOTH)
    return VIBEOS_TYPE_FREEBSD;
#else
    return VIBEOS_TYPE_LINUX;
#endif
}

static uint32_t cc_boot_guest_arch(void)
{
#if defined(__x86_64__)
    return VIBEOS_ARCH_X86_64;
#else
    return VIBEOS_ARCH_AARCH64;
#endif
}

static uint32_t cc_boot_guest_devices(void)
{
    return VIBEOS_DEV_SERIAL | VIBEOS_DEV_NET | VIBEOS_DEV_BLOCK;
}

static void cc_fill_boot_guest_info(cc_guest_info_t *out)
{
    out->guest_handle = CC_BOOT_GUEST_HANDLE;
    out->state        = g_boot_guest_state;
    out->os_type      = cc_boot_guest_os_type();
    out->arch         = cc_boot_guest_arch();
}

static void cc_fill_boot_guest_status(cc_guest_status_t *out)
{
    out->guest_handle = CC_BOOT_GUEST_HANDLE;
    out->state        = g_boot_guest_state;
    out->os_type      = cc_boot_guest_os_type();
    out->arch         = cc_boot_guest_arch();
    out->device_flags = cc_boot_guest_devices();
    for (uint32_t i = 0u; i < 3u; i++) out->_reserved[i] = 0u;
}

static bool cc_boot_guest_has_device(uint32_t dev_type)
{
    if (!g_boot_guest_present) return false;
    switch (dev_type) {
    case CC_DEV_TYPE_SERIAL:
    case CC_DEV_TYPE_NET:
    case CC_DEV_TYPE_BLOCK:
        return true;
    default:
        return false;
    }
}

static bool cc_call_boot_guest(uint32_t opcode, const uint8_t *payload,
                               uint32_t payload_len, sel4_msg_t *reply)
{
    if (payload_len > SEL4_MSG_DATA_BYTES) return false;

    sel4_msg_t msg = {0};
    msg.opcode = opcode;
    msg.length = payload_len;
    if (payload_len > 0u && payload != NULL) {
        __builtin_memcpy(msg.data, payload, payload_len);
    }

    sel4_call((seL4_CPtr)PD_CNODE_SLOT_GUEST_VMM_EP, &msg, reply);
    return reply->opcode == GUEST_OK;
}

static bool cc_forward_boot_guest_input(const cc_input_event_t *event,
                                        const uint8_t *text,
                                        uint32_t text_len)
{
    if (!g_boot_guest_present) return false;

    uint8_t payload[SEL4_MSG_DATA_BYTES];
    uint32_t payload_len = 4u + (uint32_t)sizeof(cc_input_event_t) + text_len;
    if (payload_len > sizeof(payload)) return false;
    cc_msg_wr32(payload, 0u, CC_BOOT_GUEST_HANDLE);
    __builtin_memcpy(payload + 4u, event, sizeof(cc_input_event_t));
    if (text_len > 0u) {
        __builtin_memcpy(payload + 4u + sizeof(cc_input_event_t), text, text_len);
    }

    sel4_msg_t reply = {0};
    return cc_call_boot_guest(MSG_GUEST_SEND_INPUT, payload,
                              payload_len, &reply);
}

static bool cc_drain_boot_guest_console(uint8_t *dst, uint32_t max,
                                        uint32_t *bytes_drained)
{
    if (!g_boot_guest_present) return false;

    /* One downstream call per host frame keeps CC latency bounded. */
    uint8_t payload[8u];
    uint32_t want = max;
    if (want > SEL4_MSG_DATA_BYTES) want = SEL4_MSG_DATA_BYTES;
    cc_msg_wr32(payload, 0u, CC_BOOT_GUEST_HANDLE);
    cc_msg_wr32(payload, 4u, want);

    sel4_msg_t reply = {0};
    if (!cc_call_boot_guest(MSG_GUEST_CONSOLE_DRAIN, payload,
                            (uint32_t)sizeof(payload), &reply)) {
        return false;
    }

    uint32_t n = reply.length;
    if (n > SEL4_MSG_DATA_BYTES) n = SEL4_MSG_DATA_BYTES;
    if (n > max) n = max;
    if (n > 0u) __builtin_memcpy(dst, reply.data, n);
    *bytes_drained = n;
    return true;
}

static bool cc_forward_vibe_input(uint32_t handle,
                                  const cc_input_event_t *event,
                                  const uint8_t *text,
                                  uint32_t text_len)
{
    sel4_msg_t msg = {0}, reply = {0};
    uint32_t payload_len = (uint32_t)sizeof(cc_input_event_t) + text_len;
    if (4u + payload_len > SEL4_MSG_DATA_BYTES) return false;
    msg.opcode = MSG_VIBEOS_SEND_INPUT;
    msg.length = 4u + payload_len;
    cc_msg_wr32(msg.data, 0u, handle);
    __builtin_memcpy(msg.data + 4u, event, sizeof(cc_input_event_t));
    if (text_len > 0u) {
        __builtin_memcpy(msg.data + 4u + sizeof(cc_input_event_t), text, text_len);
    }

    sel4_call((seL4_CPtr)PD_CNODE_SLOT_VIBE_ENGINE_EP, &msg, &reply);
    return reply.opcode == SEL4_ERR_OK && cc_msg_rd32(reply.data, 0u) == 0u;
}

static bool cc_drain_vibe_console(uint32_t handle, uint8_t *dst,
                                  uint32_t max, uint32_t *bytes_drained)
{
    /* Do not monopolize the VMM with a 4 KiB loop of tiny inline IPCs. */
    uint32_t want = max;
    if (want > SEL4_MSG_DATA_BYTES - 8u)
        want = SEL4_MSG_DATA_BYTES - 8u;

    sel4_msg_t msg = {0}, reply = {0};
    msg.opcode = MSG_VIBEOS_CONSOLE_DRAIN;
    msg.length = 8u;
    cc_msg_wr32(msg.data, 0u, handle);
    cc_msg_wr32(msg.data, 4u, want);
    sel4_call((seL4_CPtr)PD_CNODE_SLOT_VIBE_ENGINE_EP, &msg, &reply);
    if (reply.opcode != SEL4_ERR_OK || cc_msg_rd32(reply.data, 0u) != 0u)
        return false;

    uint32_t n = cc_msg_rd32(reply.data, 4u);
    if (n > SEL4_MSG_DATA_BYTES - 8u) n = SEL4_MSG_DATA_BYTES - 8u;
    if (n > max) n = max;
    if (n > 0u) __builtin_memcpy(dst, reply.data + 8u, n);
    *bytes_drained = n;
    return true;
}

static bool cc_lifecycle_boot_guest(uint32_t opcode, uint32_t reason,
                                    uint32_t *new_state)
{
    if (!g_boot_guest_present) return false;

    uint8_t payload[8u];
    uint32_t payload_len = 4u;
    cc_msg_wr32(payload, 0u, CC_BOOT_GUEST_HANDLE);
    if (opcode == MSG_GUEST_DESTROY) {
        cc_msg_wr32(payload, 4u, reason);
        payload_len = 8u;
    }

    sel4_msg_t reply = {0};
    if (!cc_call_boot_guest(opcode, payload, payload_len, &reply)) {
        return false;
    }

    switch (opcode) {
    case MSG_GUEST_SUSPEND:
        g_boot_guest_state = GUEST_STATE_SUSPENDED;
        break;
    case MSG_GUEST_RESUME:
        g_boot_guest_state = GUEST_STATE_RUNNING;
        break;
    case MSG_GUEST_DESTROY:
        g_boot_guest_state = GUEST_STATE_DEAD;
        g_boot_guest_present = false;
        break;
    default:
        return false;
    }

    if (new_state != NULL) *new_state = g_boot_guest_state;
    return true;
}
#endif

static bool cc_lifecycle_vibeos_guest(uint32_t opcode, uint32_t handle,
                                      uint32_t *new_state)
{
    sel4_msg_t req = {0};
    sel4_msg_t rep = {0};
    req.opcode = opcode;
    req.length = 4u;
    cc_msg_wr32(req.data, 0u, handle);

    sel4_call((seL4_CPtr)PD_CNODE_SLOT_VIBE_ENGINE_EP, &req, &rep);
    if (rep.opcode != SEL4_ERR_OK || cc_msg_rd32(rep.data, 0u) != VIBEOS_OK) {
        return false;
    }

    if (new_state != NULL) {
        *new_state = (rep.length >= 8u)
            ? cc_vibeos_to_guest_state(cc_msg_rd32(rep.data, 4u))
            : 0u;
    }
    return true;
}

/* cc_pd has no EOF signal from the host-side socket — when a client process
 * dies ungracefully, qemu's chardev silently accepts a new connection but
 * the leaked session lingers.  Aging every other active session on each
 * dispatch (cc_age_sessions) plus reaping the oldest active session when
 * alloc_session has no free slot lets the next new caller reclaim a slot
 * without a full reboot.  No threshold: the session table is small and the
 * oldest-active session is by definition the most stale once the table is
 * full, so unconditionally reap it.  ticks_since_active >= 1 means the
 * session has not been touched on the current dispatch, so the in-flight
 * caller is never reaped from under itself. */
static void cc_age_sessions(void)
{
    for (uint32_t i = 0u; i < CC_MAX_SESSIONS; i++) {
        if (g_sessions[i].active &&
            g_sessions[i].ticks_since_active < UINT32_MAX) {
            g_sessions[i].ticks_since_active++;
        }
    }
}

static int reap_oldest_session(void)
{
    int victim = -1;
    uint32_t oldest = 0u;
    for (int i = 0; i < (int)CC_MAX_SESSIONS; i++) {
        if (g_sessions[i].active &&
            g_sessions[i].ticks_since_active >= 1u &&
            g_sessions[i].ticks_since_active >= oldest) {
            oldest = g_sessions[i].ticks_since_active;
            victim = i;
        }
    }
    if (victim >= 0) {
        g_sessions[victim].active       = false;
        g_sessions[victim].state        = CC_SESSION_STATE_EXPIRED;
        g_sessions[victim].resp_pending = 0u;
        g_sessions[victim].resp_len     = 0u;
    }
    return victim;
}

static int alloc_session(void)
{
    for (int i = 0; i < (int)CC_MAX_SESSIONS; i++) {
        if (!g_sessions[i].active) return i;
    }
    /* Table full — reclaim the oldest active slot.  This is the recovery
     * path for clients that disconnect ungracefully (no MSG_CC_DISCONNECT). */
    return reap_oldest_session();
}

static bool valid_session(uint32_t sid)
{
    return sid < CC_MAX_SESSIONS &&
           g_sessions[sid].active &&
           g_sessions[sid].state != (uint32_t)CC_SESSION_STATE_EXPIRED;
}

static void cc_wire_wr32(uint8_t *dst, uint32_t off, uint32_t value)
{
    dst[off + 0u] = (uint8_t)(value & 0xffu);
    dst[off + 1u] = (uint8_t)((value >> 8u) & 0xffu);
    dst[off + 2u] = (uint8_t)((value >> 16u) & 0xffu);
    dst[off + 3u] = (uint8_t)((value >> 24u) & 0xffu);
}

static uint32_t cc_wire_rd32(const uint8_t *src, uint32_t off)
{
    return (uint32_t)src[off + 0u]
         | ((uint32_t)src[off + 1u] << 8u)
         | ((uint32_t)src[off + 2u] << 16u)
         | ((uint32_t)src[off + 3u] << 24u);
}

/* ─── Session management handlers ───────────────────────────────────────── */

static void handle_connect(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    int s = alloc_session();
    if (s < 0) {
        rep->mr[0] = CC_ERR_NO_SESSIONS;
        rep->mr[1] = 0u;
        return;
    }
    g_sessions[s].active             = true;
    g_sessions[s].client_badge       = req->mr[0]; /* badge in MR1 */
    g_sessions[s].state              = CC_SESSION_STATE_CONNECTED;
    g_sessions[s].ticks_since_active = 0u;
    g_sessions[s].resp_pending       = 0u;
    g_sessions[s].resp_len           = 0u;

    rep->mr[0] = CC_OK;
    rep->mr[1] = (uint32_t)s;
}

static void handle_disconnect(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    uint32_t sid = req->mr[0]; /* session_id in MR1 */
    if (!valid_session(sid)) {
        rep->mr[0] = CC_ERR_BAD_SESSION;
        return;
    }
    g_sessions[sid].active = false;
    g_sessions[sid].resp_pending = 0u;
    g_sessions[sid].resp_len = 0u;
    rep->mr[0] = CC_OK;
}

static void handle_send(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    uint32_t sid = req->mr[0]; /* session_id in MR1 */
    uint32_t len = req->mr[2]; /* command byte length in MR3 */

    if (!valid_session(sid)) {
        rep->mr[0] = CC_ERR_BAD_SESSION;
        rep->mr[1] = 0u;
        return;
    }
    if (len > CC_MAX_CMD_BYTES) {
        rep->mr[0] = CC_ERR_CMD_TOO_LARGE;
        rep->mr[1] = 0u;
        return;
    }

    /*
     * The real service-routing surface is the direct MSG_CC_* relay API below.
     * For the legacy session API, queue a deterministic empty success response
     * so callers can distinguish "accepted command" from "unknown opcode".
     */
    (void)req;
    g_sessions[sid].state = CC_SESSION_STATE_IDLE;
    g_sessions[sid].ticks_since_active = 0u;
    g_sessions[sid].resp_pending = 1u;
    g_sessions[sid].resp_len = 0u;

    rep->mr[0] = CC_OK;
    rep->mr[1] = 1u;
}

static void handle_recv(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    uint32_t sid = req->mr[0]; /* session_id in MR1 */
    uint32_t max = req->mr[1]; /* max response bytes in MR2 */

    if (!valid_session(sid)) {
        rep->mr[0] = CC_ERR_BAD_SESSION;
        rep->mr[1] = 0u;
        return;
    }
    if (!g_sessions[sid].resp_pending) {
        rep->mr[0] = CC_ERR_NO_RESPONSE;
        rep->mr[1] = 0u;
        return;
    }

    uint32_t n = g_sessions[sid].resp_len;
    if (n > max) n = max;
    if (n > CC_WIRE_SHMEM_SIZE) n = CC_WIRE_SHMEM_SIZE;
    if (n > 0u) {
        __builtin_memcpy(rep->shmem, g_sessions[sid].resp, n);
    }

    g_sessions[sid].resp_pending = 0u;
    g_sessions[sid].resp_len = 0u;
    g_sessions[sid].state = CC_SESSION_STATE_IDLE;
    g_sessions[sid].ticks_since_active = 0u;

    rep->mr[0] = CC_OK;
    rep->mr[1] = n;
}

static void handle_status(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    uint32_t sid = req->mr[0]; /* session_id in MR1 */
    if (!valid_session(sid)) {
        rep->mr[0] = CC_ERR_BAD_SESSION;
        rep->mr[1] = 0u;
        rep->mr[2] = 0u;
        rep->mr[3] = 0u;
        return;
    }
    rep->mr[0] = CC_OK;
    rep->mr[1] = g_sessions[sid].state;
    rep->mr[2] = g_sessions[sid].resp_pending ? 1u : 0u;
    rep->mr[3] = g_sessions[sid].ticks_since_active;
}

static void handle_list_sessions(cc_reply_wire_t *rep)
{
    cc_session_info_t *out = (cc_session_info_t *)rep->shmem;
    uint32_t count = 0u;

    for (uint32_t i = 0u; i < CC_MAX_SESSIONS; i++) {
        if (g_sessions[i].active) {
            out[count].session_id         = i;
            out[count].state              = g_sessions[i].state;
            out[count].client_badge       = g_sessions[i].client_badge;
            out[count].ticks_since_active = g_sessions[i].ticks_since_active;
            count++;
        }
    }
    rep->mr[0] = count;
}

/* ─── Relay stubs ────────────────────────────────────────────────────────── */
/*
 * Each handler below will call the corresponding downstream service PD via
 * seL4_Call once inter-PD endpoint wiring is complete (Phase 5).
 * For now they return CC_OK with empty/zero data so that external callers
 * (agentos_gui) get well-formed responses and can display an empty state.
 */

/* Query vibe_engine for the live guest list, filling cc_guest_info_t entries
 * starting at out[].  Returns the number of dynamic entries written.  The
 * boot guest is intentionally NOT included here — the caller emits it
 * separately at handle 0 so existing tests keep their layout assumption. */
static uint32_t cc_relay_vibe_list(cc_guest_info_t *out, uint32_t max_out)
{
    if (max_out == 0u) return 0u;

    sel4_msg_t lreq = {0};
    sel4_msg_t lrep = {0};
    lreq.opcode = MSG_VIBEOS_LIST;
    lreq.length = 4u;
    cc_wire_wr32(lreq.data, 0u, 0u);  /* offset = 0 */

    sel4_call((seL4_CPtr)PD_CNODE_SLOT_VIBE_ENGINE_EP, &lreq, &lrep);
    if (lrep.opcode != SEL4_ERR_OK) return 0u;

    uint32_t vos_count = cc_wire_rd32(lrep.data, 0u);
    if (vos_count > max_out) vos_count = max_out;

    uint32_t written = 0u;
    for (uint32_t i = 0u; i < vos_count; i++) {
        uint32_t handle = cc_wire_rd32(lrep.data, 4u + i * 4u);

        /* Fetch full status to get state and os_type.  arch is not tracked
         * by vibe_engine, so default to the build's native arch. */
        sel4_msg_t sreq = {0};
        sel4_msg_t srep = {0};
        sreq.opcode = MSG_VIBEOS_STATUS;
        sreq.length = 4u;
        cc_wire_wr32(sreq.data, 0u, handle);
        sel4_call((seL4_CPtr)PD_CNODE_SLOT_VIBE_ENGINE_EP, &sreq, &srep);

        uint32_t vos_state = 4u;  /* default to DEAD if status fails */
        uint32_t os_type   = 0u;
        if (srep.opcode == SEL4_ERR_OK) {
            vos_state = cc_wire_rd32(srep.data, 8u);
            os_type   = cc_wire_rd32(srep.data, 12u);
        }

        out[written].guest_handle = handle;
        out[written].state        = cc_vibeos_to_guest_state(vos_state);
        out[written].os_type      = os_type;
#if defined(__x86_64__)
        out[written].arch         = VIBEOS_ARCH_X86_64;
#else
        out[written].arch         = VIBEOS_ARCH_AARCH64;
#endif
        written++;
    }
    return written;
}

static void handle_list_guests(cc_reply_wire_t *rep)
{
    cc_guest_info_t *out = (cc_guest_info_t *)rep->shmem;
    uint32_t max_entries = (uint32_t)(CC_WIRE_SHMEM_SIZE / sizeof(cc_guest_info_t));
    uint32_t count = 0u;

#if defined(AGENTOS_GUEST_LINUX) || defined(AGENTOS_GUEST_FREEBSD)
    if (g_boot_guest_present && count < max_entries) {
        cc_fill_boot_guest_info(&out[count]);
        count++;
    }
#endif

    if (count < max_entries) {
        count += cc_relay_vibe_list(&out[count], max_entries - count);
    }

    rep->mr[0] = count;
}

static void handle_list_devices(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    uint32_t dev_type = req->mr[0];
    if (dev_type >= CC_DEV_TYPE_COUNT) {
        rep->mr[0] = 0u;
        return;
    }

#if defined(AGENTOS_GUEST_LINUX) || defined(AGENTOS_GUEST_FREEBSD)
    if (cc_boot_guest_has_device(dev_type)) {
        cc_device_info_t *out = (cc_device_info_t *)rep->shmem;
        out[0].dev_type   = dev_type;
        out[0].dev_handle = 0u;
        out[0].state      = 1u; /* active */
        out[0]._reserved  = 0u;
        rep->mr[0] = 1u;
        return;
    }
#endif

    rep->mr[0] = 0u;
}

/*
 * MSG_CC_LIST_POLECATS — report agent-worker (polecat) pool occupancy.
 *
 * DECISION (agentos-681): polecats track GENERIC AGENT work, not guest
 * workload.  The authoritative occupancy data lives in agent_pool.c's pool[]
 * array, inside the controller (monitor) PD.  A slot becomes busy when the
 * controller assigns a task via agent_pool_spawn() (WORKER_RUNNING/DONE) and
 * idle again after agent_pool_worker_done().  Guest OS lifecycle is tracked
 * separately by vibe_engine and is intentionally DECOUPLED from this metric:
 * spinning up a guest does not consume a polecat, and vice-versa.  So the
 * meaningful number here is live agent-worker occupancy, surfaced via the
 * controller's MSG_AGENTPOOL_STATUS handler (reply: MR0=total MR1=busy
 * MR2=idle MR3=faulted, mirroring agent_pool_occupancy()).
 *
 * cc_pd and the controller are separate PDs, so cc_pd must obtain live counts
 * over IPC — it cannot read pool[] directly.  The cc_pd→controller endpoint is
 * not wired yet (needs a PD_CNODE_SLOT + main.c distribution; tracked by
 * agentos-685).  Until then we relay through the placeholder slot below and,
 * if the relay is unavailable, fall back to reporting the pool as fully idle.
 * Reporting busy=0/idle=total as a *fallback* is correct-by-construction:
 * with no relay there is no observed load, and the moment wiring lands the
 * live busy count flows through unchanged.
 */
/* agentos-7j5: PD_CNODE_SLOT_CONTROLLER_EP is defined in system_desc.h (slot
 * 13) and the root task mints the controller's inbound server endpoint there
 * for cc_pd (see system_desc_aarch64.c: cc_pd init_eps + controller
 * self_svc_id = SVC_ID_CONTROLLER).  The #ifndef fallback below keeps the
 * relay compiling (and inert) on builds/arches that do not wire the slot. */
#ifndef PD_CNODE_SLOT_CONTROLLER_EP
#define PD_CNODE_SLOT_CONTROLLER_EP 0xFFFFFFFFu  /* unwired placeholder */
#endif

static void handle_list_polecats(cc_reply_wire_t *rep)
{
    uint32_t total = WORKER_POOL_SIZE;
    uint32_t busy  = 0u;
    uint32_t idle  = WORKER_POOL_SIZE;
    uint32_t faulted = 0u;

    if (PD_CNODE_SLOT_CONTROLLER_EP != 0xFFFFFFFFu) {
        sel4_msg_t req = {0};
        sel4_msg_t srep = {0};
        req.opcode = MSG_AGENTPOOL_STATUS;
        req.length = 0u;
        sel4_call((seL4_CPtr)PD_CNODE_SLOT_CONTROLLER_EP, &req, &srep);
        if (srep.opcode == SEL4_ERR_OK) {
            total   = cc_wire_rd32(srep.data, 0u);
            busy    = cc_wire_rd32(srep.data, 4u);
            idle    = cc_wire_rd32(srep.data, 8u);
            faulted = cc_wire_rd32(srep.data, 12u);
        }
    }

    rep->mr[0] = CC_OK;
    rep->mr[1] = total;
    rep->mr[2] = busy;
    rep->mr[3] = idle;
    (void)faulted;  /* contract MR slots carry total/busy/idle; faulted folds into busy upstream */
}

static void handle_guest_status(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    uint32_t handle = req->mr[0];

#if defined(AGENTOS_GUEST_LINUX) || defined(AGENTOS_GUEST_FREEBSD)
    if (handle == CC_BOOT_GUEST_HANDLE) {
        cc_guest_status_t *out = (cc_guest_status_t *)rep->shmem;
        cc_fill_boot_guest_status(out);
        rep->mr[0] = CC_OK;
        return;
    }
#endif

    /* Dynamic guest — relay to vibe_engine. */
    sel4_msg_t sreq = {0};
    sel4_msg_t srep = {0};
    sreq.opcode = MSG_VIBEOS_STATUS;
    sreq.length = 4u;
    cc_wire_wr32(sreq.data, 0u, handle);
    sel4_call((seL4_CPtr)PD_CNODE_SLOT_VIBE_ENGINE_EP, &sreq, &srep);
    if (srep.opcode != SEL4_ERR_OK) {
        rep->mr[0] = CC_ERR_BAD_HANDLE;
        return;
    }

    cc_guest_status_t *out = (cc_guest_status_t *)rep->shmem;
    out->guest_handle = handle;
    out->state        = cc_vibeos_to_guest_state(cc_wire_rd32(srep.data, 8u));
    out->os_type      = cc_wire_rd32(srep.data, 12u);
#if defined(__x86_64__)
    out->arch         = VIBEOS_ARCH_X86_64;
#else
    out->arch         = VIBEOS_ARCH_AARCH64;
#endif
    out->device_flags = cc_wire_rd32(srep.data, 20u);
    for (uint32_t i = 0u; i < 3u; i++) out->_reserved[i] = 0u;
    rep->mr[0] = CC_OK;
}

static void handle_device_status(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    uint32_t dev_type = req->mr[0];
    uint32_t dev_handle = req->mr[1];
    if (dev_type >= CC_DEV_TYPE_COUNT) {
        rep->mr[0] = CC_ERR_BAD_DEV_TYPE;
        return;
    }

#if defined(AGENTOS_GUEST_LINUX) || defined(AGENTOS_GUEST_FREEBSD)
    if (dev_handle == 0u && cc_boot_guest_has_device(dev_type)) {
        cc_device_info_t *out = (cc_device_info_t *)rep->shmem;
        out->dev_type = dev_type;
        out->dev_handle = dev_handle;
        out->state = 1u; /* active */
        out->_reserved = 0u;
        rep->mr[0] = CC_OK;
        return;
    }
#else
    (void)dev_handle;
#endif

    rep->mr[0] = CC_ERR_BAD_HANDLE;
}

static void handle_attach_framebuffer(const cc_req_wire_t *req,
                                       cc_reply_wire_t *rep)
{
    (void)req;
    /* Phase 5: seL4_Call(framebuffer_ep, MSG_FB_FLIP, fb_handle) */
    rep->mr[0] = CC_ERR_BAD_HANDLE;
    rep->mr[1] = 0u;
}

static void handle_send_input(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
#if defined(AGENTOS_GUEST_LINUX) || defined(AGENTOS_GUEST_FREEBSD)
    const cc_input_event_t *event = (const cc_input_event_t *)req->shmem;
    uint32_t text_len = event->event_type == CC_INPUT_TEXT ? event->keycode : 0u;
    if (text_len > CC_INPUT_TEXT_MAX ||
        sizeof(cc_input_event_t) + text_len > sizeof(req->shmem)) {
        rep->mr[0] = CC_ERR_BAD_HANDLE;
        return;
    }
    const uint8_t *text = req->shmem + sizeof(cc_input_event_t);
    if (req->mr[0] == CC_BOOT_GUEST_HANDLE) {
        rep->mr[0] = cc_forward_boot_guest_input(event, text, text_len)
                     ? CC_OK : CC_ERR_RELAY_FAULT;
        return;
    }

    rep->mr[0] = cc_forward_vibe_input(req->mr[0], event, text, text_len)
                 ? CC_OK : CC_ERR_BAD_HANDLE;
    return;
#else
    (void)req;
#endif
    rep->mr[0] = CC_ERR_BAD_HANDLE;
}

static void handle_snapshot(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    uint32_t handle = req->mr[0];

#if defined(AGENTOS_GUEST_LINUX) || defined(AGENTOS_GUEST_FREEBSD)
    if (handle == CC_BOOT_GUEST_HANDLE) {
        /* Boot guest snapshot is not implemented yet; surface a clear error
         * rather than the previous unconditional relay fault. */
        rep->mr[0] = CC_ERR_RELAY_FAULT;
        rep->mr[1] = 0u;
        rep->mr[2] = 0u;
        return;
    }
#endif

    sel4_msg_t sreq = {0};
    sel4_msg_t srep = {0};
    sreq.opcode = MSG_VIBEOS_SNAPSHOT;
    sreq.length = 4u;
    cc_wire_wr32(sreq.data, 0u, handle);
    sel4_call((seL4_CPtr)PD_CNODE_SLOT_VIBE_ENGINE_EP, &sreq, &srep);
    if (srep.opcode != SEL4_ERR_OK) {
        rep->mr[0] = CC_ERR_RELAY_FAULT;
        rep->mr[1] = srep.opcode;  /* vibeos error in mr[1] for diagnostics */
        rep->mr[2] = 0u;
        return;
    }
    rep->mr[0] = CC_OK;
    rep->mr[1] = cc_wire_rd32(srep.data, 8u);   /* snap_lo */
    rep->mr[2] = cc_wire_rd32(srep.data, 12u);  /* snap_hi */
}

static void handle_restore(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    uint32_t handle = req->mr[0];

#if defined(AGENTOS_GUEST_LINUX) || defined(AGENTOS_GUEST_FREEBSD)
    if (handle == CC_BOOT_GUEST_HANDLE) {
        rep->mr[0] = CC_ERR_RELAY_FAULT;
        return;
    }
#endif

    sel4_msg_t sreq = {0};
    sel4_msg_t srep = {0};
    sreq.opcode = MSG_VIBEOS_RESTORE;
    sreq.length = 12u;
    cc_wire_wr32(sreq.data, 0u, handle);
    cc_wire_wr32(sreq.data, 4u, req->mr[1]);  /* snap_lo */
    cc_wire_wr32(sreq.data, 8u, req->mr[2]);  /* snap_hi */
    sel4_call((seL4_CPtr)PD_CNODE_SLOT_VIBE_ENGINE_EP, &sreq, &srep);
    if (srep.opcode != SEL4_ERR_OK) {
        rep->mr[0] = CC_ERR_RELAY_FAULT;
        rep->mr[1] = srep.opcode;
        return;
    }
    rep->mr[0] = CC_OK;
}

/*
 * MSG_CC_LOG_STREAM — drain a guest's serial output as ASCII bytes (agentos-vsi).
 *
 * Wire args: MR1 = slot, MR2 = pd_id.  Reply: MR0 = CC_OK, MR1 = byte length,
 * shmem = the drained ASCII bytes.  MR2 echoes the resolved log slot so the
 * caller can re-address the same stream on subsequent polls.
 *
 *   slot 0,  pd_id TRACE_PD_CONTROLLER  → boot guest serial (guest_vmm drain)
 *   slot 0,  pd_id LINUX/FREEBSD_VMM    → vibe guest addressed by MR1==handle,
 *                                          assigned its own slot (1..N) on use
 *   slot N>0                            → previously assigned vibe guest slot
 *
 * The boot guest always occupies slot 0; each vibe_engine guest gets its own
 * slot from g_log_slots[] so it is independently addressable by handle.
 */
static void handle_log_stream(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
#if defined(AGENTOS_GUEST_LINUX) || defined(AGENTOS_GUEST_FREEBSD)
    uint32_t slot  = req->mr[0];
    uint32_t pd_id = req->mr[1];

    /* Slot 0 + controller tag: the boot guest's serial stream. */
    if (slot == CC_LOG_SLOT_BOOT && pd_id == TRACE_PD_CONTROLLER) {
        uint32_t drained = 0u;
        if (!cc_drain_boot_guest_console(rep->shmem, CC_WIRE_SHMEM_SIZE,
                                         &drained)) {
            rep->mr[0] = CC_ERR_RELAY_FAULT;
            rep->mr[1] = 0u;
            return;
        }
        rep->mr[0] = CC_OK;
        rep->mr[1] = drained;
        rep->mr[2] = CC_LOG_SLOT_BOOT;
        return;
    }

    /* Vibe guest streams.  An already-allocated slot (slot>0, in_use) resolves
     * straight to its guest handle; otherwise treat MR1 as a vibe handle and
     * assign it a fresh slot. */
    if (pd_id == TRACE_PD_LINUX_VMM ||
        pd_id == TRACE_PD_FREEBSD_VMM ||
        (slot > 0u && slot < CC_LOG_SLOTS && g_log_slots[slot].in_use)) {

        uint32_t guest_handle;
        uint32_t assigned;
        if (slot > 0u && slot < CC_LOG_SLOTS && g_log_slots[slot].in_use) {
            guest_handle = g_log_slots[slot].guest_handle;
            assigned     = slot;
        } else {
            guest_handle = slot;  /* caller passed the vibe handle in MR1 */
            assigned     = cc_log_slot_for_handle(guest_handle, pd_id);
            if (assigned == CC_LOG_SLOT_INVALID) {
                rep->mr[0] = CC_ERR_NO_SESSIONS;  /* slot table exhausted */
                rep->mr[1] = 0u;
                return;
            }
        }

        uint32_t drained = 0u;
        if (!cc_drain_vibe_console(guest_handle, rep->shmem,
                                   CC_WIRE_SHMEM_SIZE, &drained)) {
            rep->mr[0] = CC_ERR_BAD_HANDLE;
            rep->mr[1] = 0u;
            return;
        }
        rep->mr[0] = CC_OK;
        rep->mr[1] = drained;
        rep->mr[2] = assigned;
        return;
    }
#else
    (void)req;
#endif

    rep->mr[0] = CC_OK;
    rep->mr[1] = 0u;
}

static void handle_create_guest(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    /*
     * Relay CC_CREATE_GUEST → VIBEOS_CREATE.  The CC request shmem matches
     * vibeos_create_req (os_type/arch in bytes 0..3, ram_mb at 4, device_flags
     * at 16).  vibe_engine's MSG_VIBEOS_CREATE handler reads a flat 12-byte
     * payload at offsets 0,4,8 (os_type, ram_mb, dev_flags), so we repack.
     */
    uint8_t  os_type  = req->shmem[0];
    uint32_t ram_mb   = cc_wire_rd32(req->shmem, 4u);
    uint32_t dev_mask = cc_wire_rd32(req->shmem, 16u);

    sel4_msg_t msg   = {0};
    sel4_msg_t reply = {0};
    msg.opcode = MSG_VIBEOS_CREATE;
    msg.length = 12u;
    cc_wire_wr32(msg.data, 0u, (uint32_t)os_type);
    cc_wire_wr32(msg.data, 4u, ram_mb);
    cc_wire_wr32(msg.data, 8u, dev_mask);

    sel4_call((seL4_CPtr)PD_CNODE_SLOT_VIBE_ENGINE_EP, &msg, &reply);
    /* sel4_server propagates the handler's return code via reply.opcode
     * (see sel4_server.h: rep->opcode = rc).  On success rc == SEL4_ERR_OK == 0;
     * on failure the VIBEOS_ERR_* code (12 = BAD_TYPE, 13 = OOM, etc.) appears
     * here, while reply.data[0] is the same code echoed by handle_vos_create. */
    if (reply.opcode != SEL4_ERR_OK) {
        rep->mr[0] = CC_ERR_RELAY_FAULT;
        rep->mr[1] = reply.opcode;  /* surface vibeos error for diagnostics */
        return;
    }

    rep->mr[0] = CC_OK;
    rep->mr[1] = cc_wire_rd32(reply.data, 4u); /* guest_handle */
}

static void handle_fault_inject(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
#if defined(AGENTOS_FAULT_INJECT)
    sel4_msg_t msg = {0};
    sel4_msg_t reply = {0};
    msg.opcode = OP_FAULT_INJECT;
    msg.length = 12u;
    cc_wire_wr32(msg.data, 0u, req->mr[0]); /* slot_id */
    cc_wire_wr32(msg.data, 4u, req->mr[1]); /* fault_kind */
    cc_wire_wr32(msg.data, 8u, req->mr[2]); /* flags */

    sel4_call((seL4_CPtr)PD_CNODE_SLOT_FAULT_INJECT_EP, &msg, &reply);
    if (reply.opcode != SEL4_ERR_OK) {
        rep->mr[0] = CC_ERR_RELAY_FAULT;
        return;
    }

    rep->mr[0] = CC_OK;
    rep->mr[1] = msg_u32(&reply, 0u);  /* fault result */
    rep->mr[2] = msg_u32(&reply, 4u);  /* ticks_to_recovery */
    rep->mr[3] = msg_u32(&reply, 8u);  /* trace_event_id */
#else
    (void)req;
    rep->mr[0] = CC_ERR_RELAY_FAULT;
#endif
}

static void handle_suspend_guest(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    uint32_t handle = req->mr[0];

#if defined(AGENTOS_GUEST_LINUX) || defined(AGENTOS_GUEST_FREEBSD)
    if (handle == CC_BOOT_GUEST_HANDLE) {
        if (!g_boot_guest_present) {
            rep->mr[0] = CC_ERR_BAD_HANDLE;
            rep->mr[1] = 0u;
            return;
        }
        uint32_t state = 0u;
        if (!cc_lifecycle_boot_guest(MSG_GUEST_SUSPEND, 0u, &state)) {
            rep->mr[0] = CC_ERR_RELAY_FAULT;
            rep->mr[1] = 0u;
            return;
        }
        rep->mr[0] = CC_OK;
        rep->mr[1] = state;
        return;
    }
#endif

    uint32_t state = 0u;
    if (!cc_lifecycle_vibeos_guest(MSG_VIBEOS_SUSPEND, handle, &state)) {
        rep->mr[0] = CC_ERR_RELAY_FAULT;
        rep->mr[1] = 0u;
        return;
    }
    rep->mr[0] = CC_OK;
    rep->mr[1] = state;
}

static void handle_resume_guest(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    uint32_t handle = req->mr[0];

#if defined(AGENTOS_GUEST_LINUX) || defined(AGENTOS_GUEST_FREEBSD)
    if (handle == CC_BOOT_GUEST_HANDLE) {
        if (!g_boot_guest_present) {
            rep->mr[0] = CC_ERR_BAD_HANDLE;
            rep->mr[1] = 0u;
            return;
        }
        uint32_t state = 0u;
        if (!cc_lifecycle_boot_guest(MSG_GUEST_RESUME, 0u, &state)) {
            rep->mr[0] = CC_ERR_RELAY_FAULT;
            rep->mr[1] = 0u;
            return;
        }
        rep->mr[0] = CC_OK;
        rep->mr[1] = state;
        return;
    }
#endif

    uint32_t state = 0u;
    if (!cc_lifecycle_vibeos_guest(MSG_VIBEOS_RESUME, handle, &state)) {
        rep->mr[0] = CC_ERR_RELAY_FAULT;
        rep->mr[1] = 0u;
        return;
    }
    rep->mr[0] = CC_OK;
    rep->mr[1] = state;
}

static void handle_destroy_guest(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    uint32_t handle = req->mr[0];

#if defined(AGENTOS_GUEST_LINUX) || defined(AGENTOS_GUEST_FREEBSD)
    if (handle == CC_BOOT_GUEST_HANDLE) {
        if (!g_boot_guest_present) {
            rep->mr[0] = CC_ERR_BAD_HANDLE;
            return;
        }
        uint32_t state = 0u;
        if (!cc_lifecycle_boot_guest(MSG_GUEST_DESTROY, req->mr[1], &state)) {
            rep->mr[0] = CC_ERR_RELAY_FAULT;
            return;
        }
        (void)state;
        rep->mr[0] = CC_OK;
        return;
    }
#endif

    /* Dynamic guest — relay to vibe_engine MSG_VIBEOS_DESTROY. */
    sel4_msg_t sreq = {0};
    sel4_msg_t srep = {0};
    sreq.opcode = MSG_VIBEOS_DESTROY;
    sreq.length = 4u;
    cc_wire_wr32(sreq.data, 0u, handle);
    sel4_call((seL4_CPtr)PD_CNODE_SLOT_VIBE_ENGINE_EP, &sreq, &srep);
    if (srep.opcode != SEL4_ERR_OK) {
        rep->mr[0] = CC_ERR_RELAY_FAULT;
        return;
    }
    rep->mr[0] = CC_OK;
}

static void handle_trace_start(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    (void)req;
    cc_trace_reset(true);
    rep->mr[0] = CC_OK;
}

static void handle_trace_stop(cc_reply_wire_t *rep)
{
    g_cc_trace_recording = false;
    rep->mr[0] = CC_OK;
    rep->mr[1] = g_cc_trace_seq;
}

static void handle_trace_query(cc_reply_wire_t *rep)
{
    rep->mr[0] = CC_OK;
    rep->mr[1] = g_cc_trace_count;
    rep->mr[2] = g_cc_trace_count * (uint32_t)sizeof(cc_trace_entry_t);
    rep->mr[3] = g_cc_trace_overflow;
}

static void handle_trace_dump(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    uint32_t max = req->mr[0];
    if (max == 0u || max > g_cc_trace_count) max = g_cc_trace_count;

    uint32_t capacity = CC_WIRE_SHMEM_SIZE / (uint32_t)sizeof(cc_trace_entry_t);
    if (max > capacity) max = capacity;

    cc_trace_entry_t *out = (cc_trace_entry_t *)rep->shmem;
    uint32_t start = (g_cc_trace_head + CC_TRACE_RING_ENTRIES -
                      g_cc_trace_count) % CC_TRACE_RING_ENTRIES;
    for (uint32_t i = 0u; i < max; i++) {
        out[i] = g_cc_trace_ring[(start + i) % CC_TRACE_RING_ENTRIES];
    }

    rep->mr[0] = CC_OK;
    rep->mr[1] = max;
    rep->mr[2] = max * (uint32_t)sizeof(cc_trace_entry_t);
    rep->mr[3] = g_cc_trace_overflow;
}

/* ─── Dispatch ───────────────────────────────────────────────────────────── */

static void cc_dispatch(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    /* Age active sessions before dispatch.  Handlers that touch a specific
     * session (CONNECT/SEND/RECV/STATUS) reset that slot's tick counter back
     * to 0 inside themselves, so the net effect is: every session except the
     * one being touched ages by one tick per dispatch.  alloc_session uses
     * this to identify abandoned sessions when the table is full. */
    cc_age_sessions();

    switch (req->opcode) {
    /* Session management */
    case MSG_CC_CONNECT:    handle_connect(req, rep);          break;
    case MSG_CC_DISCONNECT: handle_disconnect(req, rep);       break;
    case MSG_CC_SEND:       handle_send(req, rep);             break;
    case MSG_CC_RECV:       handle_recv(req, rep);             break;
    case MSG_CC_STATUS:     handle_status(req, rep);           break;
    case MSG_CC_LIST:       handle_list_sessions(rep);         break;

    /* Relay API */
    case MSG_CC_LIST_GUESTS:        handle_list_guests(rep);             break;
    case MSG_CC_LIST_DEVICES:       handle_list_devices(req, rep);       break;
    case MSG_CC_LIST_POLECATS:      handle_list_polecats(rep);           break;
    case MSG_CC_GUEST_STATUS:       handle_guest_status(req, rep);       break;
    case MSG_CC_DEVICE_STATUS:      handle_device_status(req, rep);      break;
    case MSG_CC_ATTACH_FRAMEBUFFER: handle_attach_framebuffer(req, rep); break;
    case MSG_CC_SEND_INPUT:         handle_send_input(req, rep);         break;
    case MSG_CC_SNAPSHOT:           handle_snapshot(req, rep);           break;
    case MSG_CC_RESTORE:            handle_restore(req, rep);            break;
    case MSG_CC_LOG_STREAM:         handle_log_stream(req, rep);         break;
    case MSG_CC_CREATE_GUEST:       handle_create_guest(req, rep);       break;
    case MSG_CC_FAULT_INJECT:       handle_fault_inject(req, rep);       break;
    case MSG_CC_SUSPEND_GUEST:      handle_suspend_guest(req, rep);      break;
    case MSG_CC_RESUME_GUEST:       handle_resume_guest(req, rep);       break;
    case MSG_CC_DESTROY_GUEST:      handle_destroy_guest(req, rep);      break;
    case MSG_CC_TRACE_START:        handle_trace_start(req, rep);        break;
    case MSG_CC_TRACE_STOP:         handle_trace_stop(rep);              break;
    case MSG_CC_TRACE_QUERY:        handle_trace_query(rep);             break;
    case MSG_CC_TRACE_DUMP:         handle_trace_dump(req, rep);         break;

    default:
        sel4_dbg_puts("[cc_pd] unknown opcode\n");
        rep->mr[0] = CC_ERR_BAD_SESSION;
        break;
    }

    cc_trace_record(req->opcode);
}

/* ─── Entry point ────────────────────────────────────────────────────────── */

void cc_pd_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    (void)my_ep;
    (void)ns_ep;

    /* Initialise session table */
    for (uint32_t i = 0u; i < CC_MAX_SESSIONS; i++) {
        g_sessions[i].active = false;
        g_sessions[i].state  = CC_SESSION_STATE_IDLE;
        g_sessions[i].resp_pending = 0u;
        g_sessions[i].resp_len = 0u;
    }

    /* Initialise VirtIO MMIO serial (mapped by root task; VQ PAs in startup record) */
    (void)virtio_serial_init();

    /* Static buffers live in BSS — kept off the stack since each frame is
     * 4112 bytes, which would exhaust cc_pd's 16 KB stack otherwise.    */
    static cc_req_wire_t   g_req;
    static cc_reply_wire_t g_rep;
    static cc_retry_cache_t g_retry;
    cc_retry_cache_init(&g_retry);

    while (1) {
        if (!vio_serial_read(&g_req, sizeof(g_req))) {
            continue;
        }
        if (!cc_retry_cache_replay(&g_retry, &g_req, &g_rep)) {
            __builtin_memset(&g_rep, 0, sizeof(g_rep));
            cc_dispatch(&g_req, &g_rep);
        }
        if (!vio_serial_write(&g_rep, sizeof(g_rep))) {
            /*
             * The operation may already have changed state. Save the exact
             * request/reply pair before resetting the poisoned TX queue; a
             * reconnecting host can retry without executing it twice.
             */
            cc_retry_cache_record(&g_retry, &g_req, &g_rep);
            virtio_serial_recover_tx();
        }
    }
}

void pd_main(seL4_CPtr my_ep, seL4_CPtr ns_ep) { cc_pd_main(my_ep, ns_ep); }
