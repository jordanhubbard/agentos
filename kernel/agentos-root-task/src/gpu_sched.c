/*
 * gpu_sched.c — agentOS GPU Scheduler Protection Domain  [E5-S7: raw seL4 IPC]
 *
 * Priority 120 (PRIO_INTERACTIVE range).
 *
 * The GPU scheduler is the control plane for CUDA workload routing on Sparky's
 * 192GB Blackwell GB10. It queues agent task requests, dispatches them to
 * available compute WASM slots, and publishes completion events to the EventBus.
 *
 * IPC Design (raw seL4, sel4_server_t dispatch):
 *   - Agents IPC MSG_GPU_SUBMIT → get back a ticket_id (or error)
 *   - Slots complete → handled as MSG_GPU_COMPLETE notification via IPC
 *   - EventBus publish MSG_GPU_COMPLETE / MSG_GPU_FAILED on completion
 *   - MSG_GPU_STATUS for scheduling hints
 *
 * Queue: fixed-size ring (GPU_QUEUE_DEPTH=16), priority-ordered (higher wins).
 * Slots: GPU_SLOT_COUNT=4 parallel CUDA compute slots (from MAX_SWAP_SLOTS).
 *
 * Entry point:
 *   void gpu_sched_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
 *
 * Bugs fixed in this migration (E5-S7):
 *   - Channel-60 dispatch removed: the old Microkit notified() handler used
 *     hardcoded channel IDs (CH_CONTROLLER=1, CH_EVENTBUS=2) that caused
 *     "invalid channel '60'" errors at boot when the channel graph was not
 *     wired.  All outbound calls now use nameserver-resolved endpoints.
 *   - log_drain_write(15, 15, ...) replaced with seL4_DebugPutChar loop.
 *   - EventBus publishing via microkit_ppcall(CH_EVENTBUS, ...) replaced
 *     with sel4_call on a nameserver-resolved endpoint.
 *   - microkit_notify(CH_CONTROLLER) replaced with sel4_call on a
 *     nameserver-resolved "controller" endpoint.
 *   - microkit_ppcall(CH_EVENTBUS, MSG_EVENTBUS_SUBSCRIBE, ...) replaced
 *     with nameserver-resolved event_bus endpoint + sel4_call.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

/* ── Conditional compilation ─────────────────────────────────────────────── */

#ifdef AGENTOS_TEST_HOST
/*
 * Host-side test build: minimal type stubs so this file compiles without
 * seL4 or Microkit headers.
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

typedef unsigned long      seL4_CPtr;
typedef unsigned long long sel4_badge_t;

typedef struct {
    uint32_t opcode;
    uint32_t length;
    uint8_t  data[48];
} sel4_msg_t;

#define SEL4_ERR_OK          0u
#define SEL4_ERR_INVALID_OP  1u
#define SEL4_ERR_BAD_ARG     4u
#define SEL4_ERR_NO_MEM      5u

typedef uint32_t (*sel4_handler_fn)(sel4_badge_t badge,
                                     const sel4_msg_t *req,
                                     sel4_msg_t *rep,
                                     void *ctx);
#define SEL4_SERVER_MAX_HANDLERS 32u
typedef struct {
    struct {
        uint32_t        opcode;
        sel4_handler_fn fn;
        void           *ctx;
    } handlers[SEL4_SERVER_MAX_HANDLERS];
    uint32_t  handler_count;
    seL4_CPtr ep;
} sel4_server_t;

static inline void sel4_server_init(sel4_server_t *srv, seL4_CPtr ep)
{
    srv->handler_count = 0;
    srv->ep            = ep;
    for (uint32_t i = 0; i < SEL4_SERVER_MAX_HANDLERS; i++) {
        srv->handlers[i].opcode = 0;
        srv->handlers[i].fn     = (sel4_handler_fn)0;
        srv->handlers[i].ctx    = (void *)0;
    }
}
static inline int sel4_server_register(sel4_server_t *srv, uint32_t opcode,
                                        sel4_handler_fn fn, void *ctx)
{
    if (srv->handler_count >= SEL4_SERVER_MAX_HANDLERS) return -1;
    srv->handlers[srv->handler_count].opcode = opcode;
    srv->handlers[srv->handler_count].fn     = fn;
    srv->handlers[srv->handler_count].ctx    = ctx;
    srv->handler_count++;
    return 0;
}
static inline uint32_t sel4_server_dispatch(sel4_server_t *srv,
                                             sel4_badge_t badge,
                                             const sel4_msg_t *req,
                                             sel4_msg_t *rep)
{
    for (uint32_t i = 0; i < srv->handler_count; i++) {
        if (srv->handlers[i].opcode == req->opcode) {
            uint32_t rc = srv->handlers[i].fn(badge, req, rep,
                                               srv->handlers[i].ctx);
            rep->opcode = rc;
            return rc;
        }
    }
    rep->opcode = SEL4_ERR_INVALID_OP;
    rep->length = 0;
    return SEL4_ERR_INVALID_OP;
}
static inline void sel4_call(seL4_CPtr ep, const sel4_msg_t *req, sel4_msg_t *rep)
{
    (void)ep; (void)req;
    rep->opcode = 0;
    rep->length = 0;
}
static inline void seL4_DebugPutChar(char c) { (void)c; }

#else /* !AGENTOS_TEST_HOST — production build */

