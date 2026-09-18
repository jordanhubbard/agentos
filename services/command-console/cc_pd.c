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
 * Device/resource policy belongs to canonical services. CC translates public
 * guest handles to vm_manager slots, validates wire requests and replies, and
 * propagates lifecycle failures. It owns no guest device allocation policy.
 *
 * Priority: 164
 * Mode: IRQ wait for idle AArch64 RX; bounded polling for TX/partial RX.
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
#include "cc_vm_client.h"
#include "contracts/vm_manager_contract.h"
#include "sel4_ipc.h"
#include "sel4_boot.h"
#include "serial_log.h"
#include "serial_virt_client.h"
#include <platform/serial_frontend.h>
#include <platform/serial_virt_layout.h>
#include <platform/console_input.h>
#include <platform/input.h>
#include <platform/inspect.h>
#include <platform/operator_session.h>
#include <platform/framebuffer_observer.h>
#include <platform/virtio_host_transport.h>
#include "system_desc.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ─── VirtIO console driver ──────────────────────────────────────────────── */
/*
 * Transport: virtio-serial-device on QEMU virtio-mmio-bus.2 (PA 0x0A000400).
 * QEMU bridges the virtconsole named "cc.0" to build/cc_pd.sock. A validated
 * x86 PCI startup record selects the shared modern PCI transport instead.
 *
 * The root task allocates three 4K frames and maps them at fixed CPU virtual
 * addresses in cc_pd. A versioned startup record at CC_VIRTIO_STARTUP_VA
 * separately carries their device-visible physical addresses:
 *   [0] VQ struct page — descriptor tables, avail/used rings for TX+RX queues
 *   [1] TX data buffer
 *   [2] RX data buffer
 *
 * We use one descriptor chain per queue (VQ_DEPTH=4 slots, one in flight at a
 * time). Idle AArch64 RX waits for the owned device IRQ; TX and partial RX
 * poll with bounded seL4_Yield retries.
 *
 * Each 4112-byte wire frame uses a 4096-byte descriptor plus a 16-byte tail.
 * The protocol is strictly sequential (one reply per request).
 */

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

#define VSTATUS_ACK       1u
#define VSTATUS_DRIVER    2u
#define VSTATUS_FEAT_OK   8u
#define VSTATUS_DRIVER_OK 4u
#define VSTATUS_FAILED    128u
#define VIRTIO_ID_CONSOLE 3u
#define VQ_DEPTH          4u
#define CC_VIRTIO_RX_WAIT_LIMIT 16384u
/* A queue kick can be deferred while QEMU services a busy guest.  Keep TX
 * bounded below the host's frame deadline, but do not reset a healthy queue
 * after only a few hundred scheduler yields. */
#define CC_VIRTIO_TX_WAIT_LIMIT 16384u
#define CC_VIRTIO_RENOTIFY_INTERVAL 64u

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
#define TX_TAIL_OFF   1024u /* second descriptor payload, within queue page */
#define RX_TAIL_OFF   1088u /* second descriptor payload, within queue page */
#define VQ_PAGE_BYTES 4096u
#define VQ_TAIL_BYTES 64u
#define VQ_DESC_F_NEXT  1u
#define VQ_DESC_F_WRITE 2u

static seL4_Word          g_vq_pa[3];       /* [0]=structs, [1]=TX buf, [2]=RX buf */
static aos_virtio_host_t g_transport;
static aos_virtio_host_queue_t g_transport_queues[2];
static bool g_transport_ready;
static uint16_t           g_rx_used_last;   /* shadow of RX used ring consumer idx */

#define QP       ((uintptr_t)CC_VIRTIO_QUEUE_VA)
#define TX_BUFFER ((void *)(uintptr_t)CC_VIRTIO_TX_BUFFER_VA)
#define RX_BUFFER ((void *)(uintptr_t)CC_VIRTIO_RX_BUFFER_VA)
#define TX_TAIL_BUFFER ((void *)(QP + TX_TAIL_OFF))
#define RX_TAIL_BUFFER ((void *)(QP + RX_TAIL_OFF))
#define TX_DESC  ((volatile vq_desc_t  *)(QP + TX_DESC_OFF))
#define TX_AVAIL ((volatile vq_avail_t *)(QP + TX_AVAIL_OFF))
#define TX_USED  ((volatile vq_used_t  *)(QP + TX_USED_OFF))
#define RX_DESC  ((volatile vq_desc_t  *)(QP + RX_DESC_OFF))
#define RX_AVAIL ((volatile vq_avail_t *)(QP + RX_AVAIL_OFF))
#define RX_USED  ((volatile vq_used_t  *)(QP + RX_USED_OFF))

#if defined(__aarch64__)
#define VQ_MB() __asm__ volatile("dsb sy" ::: "memory")
#elif defined(__riscv)
#define VQ_MB() __asm__ volatile("fence rw,rw" ::: "memory")
#elif defined(__x86_64__)
#define VQ_MB() __asm__ volatile("mfence" ::: "memory")
#else
#define VQ_MB() __asm__ volatile("" ::: "memory")
#endif

static bool vio_queue_setup(uint32_t qidx,
                             seL4_Word desc_pa, seL4_Word avail_pa, seL4_Word used_pa)
{
    return qidx < 2u && aos_virtio_host_queue_bind(&g_transport,
        &g_transport_queues[qidx], (uint16_t)qidx, VQ_DEPTH,
        desc_pa, avail_pa, used_pa);
}