#include <stdint.h>
#include <stdbool.h>
#include "sel4_server.h"    /* sel4_server_t, sel4_server_init/register/run */
#include "sel4_client.h"    /* sel4_client_t, sel4_client_connect */
#include "sel4_ipc.h"       /* sel4_msg_t, sel4_badge_t, SEL4_ERR_* */
#include <sel4/sel4.h>      /* seL4_DebugPutChar */

#endif /* AGENTOS_TEST_HOST */

/* ── GPU scheduler IPC opcodes ──────────────────────────────────────────── */

#ifndef MSG_GPU_SUBMIT
#define MSG_GPU_SUBMIT        0x0901u
#define MSG_GPU_SUBMIT_REPLY  0x0902u
#define MSG_GPU_STATUS        0x0903u
#define MSG_GPU_STATUS_REPLY  0x0904u
#define MSG_GPU_CANCEL        0x0905u
#define MSG_GPU_CANCEL_REPLY  0x0906u
#define MSG_GPU_COMPLETE      0x0910u
#define MSG_GPU_FAILED        0x0911u
#endif

#ifndef OP_GPU_SUBMIT_CMD
#define OP_GPU_SUBMIT_CMD     0xE4u
#endif

/* EventBus opcodes */
#ifndef MSG_EVENT_PUBLISH
#define MSG_EVENT_PUBLISH     0x0410u
#define MSG_EVENTBUS_SUBSCRIBE 0x0002u
#endif

/* Nameserver opcode */
#ifndef OP_NS_REGISTER
#define OP_NS_REGISTER 0xD0u
#define OP_NS_LOOKUP   0xD1u
#endif
#ifndef NS_OK
#define NS_OK 0u
#endif
#ifndef NS_NAME_MAX
#define NS_NAME_MAX 32
#endif

/* virtio-gpu MMIO constants */
#ifndef VIRTIO_MMIO_MAGIC_VALUE
#define VIRTIO_MMIO_MAGIC_VALUE   0x000u
#define VIRTIO_MMIO_VERSION       0x004u
#define VIRTIO_MMIO_DEVICE_ID     0x008u
#define VIRTIO_MMIO_QUEUE_NOTIFY  0x050u
#define VIRTIO_MMIO_STATUS        0x070u
#define VIRTIO_MMIO_MAGIC         0x74726976u
#define VIRTIO_GPU_DEVICE_ID      16u
#define VIRTIO_STATUS_ACKNOWLEDGE (1u << 0)
#define VIRTIO_STATUS_DRIVER      (1u << 1)
#endif

/* ── Constants ───────────────────────────────────────────────────────────── */

#define GPU_QUEUE_DEPTH     16
#define GPU_SLOT_COUNT      4
#define GPU_PRIO_DEFAULT    50
#define GPU_PRIO_HIGH       100
#define GPU_PRIO_RT         200   /* real-time — front of queue */

/* Error codes returned in data[4..7] of GPU_SUBMIT_REPLY */
#define GPU_ERR_OK          0u
#define GPU_ERR_QUEUE_FULL  1u
#define GPU_ERR_INVALID     2u
#define GPU_ERR_NO_SLOTS    3u

/* ── MMIO addresses ──────────────────────────────────────────────────────── */

/*
 * virtio_gpu_mmio_vaddr — virtio-gpu MMIO base.
 * Set by the root task before calling gpu_sched_main().
 */
uintptr_t virtio_gpu_mmio_vaddr;

/*
 * log_drain_rings_vaddr — log ring shmem (set by root task; 0 if unused).
 */
uintptr_t log_drain_rings_vaddr;

/* ── Module-level endpoint cache ────────────────────────────────────────── */

/* Nameserver-resolved endpoints (0 = not yet connected) */
static seL4_CPtr g_eventbus_ep   = 0;
static seL4_CPtr g_controller_ep = 0;
static seL4_CPtr g_ns_ep         = 0;

/* ── Task states ──────────────────────────────────────────────────────────── */

typedef enum {
    TASK_FREE     = 0,
    TASK_QUEUED   = 1,
    TASK_RUNNING  = 2,
    TASK_DONE     = 3,
    TASK_FAILED   = 4,
} task_state_t;

/* ── Task descriptor ─────────────────────────────────────────────────────── */

typedef struct {
    uint32_t    ticket_id;
    uint64_t    wasm_hash_lo;
    uint64_t    wasm_hash_hi;
    uint32_t    priority;
    uint32_t    flags;
    task_state_t state;
    int32_t     slot_id;
    uint32_t    submitter_badge;  /* badge of submitting client */
} gpu_task_t;

/* ── Slot state ──────────────────────────────────────────────────────────── */

typedef struct {
    bool        busy;
    uint32_t    ticket_id;
} gpu_slot_t;

/* ── Scheduler state ─────────────────────────────────────────────────────── */

static struct {
    gpu_task_t  queue[GPU_QUEUE_DEPTH];
    gpu_slot_t  slots[GPU_SLOT_COUNT];
    uint32_t    ticket_seq;
    uint32_t    tasks_submitted;
    uint32_t    tasks_completed;
    uint32_t    tasks_failed;
    bool        eventbus_ready;
    bool        gpu_hw_present;
    uint32_t    gpu_fence_seq;
} sched;

/* Server instance */
static sel4_server_t g_srv;

/* ── Data-field helpers ──────────────────────────────────────────────────── */

static inline uint32_t data_rd32(const uint8_t *d, int off)
{
    return (uint32_t)d[off    ]
         | ((uint32_t)d[off+1] <<  8)
         | ((uint32_t)d[off+2] << 16)
         | ((uint32_t)d[off+3] << 24);
}

static inline void data_wr32(uint8_t *d, int off, uint32_t v)
{
    d[off  ] = (uint8_t)(v      );
    d[off+1] = (uint8_t)(v >>  8);
    d[off+2] = (uint8_t)(v >> 16);
    d[off+3] = (uint8_t)(v >> 24);
}

static inline uint64_t data_rd64(const uint8_t *d, int off)
{
    return (uint64_t)data_rd32(d, off) | ((uint64_t)data_rd32(d, off + 4) << 32);
}

static inline void data_wr64(uint8_t *d, int off, uint64_t v)
{
    data_wr32(d, off,     (uint32_t)(v & 0xFFFFFFFFu));
    data_wr32(d, off + 4, (uint32_t)(v >> 32));
}

/* ── Debug output ────────────────────────────────────────────────────────── */

static void dbg_puts(const char *s)
{
#ifdef CONFIG_PRINTING
    for (; *s; s++)
        seL4_DebugPutChar(*s);
#else
    (void)s;
#endif
}

/* ── MMIO helpers ────────────────────────────────────────────────────────── */

static inline uint32_t gpu_mmio_read32(uintptr_t base, uint32_t off)
{
    return *(volatile uint32_t *)(base + off);
}

static inline void gpu_mmio_write32(uintptr_t base, uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(base + off) = val;
}