static bool virtio_serial_init(void)
{
    g_transport_ready = false;
    __builtin_memset(g_transport_queues, 0, sizeof(g_transport_queues));
    const volatile cc_virtio_startup_t *sp =
        (const volatile cc_virtio_startup_t *)CC_VIRTIO_STARTUP_VA;
    const cc_virtio_startup_t startup = *sp;
    bool bound = false;
    if (cc_virtio_startup_valid(&startup, CC_VIRTIO_STARTUP_VERSION)) {
        bound = aos_virtio_host_mmio(&g_transport,
            CC_VIRTIO_MMIO_VA + VMMIO_SLOT_OFF, 0x200u, VIRTIO_ID_CONSOLE);
    }
#if defined(__x86_64__)
    else if (startup.version == CC_VIRTIO_STARTUP_PCI_VERSION) {
        const cc_virtio_pci_startup_t pci =
            *(const volatile cc_virtio_pci_startup_t *)CC_VIRTIO_STARTUP_VA;
        if (cc_virtio_pci_startup_valid(&pci)) {
            bound = aos_virtio_host_pci(&g_transport,
                CC_VIRTIO_PCI_VA + pci.offset[CC_VIRTIO_PCI_COMMON],
                pci.length[CC_VIRTIO_PCI_COMMON],
                CC_VIRTIO_PCI_VA + 2u * CC_VIRTIO_PAGE_BYTES + pci.offset[CC_VIRTIO_PCI_DEVICE],
                pci.length[CC_VIRTIO_PCI_DEVICE],
                CC_VIRTIO_PCI_VA + CC_VIRTIO_PAGE_BYTES + pci.offset[CC_VIRTIO_PCI_NOTIFY],
                pci.length[CC_VIRTIO_PCI_NOTIFY], pci.notify_multiplier);
        }
    }
#endif
    if (!bound) {
        cc_dbg_puts("[cc_pd] VirtIO init FAILED: bad startup record\n");
        return false;
    }
    g_vq_pa[0] = (seL4_Word)startup.queue_pa;
    g_vq_pa[1] = (seL4_Word)startup.tx_buffer_pa;
    g_vq_pa[2] = (seL4_Word)startup.rx_buffer_pa;

    /* VirtIO 1.0 initialisation sequence */
    aos_virtio_host_set_status(&g_transport, 0u);
    aos_virtio_host_set_status(&g_transport, VSTATUS_ACK);
    aos_virtio_host_set_status(&g_transport, VSTATUS_ACK | VSTATUS_DRIVER);
    /* Only VERSION_1 is implemented. In particular, EVENT_IDX requires
     * publishing used_event thresholds; accepting it with a fixed zero
     * threshold suppresses completion interrupts after the first event. */
    uint32_t feat1 = aos_virtio_host_features(&g_transport, 1u);
    if (!(feat1 & 1u)) {
        aos_virtio_host_set_status(&g_transport, VSTATUS_FAILED);
        return false;
    }
    aos_virtio_host_set_features(&g_transport, 0u, 0u);
    aos_virtio_host_set_features(&g_transport, 1u, 1u); /* VERSION_1 */
    aos_virtio_host_set_status(&g_transport, VSTATUS_ACK | VSTATUS_DRIVER | VSTATUS_FEAT_OK);
    uint32_t s_after = aos_virtio_host_status(&g_transport);
    cc_dbg_puts("[cc_pd] STATUS after FEAT_OK write="); cc_dbg_hex(s_after); cc_dbg_puts("\n");
    if (!(s_after & VSTATUS_FEAT_OK)) {
        cc_dbg_puts("[cc_pd] VirtIO FEAT_OK not set\n");
        aos_virtio_host_set_status(&g_transport, VSTATUS_FAILED);
        return false;
    }

    /* Device reset invalidates every in-flight descriptor. Start both rings
     * from a known epoch before making them ready again. */
    __builtin_memset((void *)QP, 0, 4096u);
    VQ_MB();
    if (!vio_queue_setup(0u,
            g_vq_pa[0] + RX_DESC_OFF, g_vq_pa[0] + RX_AVAIL_OFF, g_vq_pa[0] + RX_USED_OFF) ||
        !vio_queue_setup(1u,
            g_vq_pa[0] + TX_DESC_OFF, g_vq_pa[0] + TX_AVAIL_OFF, g_vq_pa[0] + TX_USED_OFF)) {
        aos_virtio_host_set_status(&g_transport, VSTATUS_FAILED);
        return false;
    }

    aos_virtio_host_set_status(&g_transport,
           VSTATUS_ACK | VSTATUS_DRIVER | VSTATUS_FEAT_OK | VSTATUS_DRIVER_OK);

    /*
     * A CC wire frame is 4112 bytes. Post the page and its 16-byte tail as
     * one descriptor chain before the host writes anything; sequentially
     * reposting a single 4096-byte descriptor can strand the already-buffered
     * tail because a socket chardev does not emit another readability event.
     */
    RX_DESC[0].addr  = (uint64_t)g_vq_pa[2];
    RX_DESC[0].len   = VQ_PAGE_BYTES;
    RX_DESC[0].flags = VQ_DESC_F_WRITE | VQ_DESC_F_NEXT;
    RX_DESC[0].next  = 1u;
    RX_DESC[1].addr  = (uint64_t)(g_vq_pa[0] + RX_TAIL_OFF);
    RX_DESC[1].len   = VQ_TAIL_BYTES;
    RX_DESC[1].flags = VQ_DESC_F_WRITE;
    RX_DESC[1].next  = 0u;
    VQ_MB();
    RX_AVAIL->ring[0] = 0u;
    VQ_MB();
    RX_AVAIL->idx = 1u;
    VQ_MB();
    if (!aos_virtio_host_queue_notify(&g_transport, &g_transport_queues[0])) return false;
    g_rx_used_last = 0u;
    g_transport_ready = true;

    cc_dbg_puts("[cc_pd] VirtIO serial ready\n");
    return true;
}

static void virtio_serial_recover_tx(void)
{
    cc_dbg_puts("[cc_pd] resetting VirtIO serial after incomplete reply\n");
    g_transport_ready = false;
    aos_virtio_host_set_status(&g_transport, VSTATUS_FAILED);
    VQ_MB();
    aos_virtio_host_set_status(&g_transport, 0u);
    VQ_MB();
    if (!virtio_serial_init()) {
        cc_dbg_puts("[cc_pd] VirtIO serial recovery failed\n");
    }
}