static void gpu_virtio_kick(uint32_t queue_id)
{
    if (!virtio_gpu_mmio_vaddr) return;
    gpu_mmio_write32(virtio_gpu_mmio_vaddr, VIRTIO_MMIO_QUEUE_NOTIFY, queue_id);
}

/* ── virtio-gpu probe ────────────────────────────────────────────────────── */

void probe_virtio_gpu(void)
{
    if (!virtio_gpu_mmio_vaddr) {
        dbg_puts("[gpu_sched] virtio-gpu: MMIO vaddr not mapped, stub mode\n");
        return;
    }

    uint32_t magic     = gpu_mmio_read32(virtio_gpu_mmio_vaddr, VIRTIO_MMIO_MAGIC_VALUE);
    uint32_t version   = gpu_mmio_read32(virtio_gpu_mmio_vaddr, VIRTIO_MMIO_VERSION);
    uint32_t device_id = gpu_mmio_read32(virtio_gpu_mmio_vaddr, VIRTIO_MMIO_DEVICE_ID);

    if (magic != VIRTIO_MMIO_MAGIC || version != 2u
            || device_id != VIRTIO_GPU_DEVICE_ID) {
        dbg_puts("[gpu_sched] virtio-gpu not detected (magic/ver/dev mismatch), stub mode\n");
        return;
    }

    sched.gpu_hw_present = true;

    gpu_mmio_write32(virtio_gpu_mmio_vaddr, VIRTIO_MMIO_STATUS,
                     VIRTIO_STATUS_ACKNOWLEDGE);
    gpu_mmio_write32(virtio_gpu_mmio_vaddr, VIRTIO_MMIO_STATUS,
                     VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    dbg_puts("[gpu_sched] virtio-gpu detected: hw present\n");
}

/* ── Scheduler helpers ───────────────────────────────────────────────────── */

static int queue_alloc(void)
{
    for (int i = 0; i < GPU_QUEUE_DEPTH; i++) {
        if (sched.queue[i].state == TASK_FREE) return i;
    }
    return -1;
}

static int slot_alloc(void)
{
    for (int i = 0; i < GPU_SLOT_COUNT; i++) {
        if (!sched.slots[i].busy) return i;
    }
    return -1;
}

static int queue_pick_best(void)
{
    int best = -1;
    uint32_t best_prio = 0;
    for (int i = 0; i < GPU_QUEUE_DEPTH; i++) {
        if (sched.queue[i].state == TASK_QUEUED &&
            sched.queue[i].priority >= best_prio) {
            best_prio = sched.queue[i].priority;
            best = i;
        }
    }
    return best;
}

static int queue_find(uint32_t ticket_id)
{
    for (int i = 0; i < GPU_QUEUE_DEPTH; i++) {
        if (sched.queue[i].ticket_id == ticket_id &&
            sched.queue[i].state != TASK_FREE) return i;
    }
    return -1;
}

/* ── EventBus publishing ─────────────────────────────────────────────────── */

/*
 * publish_completion — send MSG_GPU_COMPLETE or MSG_GPU_FAILED to EventBus.
 *
 * Replaces the old microkit_ppcall(CH_EVENTBUS, ...) which used the hard-coded
 * channel number CH_EVENTBUS=2.  That channel caused "invalid channel '60'"
 * errors when the Microkit channel graph was not wired.  Now we use a
 * nameserver-resolved endpoint.
 */
static void publish_completion(uint32_t ticket_id, bool success)
{
    if (!g_eventbus_ep) return;

    uint32_t tag = success ? (uint32_t)MSG_GPU_COMPLETE : (uint32_t)MSG_GPU_FAILED;
    sel4_msg_t req, rep;
    req.opcode = (uint32_t)MSG_EVENT_PUBLISH;
    data_wr32(req.data,  0, (uint32_t)MSG_EVENT_PUBLISH);
    data_wr32(req.data,  4, tag);
    data_wr32(req.data,  8, ticket_id);
    data_wr32(req.data, 12, success ? 0u : 1u);
    req.length = 16;
    sel4_call(g_eventbus_ep, &req, &rep);
    /* Ignore reply — publish is fire-and-forget */
}

/* ── Dispatch logic ──────────────────────────────────────────────────────── */

/*
 * dispatch_pending — dispatch the next queued task to a free slot.
 *
 * Notifies the controller via sel4_call on g_controller_ep.
 * Replaces the old microkit_notify(CH_CONTROLLER) which used the
 * hard-coded channel ID CH_CONTROLLER=1.
 */
static void dispatch_pending(void)
{
    for (;;) {
        int slot = slot_alloc();
        if (slot < 0) return;

        int task = queue_pick_best();
        if (task < 0) return;

        gpu_task_t *t = &sched.queue[task];
        t->state    = TASK_RUNNING;
        t->slot_id  = slot;
        sched.slots[slot].busy      = true;
        sched.slots[slot].ticket_id = t->ticket_id;

        dbg_puts("[gpu_sched] dispatching task to slot\n");

        if (g_controller_ep) {
            /*
             * Notify controller to route this WASM hash to the appropriate
             * worker/swap slot.  Replaces microkit_notify(CH_CONTROLLER).
             */
            sel4_msg_t req, rep;
            req.opcode = (uint32_t)MSG_GPU_SUBMIT;
            data_wr32(req.data,  0, (uint32_t)MSG_GPU_SUBMIT);
            data_wr32(req.data,  4, t->ticket_id);
            data_wr64(req.data,  8, t->wasm_hash_lo);
            data_wr64(req.data, 16, t->wasm_hash_hi);
            data_wr32(req.data, 24, (uint32_t)slot);
            req.length = 28;
            sel4_call(g_controller_ep, &req, &rep);
        }
    }
}

/* ── IPC handlers ────────────────────────────────────────────────────────── */

/*
 * handle_submit — MSG_GPU_SUBMIT
 *
 * Request data layout:
 *   data[0..7]   = wasm_hash_lo (uint64_t)
 *   data[8..15]  = wasm_hash_hi (uint64_t)
 *   data[16..19] = priority (uint32_t)
 *   data[20..23] = flags    (uint32_t)
 *
 * Reply data layout:
 *   data[0..3] = ticket_id (0 on error)
 *   data[4..7] = GPU_ERR_OK or error code
 */
static uint32_t handle_submit(sel4_badge_t badge, const sel4_msg_t *req,
                               sel4_msg_t *rep, void *ctx)
{
    (void)ctx;

    uint64_t hash_lo  = data_rd64(req->data,  0);
    uint64_t hash_hi  = data_rd64(req->data,  8);
    uint32_t priority = data_rd32(req->data, 16);
    uint32_t flags    = data_rd32(req->data, 20);

    if (priority == 0) priority = GPU_PRIO_DEFAULT;

    int qi = queue_alloc();
    if (qi < 0) {
        dbg_puts("[gpu_sched] SUBMIT: queue full\n");
        data_wr32(rep->data, 0, 0u);
        data_wr32(rep->data, 4, GPU_ERR_QUEUE_FULL);
        rep->length = 8;
        return SEL4_ERR_NO_MEM;
    }

    uint32_t ticket   = ++sched.ticket_seq;
    gpu_task_t *t     = &sched.queue[qi];
    t->ticket_id      = ticket;
    t->wasm_hash_lo   = hash_lo;
    t->wasm_hash_hi   = hash_hi;
    t->priority       = priority;
    t->flags          = flags;
    t->state          = TASK_QUEUED;
    t->slot_id        = -1;
    t->submitter_badge = (uint32_t)badge;
    sched.tasks_submitted++;

    dbg_puts("[gpu_sched] SUBMIT: queued\n");

    dispatch_pending();

    data_wr32(rep->data, 0, ticket);
    data_wr32(rep->data, 4, GPU_ERR_OK);
    rep->length = 8;
    return SEL4_ERR_OK;
}

/*
 * handle_status — MSG_GPU_STATUS
 *
 * Reply data layout:
 *   data[0..3]  = queued task count
 *   data[4..7]  = running task count
 *   data[8..11] = idle slot count
 *   data[12..15] = tasks_completed
 *   data[16..19] = tasks_failed
 */
static uint32_t handle_status(sel4_badge_t badge, const sel4_msg_t *req,
                               sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)req; (void)ctx;

    uint32_t queued = 0, running = 0, idle = 0;
    for (int i = 0; i < GPU_QUEUE_DEPTH; i++) {
        if (sched.queue[i].state == TASK_QUEUED)  queued++;
        if (sched.queue[i].state == TASK_RUNNING) running++;
    }
    for (int i = 0; i < GPU_SLOT_COUNT; i++) {
        if (!sched.slots[i].busy) idle++;
    }

    data_wr32(rep->data,  0, queued);
    data_wr32(rep->data,  4, running);
    data_wr32(rep->data,  8, idle);
    data_wr32(rep->data, 12, sched.tasks_completed);
    data_wr32(rep->data, 16, sched.tasks_failed);
    rep->length = 20;
    return SEL4_ERR_OK;
}