static bool vio_serial_write(const void *buf, uint32_t n)
{
    if (!g_transport_ready) return false;
    const uint8_t *p = (const uint8_t *)buf;
    while (n > 0u) {
        uint32_t frame = n;
        if (frame > VQ_PAGE_BYTES + VQ_TAIL_BYTES) {
            frame = VQ_PAGE_BYTES + VQ_TAIL_BYTES;
        }
        uint32_t first = frame > VQ_PAGE_BYTES ? VQ_PAGE_BYTES : frame;
        uint32_t tail = frame - first;
        __builtin_memcpy(TX_BUFFER, p, first);
        if (tail > 0u) {
            __builtin_memcpy(TX_TAIL_BUFFER, p + first, tail);
        }
        VQ_MB();
        TX_DESC[0].addr  = (uint64_t)g_vq_pa[1];
        TX_DESC[0].len   = first;
        TX_DESC[0].flags = tail > 0u ? VQ_DESC_F_NEXT : 0u;
        TX_DESC[0].next  = tail > 0u ? 1u : 0u;
        TX_DESC[1].addr  = (uint64_t)(g_vq_pa[0] + TX_TAIL_OFF);
        TX_DESC[1].len   = tail;
        TX_DESC[1].flags = 0u;
        TX_DESC[1].next  = 0u;
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
        if (!aos_virtio_host_queue_notify(&g_transport, &g_transport_queues[1])) return false;
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
            /*
             * A socket-backed virtconsole can transiently defer a queue kick
             * while its chardev frontend changes writable state.  The
             * descriptor remains owned by the device, so periodically
             * re-notifying the same queue is idempotent and prompts QEMU to
             * rescan it instead of leaving the caller blocked indefinitely.
             */
            if ((wait % CC_VIRTIO_RENOTIFY_INTERVAL) == 0u) {
                if (!aos_virtio_host_queue_notify(&g_transport, &g_transport_queues[1])) return false;
            }
            if (wait >= CC_VIRTIO_TX_WAIT_LIMIT) {
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
        p += frame;
        n -= frame;
    }
    return true;
}

static bool vio_serial_read(void *buf, uint32_t n)
{
    if (!g_transport_ready) { seL4_Yield(); return false; }
    uint8_t *p = (uint8_t *)buf;
    const uint32_t total = n;
    while (n > 0u) {
        uint16_t cur;
        uint32_t wait = 0u;
        for (;;) {
            VQ_MB();
            cur = RX_USED->idx;
            if (cur != g_rx_used_last) { break; }
#if defined(__aarch64__)
            if (n == total) {
                /* No request has begun: sleep on the driver's persistent IRQ
                 * notification instead of forfeiting the MCS budget. Clear
                 * the device cause, unmask the IRQ, then recheck the ring;
                 * an arrival after that check leaves a pending notification.
                 * Partial frames retain the bounded polling recovery below. */
                uint32_t irq = aos_virtio_host_interrupt_status(&g_transport);
                if (irq) aos_virtio_host_interrupt_ack(&g_transport, irq);
                VQ_MB();
                if (seL4_IRQHandler_Ack(PD_IRQHANDLER_SLOT_BASE) == seL4_NoError) {
                    VQ_MB();
                    if (RX_USED->idx == g_rx_used_last) {
                        seL4_Word badge;
                        seL4_Wait(PD_CNODE_SLOT_CC_IRQ_WAIT, &badge);
                    }
                    continue;
                }
            }
#else
            (void)total;
#endif
            seL4_Yield();
            wait++;
            if (wait >= CC_VIRTIO_RX_WAIT_LIMIT) {
                cc_dbg_puts("[cc_pd] RX timeout waiting for used ring\n");
                return false;
            }
        }
        uint32_t got  = RX_USED->ring[g_rx_used_last & (uint16_t)(VQ_DEPTH - 1u)].len;
        VQ_MB();
        uint32_t first = got;
        if (first > n) first = n;
        if (first > VQ_PAGE_BYTES) first = VQ_PAGE_BYTES;
        __builtin_memcpy(p, RX_BUFFER, first);
        p += first;
        n -= first;
        uint32_t tail = got - first;
        if (tail > n) tail = n;
        if (tail > VQ_TAIL_BYTES) tail = VQ_TAIL_BYTES;
        if (tail > 0u) {
            __builtin_memcpy(p, RX_TAIL_BUFFER, tail);
            p += tail;
            n -= tail;
        }
        g_rx_used_last++;
        /* Re-post RX buffer */
        RX_DESC[0].addr  = (uint64_t)g_vq_pa[2];
        RX_DESC[0].len   = VQ_PAGE_BYTES;
        RX_DESC[0].flags = VQ_DESC_F_WRITE | VQ_DESC_F_NEXT;
        RX_DESC[0].next  = 1u;
        RX_DESC[1].addr  = (uint64_t)(g_vq_pa[0] + RX_TAIL_OFF);
        RX_DESC[1].len   = VQ_TAIL_BYTES;
        RX_DESC[1].flags = VQ_DESC_F_WRITE;
        RX_DESC[1].next  = 0u;
        VQ_MB();
        RX_AVAIL->ring[RX_AVAIL->idx & (uint16_t)(VQ_DEPTH - 1u)] = 0u;
        VQ_MB();
        RX_AVAIL->idx++;
        VQ_MB();
        if (!aos_virtio_host_queue_notify(&g_transport, &g_transport_queues[0])) return false;
    }
    return true;
}

/* ─── Wire frame types ───────────────────────────────────────────────────── */
/*
 * These match the wire layout consumed by the non-interactive agentctl CLI.
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
        return (uint8_t)TRACE_PD_VM_MANAGER;
    case MSG_CC_SEND_INPUT:
    case MSG_CC_SUSPEND_GUEST:
    case MSG_CC_RESUME_GUEST:
    case MSG_CC_DESTROY_GUEST:
#if defined(AGENTOS_GUEST_SECONDARY)
        return (uint8_t)TRACE_PD_GUEST_VMM_SECONDARY;
#elif defined(AGENTOS_GUEST_PRIMARY)
        return (uint8_t)TRACE_PD_GUEST_VMM_PRIMARY;
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
        return (uint8_t)(CH_VM_MANAGER & 0xffu);
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
 * set. LIST_GUESTS/GUEST_STATUS expose that running guest independently of
 * the monotonic registry used for explicitly created guests.
 */

#if defined(AGENTOS_GUEST_PRIMARY) || defined(AGENTOS_GUEST_SECONDARY)
#define CC_BOOT_GUEST_HANDLE 0u

/* Dual images defer both vCPUs to explicit vm_manager CREATE calls. They
 * have no pre-existing handle-zero guest to reserve in the inventory. */
#if defined(AGENTOS_GUEST_DUAL) || defined(AGENTOS_GUEST_MANAGED_BOOT)
static bool     g_boot_guest_present = false;
#else
static bool     g_boot_guest_present = true;
#endif
static uint32_t g_boot_guest_state = GUEST_STATE_RUNNING;
#endif

static cc_vm_client_t g_vm_client;
_Static_assert(CC_VM_PAYLOAD_BYTES == SEL4_MSG_DATA_BYTES,
               "CC VM transport payload must match seL4 framing");

static void cc_vm_rpc(uint32_t opcode, const uint8_t *data, uint32_t length,
                      cc_vm_reply_t *out, void *ctx)
{
    (void)ctx;
    sel4_msg_t req = {0}, rep = {0};
    req.opcode = opcode;
    req.length = length;
    __builtin_memcpy(req.data, data, length);
    sel4_call((seL4_CPtr)PD_CNODE_SLOT_VM_MANAGER_EP, &req, &rep);
    out->status = rep.opcode;
    out->length = rep.length;
    __builtin_memcpy(out->data, rep.data, sizeof(out->data));
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

#if defined(AGENTOS_GUEST_PRIMARY) || defined(AGENTOS_GUEST_SECONDARY)

static uint32_t cc_boot_guest_os_type(void)
{
#if defined(AGENTOS_GUEST_SECONDARY) && !defined(AGENTOS_GUEST_DUAL)
    return VIBEOS_PROFILE_SECONDARY;
#else
    return VIBEOS_PROFILE_PRIMARY;
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

#endif /* configured guest helpers */

static aos_serial_channel_t cc_serial_channels[AOS_SERIAL_CLIENTS];
static bool cc_serial_attached[AOS_SERIAL_CLIENTS];
static uint32_t cc_serial_input_reported;

static void cc_serial_init(void)
{
    for (uint32_t slot = 0; slot < AOS_SERIAL_CLIENTS; slot++) {
#if !defined(AGENTOS_GUEST_PRIMARY)
        if (slot == 0u) continue;
#endif
#if !defined(AGENTOS_GUEST_SECONDARY)
        if (slot == 1u) continue;
#endif
        cc_serial_channels[slot] = aos_serial_channel_at(AOS_SERIAL_SHMEM_VA +
            AOS_SERIAL_FRONTEND_FRAME * AOS_SERIAL_FRAME_SIZE +
            slot * AOS_SERIAL_FRONTEND_STRIDE);
        cc_serial_attached[slot] = serial_virt_client_attach(slot, SERIAL_VIRT_ROLE_FRONTEND);
        if (!cc_serial_attached[slot])
            cc_dbg_puts("[cc_pd] serial_virt frontend attachment failed\n");
    }
}

/* Public handles never become array indices. Check live lifecycle authority
 * before queue access; the mirrored shared state is advisory only. */
#if defined(AGENTOS_GUEST_PRIMARY) || defined(AGENTOS_GUEST_SECONDARY)
static bool cc_serial_slot(uint32_t handle, bool input, uint32_t *slot)
{
    uint32_t state;
    if (handle == CC_BOOT_GUEST_HANDLE) {
        if (!g_boot_guest_present) return false;
        *slot = cc_boot_guest_os_type() == VIBEOS_PROFILE_SECONDARY ? 1u : 0u;
        state = g_boot_guest_state;
    } else {
        const cc_vm_entry_t *entry = NULL;
        for (uint32_t i = 0; i < CC_VM_CLIENT_SLOTS; i++) {
            if (g_vm_client.entries[i].active && g_vm_client.entries[i].handle == handle) {
                entry = &g_vm_client.entries[i];
                break;
            }
        }
        if (!entry || entry->slot >= AOS_SERIAL_CLIENTS ||
            !(entry->devices & VIBEOS_DEV_SERIAL)) return false;
        cc_guest_status_t status;
        if (cc_vm_status(&g_vm_client, handle, &status) != CC_OK) return false;
        *slot = entry->slot;
        state = status.state;
    }
    return cc_serial_attached[*slot] && state != GUEST_STATE_DEAD &&
           (!input || state == GUEST_STATE_RUNNING);
}

static bool cc_serial_input(uint32_t handle, const cc_input_event_t *event,
                            const uint8_t *text, uint32_t text_len)
{
    uint32_t slot;
    if (!event || !cc_serial_slot(handle, true, &slot)) return false;
    uint8_t byte;
    const uint8_t *bytes = text;
    uint32_t length = text_len;
    if (event->event_type != CC_INPUT_TEXT) {
        if (!aos_console_input_event_to_byte(event->event_type, event->keycode, &byte))
            return true;
        bytes = &byte;
        length = 1;
    } else if (length != event->keycode || length > CC_INPUT_TEXT_MAX) return false;
    if (aos_serial_frontend_write(&cc_serial_channels[slot], bytes, length) !=
        AOS_SERIAL_PUMP_OK) return false;
    if (length) {
        seL4_Signal(PD_CNODE_SLOT_SERIAL_VIRT_NOTIFY);
        if (!(cc_serial_input_reported & (1u << slot))) {
            cc_dbg_puts(slot ? "[cc_pd] input accepted into serial client 1 queue\n" :
                               "[cc_pd] input accepted into serial client 0 queue\n");
            cc_serial_input_reported |= 1u << slot;
        }
    }
    return true;
}

static bool cc_serial_drain(uint32_t handle, uint8_t *dst, uint32_t max,
                            uint32_t *bytes_drained)
{
    uint32_t slot;
    if (!cc_serial_slot(handle, false, &slot)) return false;
    if (aos_serial_frontend_read(&cc_serial_channels[slot], dst, max,
                             bytes_drained) != AOS_SERIAL_PUMP_OK) return false;
    if (*bytes_drained) seL4_Signal(PD_CNODE_SLOT_SERIAL_VIRT_NOTIFY);
    return true;
}

static bool cc_forward_boot_guest_input(const cc_input_event_t *event,
                                        const uint8_t *text, uint32_t text_len)
{
    return cc_serial_input(CC_BOOT_GUEST_HANDLE, event, text, text_len);
}

static bool cc_drain_boot_guest_console(uint8_t *dst, uint32_t max,
                                        uint32_t *bytes_drained)
{
    return cc_serial_drain(CC_BOOT_GUEST_HANDLE, dst, max, bytes_drained);
}

static bool cc_forward_vm_input(uint32_t handle, const cc_input_event_t *event,
                                const uint8_t *text, uint32_t text_len)
{
    return cc_serial_input(handle, event, text, text_len);
}

static bool cc_drain_vm_console(uint32_t handle, uint8_t *dst,
                                uint32_t max, uint32_t *bytes_drained)
{
    return cc_serial_drain(handle, dst, max, bytes_drained);
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

/* Query vm_manager for registered dynamic guests, filling cc_guest_info_t entries
 * starting at out[].  Returns the number of dynamic entries written.  The
 * boot guest is not included here — the caller emits it
 * separately at handle 0 so existing tests keep their layout assumption. */
static uint32_t cc_relay_vm_list(cc_guest_info_t *out, uint32_t max_out)
{
    return cc_vm_list(&g_vm_client, out, max_out);
}

static void handle_list_guests(cc_reply_wire_t *rep)
{
    cc_guest_info_t *out = (cc_guest_info_t *)rep->shmem;
    uint32_t max_entries = (uint32_t)(CC_WIRE_SHMEM_SIZE / sizeof(cc_guest_info_t));
    uint32_t count = 0u;

#if defined(AGENTOS_GUEST_PRIMARY) || defined(AGENTOS_GUEST_SECONDARY)
    if (g_boot_guest_present && count < max_entries) {
        cc_fill_boot_guest_info(&out[count]);
        count++;
    }
#endif

    if (count < max_entries) {
        count += cc_relay_vm_list(&out[count], max_entries - count);
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

#if defined(AGENTOS_GUEST_PRIMARY) || defined(AGENTOS_GUEST_SECONDARY)
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
/* MAC task_f95d118416a24fa484c2c43f0d955b56: the controller (monitor) PD is
 * no longer in the boot image, so cc_pd holds no controller endpoint
 * (system_desc_aarch64.c: cc_pd init_eps).  Calling an endpoint with no
 * server would block cc_pd forever, so the relay is gone and the reply is
 * the correct-by-construction fallback: no controller, no observed agent
 * load, pool fully idle. */
static void handle_list_polecats(cc_reply_wire_t *rep)
{
    rep->mr[0] = CC_OK;
    rep->mr[1] = WORKER_POOL_SIZE;  /* total */
    rep->mr[2] = 0u;                /* busy  */
    rep->mr[3] = WORKER_POOL_SIZE;  /* idle  */
}

static void handle_guest_status(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    uint32_t handle = req->mr[0];
#if defined(AGENTOS_GUEST_PRIMARY) || defined(AGENTOS_GUEST_SECONDARY)
    if (handle == CC_BOOT_GUEST_HANDLE) {
        if (!g_boot_guest_present) {
            rep->mr[0] = CC_ERR_BAD_HANDLE;
            return;
        }
        cc_fill_boot_guest_status((cc_guest_status_t *)rep->shmem);
        rep->mr[0] = CC_OK;
        return;
    }
#endif
    rep->mr[0] = cc_vm_status(&g_vm_client, handle,
                              (cc_guest_status_t *)rep->shmem);
}

static void handle_device_status(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    uint32_t dev_type = req->mr[0];
    uint32_t dev_handle = req->mr[1];
    if (dev_type >= CC_DEV_TYPE_COUNT) {
        rep->mr[0] = CC_ERR_BAD_DEV_TYPE;
        return;
    }

#if defined(AGENTOS_GUEST_PRIMARY) || defined(AGENTOS_GUEST_SECONDARY)
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
#if defined(AGENTOS_GUEST_PRIMARY) || defined(AGENTOS_GUEST_SECONDARY)
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

    rep->mr[0] = cc_forward_vm_input(req->mr[0], event, text, text_len)
                 ? CC_OK : CC_ERR_RELAY_FAULT;
    return;
#else
    (void)req;
#endif
    rep->mr[0] = CC_ERR_BAD_HANDLE;
}

static void handle_snapshot(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    cc_vm_reply_t reply = {0};
    rep->mr[0] = cc_vm_request(&g_vm_client, VM_MANAGER_OP_SNAPSHOT,
                               req->mr[0], NULL, 0u, &reply);
    /* vm_manager currently reports VM_NOT_IMPL; do not publish empty success. */
    if (rep->mr[0] == CC_OK) rep->mr[0] = CC_ERR_RELAY_FAULT;
    rep->mr[1] = 0u;
    rep->mr[2] = 0u;
}

static void handle_restore(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    cc_vm_reply_t reply = {0};
    uint8_t payload[8u];
    cc_wire_wr32(payload, 0u, req->mr[1]);
    cc_wire_wr32(payload, 4u, req->mr[2]);
    rep->mr[0] = cc_vm_request(&g_vm_client, VM_MANAGER_OP_RESTORE,
                               req->mr[0], payload, sizeof(payload), &reply);
    if (rep->mr[0] == CC_OK) rep->mr[0] = CC_ERR_RELAY_FAULT;
}

/*
 * MSG_CC_LOG_STREAM — drain a guest's serial output as ASCII bytes (agentos-vsi).
 *
 * Wire args: MR1 = slot, MR2 = pd_id.  Reply: MR0 = CC_OK, MR1 = byte length,
 * shmem = the drained ASCII bytes.  MR2 echoes the resolved log slot so the
 * caller can re-address the same stream on subsequent polls.
 *
 *   slot 0,  pd_id TRACE_PD_CONTROLLER  → boot guest serial (guest_vmm drain)
 *   slot 0,  pd_id GUEST_VMM_PRIMARY/SECONDARY → vibe guest addressed by MR1==handle,
 *                                          assigned its own slot (1..N) on use
 *   slot N>0                            → previously assigned vibe guest slot
 *
 * The boot guest always occupies slot 0; each vibe_engine guest gets its own
 * slot from g_log_slots[] so it is independently addressable by handle.
 */
static void handle_log_stream(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
#if defined(AGENTOS_GUEST_PRIMARY) || defined(AGENTOS_GUEST_SECONDARY)
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
    if (pd_id == TRACE_PD_GUEST_VMM_PRIMARY ||
        pd_id == TRACE_PD_GUEST_VMM_SECONDARY ||
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
        if (!cc_drain_vm_console(guest_handle, rep->shmem,
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
    uint32_t handle = 0u;
#if defined(__x86_64__)
    const uint8_t arch = VIBEOS_ARCH_X86_64;
#else
    const uint8_t arch = VIBEOS_ARCH_AARCH64;
#endif
    if (req->shmem[1] != arch) {
        rep->mr[0] = CC_ERR_RELAY_FAULT;
        return;
    }
#if defined(AGENTOS_GUEST_PRIMARY) || defined(AGENTOS_GUEST_SECONDARY)
    if (g_boot_guest_present && req->shmem[0] == cc_boot_guest_os_type()) {
        rep->mr[0] = CC_ERR_RELAY_FAULT;
        return;
    }
#endif
    rep->mr[0] = cc_vm_create(&g_vm_client, req->shmem[0],
                               cc_wire_rd32(req->shmem, 4u),
                               cc_wire_rd32(req->shmem, 16u), &handle);
    rep->mr[1] = rep->mr[0] == CC_OK ? handle : 0u;
    /* Failed rollback remains discoverable and explicitly addressable. */
    rep->mr[2] = rep->mr[0] == CC_OK ? 0u : handle;
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

#if defined(AGENTOS_GUEST_PRIMARY) || defined(AGENTOS_GUEST_SECONDARY)
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
    rep->mr[0] = cc_vm_lifecycle(&g_vm_client, VM_MANAGER_OP_STOP, handle, &state);
    rep->mr[1] = rep->mr[0] == CC_OK ? state : 0u;
}

static void handle_resume_guest(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    uint32_t handle = req->mr[0];

#if defined(AGENTOS_GUEST_PRIMARY) || defined(AGENTOS_GUEST_SECONDARY)
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
    rep->mr[0] = cc_vm_lifecycle(&g_vm_client, VM_MANAGER_OP_RESUME, handle, &state);
    rep->mr[1] = rep->mr[0] == CC_OK ? state : 0u;
}

static void handle_destroy_guest(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    uint32_t handle = req->mr[0];

#if defined(AGENTOS_GUEST_PRIMARY) || defined(AGENTOS_GUEST_SECONDARY)
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

    rep->mr[0] = cc_vm_lifecycle(&g_vm_client, VM_MANAGER_OP_DESTROY,
                                  handle, NULL);
    if (rep->mr[0] == CC_OK) {
        for (uint32_t i = 1u; i < CC_LOG_SLOTS; i++) {
            if (g_log_slots[i].in_use && g_log_slots[i].guest_handle == handle)
                g_log_slots[i] = (cc_log_slot_t){0};
        }
    }

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

#ifdef AGENTOS_NATIVE_RUST_TEST
#include "contracts/native_rust_probe.h"
static void handle_native_network(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    rep->mr[0] = CC_ERR_RELAY_FAULT;
    if (req->mr[0] != NATIVE_RUST_VERSION) return;
    seL4_SetMR(0, NATIVE_RUST_VERSION);
    seL4_MessageInfo_t response = seL4_Call(NATIVE_RUST_CC_ENDPOINT,
        seL4_MessageInfo_new(NATIVE_RUST_NETWORK, 0, 0, 1));
    if (seL4_MessageInfo_get_label(response) != NATIVE_RUST_OK ||
        seL4_MessageInfo_get_length(response) != 4 ||
        seL4_GetMR(0) != 1 || seL4_GetMR(1) != 3 || seL4_GetMR(2) < 3) return;
    rep->mr[0] = CC_OK;
    rep->mr[1] = seL4_GetMR(0);
    rep->mr[2] = seL4_GetMR(1);
    rep->mr[3] = seL4_GetMR(3);
}
#endif

static void handle_inspect(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    rep->mr[0] = CC_ERR_INVALID_ARG;
    if (req->mr[0] != AOS_INSPECT_VERSION || req->mr[1] || req->mr[2]) return;
    const aos_inspect_snapshot_t *snap = (const void *)AOS_INSPECT_BOOT_VA;
    if (aos_inspect_validate(snap) != AOS_INSPECT_OK ||
        !(snap->flags & AOS_INSPECT_FLAG_BOOT)) return;
    for (size_t i = 0; i < sizeof(*snap); i++)
        rep->shmem[i] = ((const uint8_t *)snap)[i];
    rep->mr[0] = CC_OK;
    rep->mr[1] = sizeof(*snap);
    rep->mr[2] = snap->flags;
    rep->mr[3] = snap->version;
}

static void handle_operator(const cc_req_wire_t *req, cc_reply_wire_t *rep, bool write)
{
    rep->mr[0] = CC_ERR_INVALID_ARG;
    if (req->mr[0] != AOS_OPERATOR_VERSION || req->mr[1] > sizeof(req->shmem) || req->mr[2]) return;
    uint32_t slot = SERIAL_VIRT_OPERATOR_CLIENT, count = 0;
    if (!cc_serial_attached[slot]) { rep->mr[0] = CC_ERR_RELAY_FAULT; return; }
    aos_serial_pump_status_t status;
    if (write) {
        status = aos_serial_frontend_write(&cc_serial_channels[slot], req->shmem, req->mr[1]);
        if (status == AOS_SERIAL_PUMP_OK) count = req->mr[1];
    } else status = aos_serial_frontend_read(&cc_serial_channels[slot],
                                          rep->shmem, req->mr[1], &count);
    rep->mr[0] = status == AOS_SERIAL_PUMP_OK ? CC_OK :
                 status == AOS_SERIAL_PUMP_FULL ? CC_ERR_WOULD_BLOCK : CC_ERR_RELAY_FAULT;
    rep->mr[1] = count;
    if (count) seL4_Signal(PD_CNODE_SLOT_SERIAL_VIRT_NOTIFY);
}

static void handle_input_submit(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    aos_input_request_t query;
    __builtin_memcpy(&query,req->shmem,sizeof(query));
    bool release=query.version==AOS_INPUT_RELEASE_VERSION && query.count==0;
    if (req->mr[1] || req->mr[2] || (!release && query.version!=AOS_INPUT_VERSION) ||
        query.id || query.client || query.reserved[0] || query.reserved[1] || query.reserved[2] ||
        query.device>=AOS_INPUT_DEVICES || (!release && !query.count) || query.count>AOS_INPUT_BATCH_EVENTS) {
        rep->mr[0]=CC_ERR_INVALID_ARG;
        return;
    }
#ifdef AGENTOS_GUEST_INPUT
    uint32_t handle=req->mr[0];
    if (handle==CC_BOOT_GUEST_HANDLE) {
        if (!g_boot_guest_present || g_boot_guest_state==GUEST_STATE_DEAD) {
            rep->mr[0]=CC_ERR_BAD_HANDLE;
            return;
        }
        query.client=cc_boot_guest_os_type()==VIBEOS_PROFILE_SECONDARY ? 1u : 0u;
    } else {
        const cc_vm_entry_t *entry=NULL;
        for (uint32_t i=0;i<CC_VM_CLIENT_SLOTS;++i)
            if (g_vm_client.entries[i].active && g_vm_client.entries[i].handle==handle)
                entry=&g_vm_client.entries[i];
        cc_guest_status_t status;
        if (!entry || entry->slot>=AOS_INPUT_CLIENTS ||
            cc_vm_status(&g_vm_client,handle,&status)!=CC_OK || status.state==GUEST_STATE_DEAD) {
            rep->mr[0]=CC_ERR_BAD_HANDLE;
            return;
        }
        query.client=entry->slot;
    }
    static uint32_t next_id;
    query.id=++next_id;
    aos_input_frontend_t *frontend=(void *)AOS_INPUT_FRONTEND_VA;
    if (aos_input_submit(frontend,&query)!=0) { rep->mr[0]=CC_ERR_RELAY_FAULT; return; }
    seL4_Signal(PD_CNODE_SLOT_INPUT_PEER_NOTIFY);
    aos_input_response_t response;
    while (aos_input_receive(frontend,&response)!=0) {
        seL4_Word badge; seL4_Wait(PD_CNODE_SLOT_INPUT_WAIT,&badge);
    }
    if (response.version!=query.version || response.id!=query.id ||
        response.status>AOS_INPUT_WOULD_BLOCK ||
        response.accepted!=(response.status==AOS_INPUT_OK ? query.count : 0u)) {
        rep->mr[0]=CC_ERR_RELAY_FAULT;
    } else {
        response.id=0;
        __builtin_memcpy(rep->shmem,&response,sizeof(response));
        rep->mr[0]=CC_OK; rep->mr[1]=sizeof(response);
        rep->mr[2]=response.status; rep->mr[3]=response.version;
    }
    seL4_Signal(PD_CNODE_SLOT_INPUT_PEER_NOTIFY);
#else
    rep->mr[0]=CC_ERR_RELAY_FAULT;
#endif
}

static void handle_frame_capture(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    aos_fb_observer_request_t query;
    __builtin_memcpy(&query, req->shmem, sizeof(query));
    if (req->mr[1] || req->mr[2] || query.version != AOS_FB_OBSERVER_VERSION ||
        query.id || query.client || query.operation < AOS_FB_CAPTURE ||
        query.operation > AOS_FB_CAPTURE_RELEASE ||
        (query.operation == AOS_FB_CAPTURE && (query.cookie || query.offset || query.length)) ||
        (query.operation != AOS_FB_CAPTURE && (req->mr[0] || !query.cookie)) ||
        (query.operation == AOS_FB_CAPTURE_READ && (!query.length ||
            query.length > CC_WIRE_SHMEM_SIZE - sizeof(aos_fb_observer_response_t))) ||
        (query.operation == AOS_FB_CAPTURE_RELEASE && (query.offset || query.length))) {
        rep->mr[0] = CC_ERR_INVALID_ARG;
        return;
    }
#if defined(AGENTOS_GUEST_GRAPHICS) || defined(AGENTOS_FRAMEBUFFER_TEST)
    if (query.operation == AOS_FB_CAPTURE) {
        uint32_t handle = req->mr[0];
#ifdef AGENTOS_FRAMEBUFFER_TEST
        /* Native producers in the focused test image only. Never interpreted
         * as guest handles, and compiled out of every production variant. */
        if (handle < 0xfb000000u || handle >= 0xfb000000u + AOS_FB_CLIENTS) {
            rep->mr[0] = CC_ERR_BAD_HANDLE;
            return;
        }
        query.client = handle - 0xfb000000u;
#else
        if (handle == CC_BOOT_GUEST_HANDLE) {
            if (!g_boot_guest_present || g_boot_guest_state == GUEST_STATE_DEAD) {
                rep->mr[0] = CC_ERR_BAD_HANDLE;
                return;
            }
            query.client = cc_boot_guest_os_type() == VIBEOS_PROFILE_SECONDARY ? 1u : 0u;
        } else {
            const cc_vm_entry_t *entry = NULL;
            for (uint32_t i = 0; i < CC_VM_CLIENT_SLOTS; ++i)
                if (g_vm_client.entries[i].active && g_vm_client.entries[i].handle == handle)
                    entry = &g_vm_client.entries[i];
            cc_guest_status_t status;
            if (!entry || entry->slot >= AOS_FB_CLIENTS ||
                cc_vm_status(&g_vm_client, handle, &status) != CC_OK ||
                status.state == GUEST_STATE_DEAD) {
                rep->mr[0] = CC_ERR_BAD_HANDLE;
                return;
            }
            query.client = entry->slot;
        }
#endif
    }
    static uint32_t next_id;
    query.id = ++next_id;
    aos_fb_observer_region_t *region = (void *)AOS_FB_OBSERVER_VA;
    if (aos_fb_observer_submit(region, &query) != 0) {
        rep->mr[0] = CC_ERR_RELAY_FAULT;
        return;
    }
    seL4_Signal(PD_CNODE_SLOT_FB_PEER_NOTIFY);
    aos_fb_observer_response_t response;
    while (aos_fb_observer_receive(region, &response) != 0) {
        seL4_Word badge;
        seL4_Wait(PD_CNODE_SLOT_FB_WAIT, &badge);
    }
    bool valid = response.version == AOS_FB_OBSERVER_VERSION && response.id == query.id &&
        response.status <= AOS_FB_OBSERVER_EXHAUSTED &&
        response.length <= CC_WIRE_SHMEM_SIZE - sizeof(response) &&
        (response.length == 0 || (response.status == AOS_FB_OBSERVER_OK &&
            query.operation == AOS_FB_CAPTURE_READ && response.length == query.length));
    if (valid) {
        response.id = 0;
        __builtin_memcpy(rep->shmem, &response, sizeof(response));
        __builtin_memcpy(rep->shmem + sizeof(response), region->data, response.length);
        rep->mr[0] = CC_OK;
        rep->mr[1] = sizeof(response) + response.length;
        rep->mr[2] = response.status;
        rep->mr[3] = response.version;
    } else rep->mr[0] = CC_ERR_RELAY_FAULT;
    seL4_Signal(PD_CNODE_SLOT_FB_PEER_NOTIFY);
#else
    rep->mr[0] = CC_ERR_RELAY_FAULT;
#endif
}

static void cc_dispatch(const cc_req_wire_t *req, cc_reply_wire_t *rep)
{
    /* Age active sessions before dispatch.  Handlers that touch a specific
     * session (CONNECT/SEND/RECV/STATUS) reset that slot's tick counter back
     * to 0 inside themselves, so the net effect is: every session except the
     * one being touched ages by one tick per dispatch.  alloc_session uses
     * this to identify abandoned sessions when the table is full. */
    cc_age_sessions();

    switch (req->opcode) {
    case MSG_CC_FRAME_CAPTURE: handle_frame_capture(req, rep); break;
    case MSG_CC_INPUT_SUBMIT: handle_input_submit(req, rep); break;
#ifdef AGENTOS_NATIVE_RUST_TEST
    case NATIVE_RUST_CC_NETWORK: handle_native_network(req, rep); break;
#endif
    /* Session management */
    case MSG_CC_CONNECT:    handle_connect(req, rep);          break;
    case MSG_CC_DISCONNECT: handle_disconnect(req, rep);       break;
    case MSG_CC_SEND:       handle_send(req, rep);             break;
    case MSG_CC_RECV:       handle_recv(req, rep);             break;
    case MSG_CC_STATUS:     handle_status(req, rep);           break;
    case MSG_CC_LIST:       handle_list_sessions(rep);         break;

    /* Relay API */
    case MSG_CC_INSPECT:            handle_inspect(req, rep);           break;
    case MSG_CC_OPERATOR_WRITE:     handle_operator(req, rep, true);    break;
    case MSG_CC_OPERATOR_READ:      handle_operator(req, rep, false);   break;
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
    cc_vm_client_init(&g_vm_client, cc_vm_rpc, NULL);
#if defined(__aarch64__)
    cc_serial_init();
#endif

    /*
     * Canonical boot-complete marker — must match xtask/src/cmd_test.rs and
     * xtask/src/cmd_ci_matrix.rs.  cc_pd is the lowest-priority PD in the
     * image (system_desc_aarch64.c), so reaching this point means every
     * other PD has run to its blocking point and the control console is
     * ready to accept requests.  The controller PD used to print this.
     */
    cc_dbg_puts("agentOS boot complete\n");
#ifdef AGENTOS_INSPECT_WRITE_PROBE
    if (aos_inspect_validate((const void *)AOS_INSPECT_BOOT_VA) == AOS_INSPECT_OK) {
        cc_dbg_puts("[cc_pd] inspect: valid boot page read before write probe\n");
        *(volatile uint32_t *)AOS_INSPECT_BOOT_VA = 0;
        cc_dbg_puts("[cc_pd] FAIL: inspect page was writable\n");
    }
#endif

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
             * request/reply pair before resetting the poisoned TX queue.
             * First retry the reply on the fresh queue so the still-connected
             * host does not need to wait for its frame deadline.  Retain the
             * cache either way: if the socket crossed its deadline at the
             * same instant, a reconnecting host can repeat the request without
             * executing it twice.
             */
            cc_retry_cache_record(&g_retry, &g_req, &g_rep);
            virtio_serial_recover_tx();
            if (!vio_serial_write(&g_rep, sizeof(g_rep))) {
                cc_dbg_puts("[cc_pd] recovered reply TX remained blocked\n");
                virtio_serial_recover_tx();
            }
        }
    }
}

void pd_main(seL4_CPtr my_ep, seL4_CPtr ns_ep) { cc_pd_main(my_ep, ns_ep); }