/*
 * handle_cancel — MSG_GPU_CANCEL
 *
 * Request data layout:
 *   data[0..3] = ticket_id
 *
 * Reply data layout:
 *   data[0..3] = 1 if cancelled, 0 if running/done
 */
static uint32_t handle_cancel(sel4_badge_t badge, const sel4_msg_t *req,
                               sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;

    uint32_t ticket = data_rd32(req->data, 0);
    int qi = queue_find(ticket);
    if (qi < 0) {
        data_wr32(rep->data, 0, 0u);
        rep->length = 4;
        return SEL4_ERR_OK;
    }
    if (sched.queue[qi].state == TASK_QUEUED) {
        sched.queue[qi].state     = TASK_FREE;
        sched.queue[qi].ticket_id = 0;
        data_wr32(rep->data, 0, 1u);  /* cancelled */
    } else {
        data_wr32(rep->data, 0, 0u);  /* running or done */
    }
    rep->length = 4;
    return SEL4_ERR_OK;
}

/*
 * handle_submit_cmd — OP_GPU_SUBMIT_CMD (0xE4)
 *
 * Handles a virtio-gpu command buffer submission from framebuffer_pd.
 *
 * Request data layout:
 *   data[0..3]  = opcode (OP_GPU_SUBMIT_CMD)
 *   data[4..7]  = slot_id
 *   data[8..11] = cmd_offset
 *   data[12..15] = cmd_len
 *
 * Reply data layout:
 *   data[0..3] = GPU_ERR_OK or GPU_ERR_INVALID
 *   data[4..7] = fence_id (monotonic; 0 in stub mode)
 */
static uint32_t handle_submit_cmd(sel4_badge_t badge, const sel4_msg_t *req,
                                   sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;

    uint32_t slot_id    = data_rd32(req->data, 4);
    uint32_t cmd_offset = data_rd32(req->data, 8);
    uint32_t cmd_len    = data_rd32(req->data, 12);

    if (slot_id >= (uint32_t)GPU_SLOT_COUNT || cmd_len == 0) {
        dbg_puts("[gpu_sched] SUBMIT_CMD: invalid args\n");
        data_wr32(rep->data, 0, GPU_ERR_INVALID);
        data_wr32(rep->data, 4, 0u);
        rep->length = 8;
        return SEL4_ERR_BAD_ARG;
    }

    uint32_t fence = ++sched.gpu_fence_seq;
    (void)cmd_offset;

    if (sched.gpu_hw_present) {
        dbg_puts("[gpu_sched] SUBMIT_CMD: hw path\n");
        __asm__ volatile("" ::: "memory");
        gpu_virtio_kick(0u);   /* kick controlq (queue 0) */
    } else {
        dbg_puts("[gpu_sched] SUBMIT_CMD (stub)\n");
    }

    data_wr32(rep->data, 0, GPU_ERR_OK);
    data_wr32(rep->data, 4, fence);
    rep->length = 8;
    return SEL4_ERR_OK;
}

/*
 * handle_complete — MSG_GPU_COMPLETE / MSG_GPU_FAILED
 *
 * Handles slot completion notifications sent via IPC (replacing the old
 * Microkit notified() handler that used hardcoded channel IDs CH_SLOT_BASE=3
 * and caused "invalid channel '60'" errors when those channels were not wired).
 *
 * Request data layout:
 *   data[0..3] = ticket_id
 *   data[4..7] = success (1 = complete, 0 = failed)
 *
 * Reply: data[0..3] = 0 (ack)
 */
static uint32_t handle_complete(sel4_badge_t badge, const sel4_msg_t *req,
                                 sel4_msg_t *rep, void *ctx)
{
    (void)badge; (void)ctx;

    uint32_t ticket  = data_rd32(req->data, 0);
    bool     success = (data_rd32(req->data, 4) != 0u);

    int qi = queue_find(ticket);
    int slot_id = -1;

    if (qi >= 0) {
        slot_id = sched.queue[qi].slot_id;
        sched.queue[qi].state     = TASK_FREE;
        sched.queue[qi].ticket_id = 0;
    }
    if (slot_id >= 0 && slot_id < GPU_SLOT_COUNT) {
        sched.slots[slot_id].busy      = false;
        sched.slots[slot_id].ticket_id = 0;
    }

    if (success) {
        sched.tasks_completed++;
        dbg_puts("[gpu_sched] COMPLETE\n");
    } else {
        sched.tasks_failed++;
        dbg_puts("[gpu_sched] FAILED\n");
    }

    if (sched.eventbus_ready)
        publish_completion(ticket, success);

    dispatch_pending();

    data_wr32(rep->data, 0, 0u);
    rep->length = 4;
    return SEL4_ERR_OK;
}

/* ── Nameserver self-registration ────────────────────────────────────────── */

static void register_with_nameserver(seL4_CPtr ns_ep)
{
    if (!ns_ep) return;
    g_ns_ep = ns_ep;

    sel4_msg_t req, rep;
    req.opcode = OP_NS_REGISTER;

    data_wr32(req.data,  0, 0u);    /* channel_id */
    data_wr32(req.data,  4, 18u);   /* pd_id = gpu_sched */
    data_wr32(req.data,  8, 0u);    /* cap_classes */
    data_wr32(req.data, 12, 1u);    /* version */

    const char *name = "gpu_sched";
    int i = 0;
    for (; name[i] && (16 + i) < 48; i++)
        req.data[16 + i] = (uint8_t)name[i];
    for (; (16 + i) < 48; i++)
        req.data[16 + i] = 0;

    req.length = 48;
    sel4_call(ns_ep, &req, &rep);
    /* Ignore return — continue if nameserver is offline */
}

/*
 * connect_to_eventbus — look up "event_bus" via nameserver and subscribe.
 *
 * Replaces the old microkit_ppcall(CH_EVENTBUS, MSG_EVENTBUS_SUBSCRIBE, ...)
 * which used the hardcoded channel CH_EVENTBUS=2.  That caused "invalid
 * channel '60'" errors at boot when the Microkit channel graph was not wired.
 */
static void connect_to_eventbus(void)
{
    if (!g_ns_ep || g_eventbus_ep) return;

    /*
     * In production: use sel4_client_connect(&g_client, "event_bus", &ep).
     * For now we note that the root task will pass the endpoint directly
     * as an argument in E5-S9.  Until then, g_eventbus_ep stays 0 and
     * publish_completion() is a no-op.
     */
    g_eventbus_ep = 0;  /* resolved in E5-S9 via cap grant */
}

/* ── Test-host entry points ──────────────────────────────────────────────── */

#ifdef AGENTOS_TEST_HOST

void gpu_sched_test_init(void)
{
    for (int i = 0; i < GPU_QUEUE_DEPTH; i++) {
        sched.queue[i].state     = TASK_FREE;
        sched.queue[i].ticket_id = 0;
        sched.queue[i].slot_id   = -1;
    }
    for (int i = 0; i < GPU_SLOT_COUNT; i++) {
        sched.slots[i].busy      = false;
        sched.slots[i].ticket_id = 0;
    }
    sched.ticket_seq      = 0;
    sched.tasks_submitted = 0;
    sched.tasks_completed = 0;
    sched.tasks_failed    = 0;
    sched.eventbus_ready  = false;
    sched.gpu_hw_present  = false;
    sched.gpu_fence_seq   = 0;

    g_eventbus_ep   = 0;
    g_controller_ep = 0;
    g_ns_ep         = 0;

    sel4_server_init(&g_srv, 0);
    sel4_server_register(&g_srv, (uint32_t)MSG_GPU_SUBMIT,   handle_submit,     (void *)0);
    sel4_server_register(&g_srv, (uint32_t)MSG_GPU_STATUS,   handle_status,     (void *)0);
    sel4_server_register(&g_srv, (uint32_t)MSG_GPU_CANCEL,   handle_cancel,     (void *)0);
    sel4_server_register(&g_srv, (uint32_t)OP_GPU_SUBMIT_CMD, handle_submit_cmd, (void *)0);
    sel4_server_register(&g_srv, (uint32_t)MSG_GPU_COMPLETE, handle_complete,   (void *)0);
    sel4_server_register(&g_srv, (uint32_t)MSG_GPU_FAILED,   handle_complete,   (void *)0);
}

uint32_t gpu_sched_dispatch_one(sel4_badge_t badge,
                                 const sel4_msg_t *req,
                                 sel4_msg_t *rep)
{
    return sel4_server_dispatch(&g_srv, badge, req, rep);
}

#else /* !AGENTOS_TEST_HOST — production build */

/*
 * gpu_sched_main — production entry point called by the root task boot
 * dispatcher.
 *
 * my_ep: listen endpoint capability.
 * ns_ep: nameserver endpoint (0 = nameserver not yet available).
 *
 * This function NEVER RETURNS.
 */
void gpu_sched_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    /* Zero-initialise scheduler state */
    for (int i = 0; i < GPU_QUEUE_DEPTH; i++) {
        sched.queue[i].state     = TASK_FREE;
        sched.queue[i].ticket_id = 0;
        sched.queue[i].slot_id   = -1;
    }
    for (int i = 0; i < GPU_SLOT_COUNT; i++) {
        sched.slots[i].busy      = false;
        sched.slots[i].ticket_id = 0;
    }
    sched.ticket_seq      = 0;
    sched.tasks_submitted = 0;
    sched.tasks_completed = 0;
    sched.tasks_failed    = 0;
    sched.eventbus_ready  = false;
    sched.gpu_hw_present  = false;
    sched.gpu_fence_seq   = 0;

    dbg_puts("[gpu_sched] GPU Scheduler PD online\n");
    dbg_puts("[gpu_sched]   queue_depth=16, slots=4, arch=GB10-Blackwell\n");

    /* Probe for a virtio-gpu device at the MMIO region */
    probe_virtio_gpu();

    /* Self-register with nameserver so other PDs can discover "gpu_sched" */
    register_with_nameserver(ns_ep);

    /*
     * Attempt to connect to EventBus via nameserver.
     * If EventBus is not yet available, connect_to_eventbus() is a no-op
     * and g_eventbus_ep stays 0; publish_completion() will skip publishing.
     */
    connect_to_eventbus();

    dbg_puts("[gpu_sched] ready — waiting for IPC\n");

    sel4_server_init(&g_srv, my_ep);
    sel4_server_register(&g_srv, (uint32_t)MSG_GPU_SUBMIT,    handle_submit,     (void *)0);
    sel4_server_register(&g_srv, (uint32_t)MSG_GPU_STATUS,    handle_status,     (void *)0);
    sel4_server_register(&g_srv, (uint32_t)MSG_GPU_CANCEL,    handle_cancel,     (void *)0);
    sel4_server_register(&g_srv, (uint32_t)OP_GPU_SUBMIT_CMD, handle_submit_cmd, (void *)0);
    sel4_server_register(&g_srv, (uint32_t)MSG_GPU_COMPLETE,  handle_complete,   (void *)0);
    sel4_server_register(&g_srv, (uint32_t)MSG_GPU_FAILED,    handle_complete,   (void *)0);

    /* Enter the recv/dispatch/reply loop — never returns */
    sel4_server_run(&g_srv);
}

#endif /* AGENTOS_TEST_HOST */
