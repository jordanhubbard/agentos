/*
 * agentOS GPU Scheduler Protection Domain
 *
 * Priority 120 (PRIO_INTERACTIVE range).
 *
 * The GPU scheduler is the control plane for CUDA workload routing on Sparky's
 * 192GB Blackwell GB10. It queues agent task requests, dispatches them to
 * available compute WASM slots, and publishes completion events to the EventBus.
 *
 * IPC Design:
 *   - Agents PPC MSG_GPU_SUBMIT → get back a ticket_id (or error)
 *   - Slots complete → notify gpu_sched on CH_SLOT_DONE
 *   - EventBus publish MSG_GPU_COMPLETE / MSG_GPU_FAILED on completion
 *   - Natasha's gpu-idle-routing.mjs queries MSG_GPU_STATUS for scheduling hints
 *
 * Queue: fixed-size ring (GPU_QUEUE_DEPTH=16), priority-ordered (higher wins).
 * Slots: GPU_SLOT_COUNT=4 parallel CUDA compute slots (from MAX_SWAP_SLOTS).
 *
 * This PD does NOT perform CUDA calls — it routes work to WASM agents that
 * hold CUDA contexts. On Sparky, WASM agents call CUDA via host-function
 * imports exposed by wasm3_host.c (cuda_malloc, cuda_launch, cuda_sync).
 */

#include "agentos.h"
#include "gpu_sched.h"
#include <stdint.h>
#include <string.h>

/* ── virtio-gpu MMIO ─────────────────────────────────────────────────────── */

/*
 * VIRTIO_GPU_DEVICE_ID is defined in gpu_sched.h (value: 16).
 * virtio-MMIO register offsets are also declared there; the common
 * VIRTIO_MMIO_* macros come in via the gpu_sched.h include guard chain.
 */

/* virtio-gpu MMIO base (set by Microkit via setvar_vaddr) */
uintptr_t virtio_gpu_mmio_vaddr;
/* log_drain_rings shmem (set by Microkit via setvar_vaddr) */
uintptr_t log_drain_rings_vaddr;

/* true once probe_virtio_gpu() detects a live virtio-gpu device */
static bool gpu_hw_present = false;

/* Monotonic fence counter for OP_GPU_SUBMIT_CMD replies */
static uint32_t gpu_fence_seq = 0;

/* ── MMIO helpers (mirrors net_server.c) ────────────────────────────────── */

static inline uint32_t gpu_mmio_read32(uintptr_t base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}
static inline void gpu_mmio_write32(uintptr_t base, uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(base + off) = val;
}

/* ── gpu_virtio_kick ─────────────────────────────────────────────────────── */

/*
 * gpu_virtio_kick — notify the virtio-gpu device that a queue has new work.
 *
 * Writes queue_id to VIRTIO_MMIO_QUEUE_NOTIFY (offset 0x50), which causes
 * the QEMU/KVM virtio-gpu backend to process any descriptors we have placed
 * in the virtqueue.
 */
static void gpu_virtio_kick(uint32_t queue_id) {
    if (!virtio_gpu_mmio_vaddr)
        return;
    gpu_mmio_write32(virtio_gpu_mmio_vaddr, VIRTIO_MMIO_QUEUE_NOTIFY, queue_id);
}

/* ── probe_virtio_gpu ────────────────────────────────────────────────────── */

/*
 * probe_virtio_gpu — probe MMIO for a virtio-gpu device.
 * Pattern identical to probe_virtio_net() in net_server.c.
 * Called from init() below.
 */
void probe_virtio_gpu(void) {
    if (!virtio_gpu_mmio_vaddr) {
        log_drain_write(15, 15, "[gpu_sched] virtio-gpu: MMIO vaddr not mapped, stub mode\n");
        return;
    }

    uint32_t magic     = gpu_mmio_read32(virtio_gpu_mmio_vaddr, VIRTIO_MMIO_MAGIC_VALUE);
    uint32_t version   = gpu_mmio_read32(virtio_gpu_mmio_vaddr, VIRTIO_MMIO_VERSION);
    uint32_t device_id = gpu_mmio_read32(virtio_gpu_mmio_vaddr, VIRTIO_MMIO_DEVICE_ID);

    if (magic != VIRTIO_MMIO_MAGIC || version != 2u
            || device_id != VIRTIO_GPU_DEVICE_ID) {
        log_drain_write(15, 15, "[gpu_sched] virtio-gpu not detected (magic/ver/dev mismatch), stub mode\n");
        return;
    }

    gpu_hw_present = true;

    /*
     * Minimal device initialisation (virtio spec §3.1.1):
     *   ACKNOWLEDGE → DRIVER → (feature negotiation deferred)
     * Full virtqueue setup (descriptor table allocation, QUEUE_READY) is
     * deferred to the gpu_shmem integration phase.
     */
    gpu_mmio_write32(virtio_gpu_mmio_vaddr, VIRTIO_MMIO_STATUS,
                     VIRTIO_STATUS_ACKNOWLEDGE);
    gpu_mmio_write32(virtio_gpu_mmio_vaddr, VIRTIO_MMIO_STATUS,
                     VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);

    log_drain_write(15, 15, "[gpu_sched] virtio-gpu detected: hw present, MMIO at ");
    {
        /* Inline hex log — no libc */
        static const char h[] = "0123456789abcdef";
        char buf[11];
        uint32_t v = (uint32_t)virtio_gpu_mmio_vaddr;
        buf[0]='0'; buf[1]='x';
        for (int k = 0; k < 8; k++)
            buf[2+k] = h[(v >> (28 - k*4)) & 0xf];
        buf[10] = '\0';
        log_drain_write(15, 15, buf);
    }
    log_drain_write(15, 15, "\n");
}

/* ── Channel IDs (from gpu_sched's perspective in agentos.system) ──────────── */
#define CH_CONTROLLER   1   /* controller <-> gpu_sched */
#define CH_EVENTBUS     2   /* eventbus   <-> gpu_sched */
/* Channels 3..6 reserved for slot completion notifications */
#define CH_SLOT_BASE    3   /* CH_SLOT_BASE+i = done notification from slot i */

/* ── Constants ─────────────────────────────────────────────────────────────── */
#define GPU_QUEUE_DEPTH     16
#define GPU_SLOT_COUNT      4
#define GPU_TICKET_NONE     0
#define GPU_PRIO_DEFAULT    50
#define GPU_PRIO_HIGH       100
#define GPU_PRIO_RT         200   /* real-time — front of queue */

/* Error codes returned in MR1 of GPU_SUBMIT_REPLY */
#define GPU_ERR_OK          0
#define GPU_ERR_QUEUE_FULL  1
#define GPU_ERR_INVALID     2
#define GPU_ERR_NO_SLOTS    3   /* submitted OK but no slot free right now */

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
    uint32_t    ticket_id;    /* monotonic ID, 0 = free */
    uint64_t    wasm_hash_lo; /* WASM module to execute (low 64 bits of BLAKE3) */
    uint64_t    wasm_hash_hi;
    uint32_t    priority;     /* higher = sooner */
    uint32_t    flags;        /* reserved */
    task_state_t state;
    int32_t     slot_id;      /* -1 if not dispatched */
    uint32_t    submitter;    /* channel of submitting agent (for completion routing) */
} gpu_task_t;

/* ── Slot state ──────────────────────────────────────────────────────────── */
typedef struct {
    bool        busy;
    uint32_t    ticket_id;  /* currently executing ticket, 0 if idle */
} gpu_slot_t;

/* ── Scheduler state ──────────────────────────────────────────────────────── */
static struct {
    gpu_task_t  queue[GPU_QUEUE_DEPTH];
    gpu_slot_t  slots[GPU_SLOT_COUNT];
    uint32_t    ticket_seq;         /* monotonic ticket counter */
    uint32_t    tasks_submitted;
    uint32_t    tasks_completed;
    uint32_t    tasks_failed;
    bool        eventbus_ready;
} sched;

/* ── Helpers ──────────────────────────────────────────────────────────────── */

static void put_dec(uint32_t v) {
    log_drain_write(15, 15, "0");
    char buf[12]; int i = 11; buf[i] = '\0';
    while (v > 0 && i > 0) { buf[--i] = '0' + (v % 10); v /= 10; }
    log_drain_write(15, 15, &buf[i]);
}

/* Find a free queue slot */
static int queue_alloc(void) {
    for (int i = 0; i < GPU_QUEUE_DEPTH; i++) {
        if (sched.queue[i].state == TASK_FREE) return i;
    }
    return -1;
}

/* Find a free compute slot */
static int slot_alloc(void) {
    for (int i = 0; i < GPU_SLOT_COUNT; i++) {
        if (!sched.slots[i].busy) return i;
    }
    return -1;
}

/* Find the highest-priority QUEUED task */
static int queue_pick_best(void) {
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

/* Find queue entry by ticket_id */
static int queue_find(uint32_t ticket_id) {
    for (int i = 0; i < GPU_QUEUE_DEPTH; i++) {
        if (sched.queue[i].ticket_id == ticket_id &&
            sched.queue[i].state != TASK_FREE) return i;
    }
    return -1;
}

/* ── Dispatch logic ────────────────────────────────────────────────────────── */

/*
 * Dispatch the next queued task to a free slot.
 * Notifies the controller with MSG_WORKER_RETRIEVE to fetch the WASM from AgentFS.
 * The controller -> worker -> CUDA pipeline handles actual execution.
 */
static void dispatch_pending(void) {
    for (;;) {
        int slot = slot_alloc();
        if (slot < 0) return;  /* all slots busy */

        int task = queue_pick_best();
        if (task < 0) return;  /* nothing queued */

        gpu_task_t *t = &sched.queue[task];
        t->state    = TASK_RUNNING;
        t->slot_id  = slot;
        sched.slots[slot].busy      = true;
        sched.slots[slot].ticket_id = t->ticket_id;

        log_drain_write(15, 15, "[gpu_sched] Dispatching ticket=");
        put_dec(t->ticket_id);
        log_drain_write(15, 15, " to slot=");
        put_dec((uint32_t)slot);
        log_drain_write(15, 15, " prio=");
        put_dec(t->priority);
        log_drain_write(15, 15, "\n");

        /*
         * Notify controller to route this WASM hash to the appropriate
         * worker / swap slot. Layout:
         *   MR0: MSG_GPU_SUBMIT (tag for controller)
         *   MR1: ticket_id
         *   MR2: wasm_hash_lo low32
         *   MR3: wasm_hash_lo hi32
         *   MR4: wasm_hash_hi low32
         *   MR5: slot_id
         */
        microkit_mr_set(0, (uint64_t)MSG_GPU_SUBMIT);
        microkit_mr_set(1, t->ticket_id);
        microkit_mr_set(2, (uint32_t)(t->wasm_hash_lo & 0xFFFFFFFF));
        microkit_mr_set(3, (uint32_t)((t->wasm_hash_lo >> 32) & 0xFFFFFFFF));
        microkit_mr_set(4, (uint32_t)(t->wasm_hash_hi & 0xFFFFFFFF));
        microkit_mr_set(5, (uint32_t)slot);
        microkit_notify(CH_CONTROLLER);
    }
}

/* Publish completion event to EventBus */
static void publish_completion(uint32_t ticket_id, bool success) {
    uint64_t tag = success ? (uint64_t)MSG_GPU_COMPLETE : (uint64_t)MSG_GPU_FAILED;
    microkit_mr_set(0, (uint64_t)MSG_EVENT_PUBLISH);
    microkit_mr_set(1, tag);
    microkit_mr_set(2, ticket_id);
    microkit_mr_set(3, (uint32_t)(success ? 0 : 1));
    microkit_ppcall(CH_EVENTBUS, microkit_msginfo_new(MSG_EVENT_PUBLISH, 4));
}

/* ── protected() — synchronous PPC handler ────────────────────────────────── */

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msg) {
    (void)ch;
    uint64_t tag = microkit_msginfo_get_label(msg);

    switch ((uint32_t)tag) {

    case MSG_GPU_SUBMIT: {
        /*
         * Agent submits a GPU task.
         * MR0/1: wasm_hash_lo, MR2/3: wasm_hash_hi, MR4: priority, MR5: flags
         */
        uint64_t hash_lo = (uint64_t)microkit_mr_get(0) |
                           ((uint64_t)microkit_mr_get(1) << 32);
        uint64_t hash_hi = (uint64_t)microkit_mr_get(2) |
                           ((uint64_t)microkit_mr_get(3) << 32);
        uint32_t priority = (uint32_t)microkit_mr_get(4);
        uint32_t flags    = (uint32_t)microkit_mr_get(5);

        if (priority == 0) priority = GPU_PRIO_DEFAULT;

        int qi = queue_alloc();
        if (qi < 0) {
            log_drain_write(15, 15, "[gpu_sched] SUBMIT: queue full\n");
            microkit_mr_set(0, 0);
            microkit_mr_set(1, GPU_ERR_QUEUE_FULL);
            return microkit_msginfo_new(MSG_GPU_SUBMIT_REPLY, 2);
        }

        uint32_t ticket = ++sched.ticket_seq;
        gpu_task_t *t  = &sched.queue[qi];
        t->ticket_id   = ticket;
        t->wasm_hash_lo = hash_lo;
        t->wasm_hash_hi = hash_hi;
        t->priority    = priority;
        t->flags       = flags;
        t->state       = TASK_QUEUED;
        t->slot_id     = -1;
        t->submitter   = (uint32_t)ch;
        sched.tasks_submitted++;

        log_drain_write(15, 15, "[gpu_sched] SUBMIT: ticket=");
        put_dec(ticket);
        log_drain_write(15, 15, " prio=");
        put_dec(priority);
        log_drain_write(15, 15, " queued\n");

        /* Try to dispatch immediately if a slot is free */
        dispatch_pending();

        microkit_mr_set(0, ticket);
        microkit_mr_set(1, GPU_ERR_OK);
        return microkit_msginfo_new(MSG_GPU_SUBMIT_REPLY, 2);
    }

    case MSG_GPU_STATUS: {
        /*
         * Return scheduler health metrics.
         * MR0: queue_depth (queued tasks), MR1: running, MR2: idle_slots
         * MR3: tasks_completed, MR4: tasks_failed
         */
        uint32_t queued  = 0, running = 0, idle = 0;
        for (int i = 0; i < GPU_QUEUE_DEPTH; i++) {
            if (sched.queue[i].state == TASK_QUEUED)  queued++;
            if (sched.queue[i].state == TASK_RUNNING) running++;
        }
        for (int i = 0; i < GPU_SLOT_COUNT; i++) {
            if (!sched.slots[i].busy) idle++;
        }
        microkit_mr_set(0, queued);
        microkit_mr_set(1, running);
        microkit_mr_set(2, idle);
        microkit_mr_set(3, sched.tasks_completed);
        microkit_mr_set(4, sched.tasks_failed);
        return microkit_msginfo_new(MSG_GPU_STATUS_REPLY, 5);
    }

    case MSG_GPU_CANCEL: {
        uint32_t ticket = (uint32_t)microkit_mr_get(0);
        int qi = queue_find(ticket);
        if (qi < 0) {
            microkit_mr_set(0, 0);
            return microkit_msginfo_new(MSG_GPU_CANCEL_REPLY, 1);
        }
        if (sched.queue[qi].state == TASK_QUEUED) {
            sched.queue[qi].state = TASK_FREE;
            sched.queue[qi].ticket_id = 0;
            log_drain_write(15, 15, "[gpu_sched] CANCEL: ticket=");
            put_dec(ticket);
            log_drain_write(15, 15, " removed from queue\n");
            microkit_mr_set(0, 1);  /* cancelled */
        } else {
            microkit_mr_set(0, 0);  /* running or done, can't cancel */
        }
        return microkit_msginfo_new(MSG_GPU_CANCEL_REPLY, 1);
    }

    case OP_GPU_SUBMIT_CMD: {
        /*
         * OP_GPU_SUBMIT_CMD (0xE4) — submit a virtio-gpu command buffer.
         *
         * MR1 = slot_id    (gpu_shmem slot)
         * MR2 = cmd_offset (byte offset into gpu_shmem payload area)
         * MR3 = cmd_len    (command buffer length in bytes)
         *
         * Reply:
         * MR0 = GPU_ERR_OK or GPU_ERR_INVALID
         * MR1 = fence_id (monotonic; 0 in stub mode)
         */
        uint32_t slot_id    = (uint32_t)microkit_mr_get(1);
        uint32_t cmd_offset = (uint32_t)microkit_mr_get(2);
        uint32_t cmd_len    = (uint32_t)microkit_mr_get(3);

        if (slot_id >= GPU_SLOT_COUNT || cmd_len == 0) {
            log_drain_write(15, 15, "[gpu_sched] SUBMIT_CMD: invalid args\n");
            microkit_mr_set(0, GPU_ERR_INVALID);
            microkit_mr_set(1, 0);
            return microkit_msginfo_new(OP_GPU_SUBMIT_CMD, 2);
        }

        uint32_t fence = ++gpu_fence_seq;

        if (gpu_hw_present) {
            /*
             * Forward command buffer to virtio-gpu virtqueue (queue 0 =
             * controlq).  A production implementation would:
             *   1. Acquire a free descriptor from the descriptor table.
             *   2. Write desc.addr  = physical address of gpu_shmem + cmd_offset
             *      desc.len   = cmd_len
             *      desc.flags = 0 (device-readable)
             *   3. Update the available ring: avail->ring[avail->idx] = desc_idx
             *      avail->idx++
             *   4. Memory barrier to ensure writes are visible.
             *   5. Kick the queue.
             *
             * For now the descriptor table is not yet allocated (deferred to
             * gpu_shmem integration); we kick the queue to signal readiness
             * and log the submission.
             */
            log_drain_write(15, 15, "[gpu_sched] SUBMIT_CMD: hw path slot=");
            put_dec(slot_id);
            log_drain_write(15, 15, " off=");
            put_dec(cmd_offset);
            log_drain_write(15, 15, " len=");
            put_dec(cmd_len);
            log_drain_write(15, 15, " fence=");
            put_dec(fence);
            log_drain_write(15, 15, "\n");

            /* Memory barrier before kicking queue */
            __asm__ volatile("" ::: "memory");
            gpu_virtio_kick(0u);   /* kick controlq (queue 0) */
        } else {
            /* Stub mode: log and return success with a fake fence */
            log_drain_write(15, 15, "[gpu_sched] SUBMIT_CMD (stub): slot=");
            put_dec(slot_id);
            log_drain_write(15, 15, " off=");
            put_dec(cmd_offset);
            log_drain_write(15, 15, " len=");
            put_dec(cmd_len);
            log_drain_write(15, 15, " fence=");
            put_dec(fence);
            log_drain_write(15, 15, "\n");
        }

        microkit_mr_set(0, GPU_ERR_OK);
        microkit_mr_set(1, fence);
        return microkit_msginfo_new(OP_GPU_SUBMIT_CMD, 2);
    }

    default:
        microkit_mr_set(0, 0xFFFF);
        return microkit_msginfo_new(0xFFFF, 1);
    }
}

/* ── notified() — async events ────────────────────────────────────────────── */

void notified(microkit_channel ch) {
    if (ch == CH_CONTROLLER) {
        /*
         * Controller notifying gpu_sched:
         * MR0 tag tells us what happened.
         *   MSG_GPU_COMPLETE: slot finished, ticket in MR1, success in MR2
         *   MSG_EVENTBUS_READY: EventBus is up — subscribe now
         */
        uint32_t ntag = (uint32_t)microkit_mr_get(0);

        if (ntag == (uint32_t)MSG_GPU_COMPLETE || ntag == (uint32_t)MSG_GPU_FAILED) {
            uint32_t ticket  = (uint32_t)microkit_mr_get(1);
            bool     success = (ntag == (uint32_t)MSG_GPU_COMPLETE);
            int qi = queue_find(ticket);
            int slot_id = -1;

            if (qi >= 0) {
                slot_id = sched.queue[qi].slot_id;
                sched.queue[qi].state = success ? TASK_DONE : TASK_FAILED;
                sched.queue[qi].ticket_id = 0;  /* free the slot */
                sched.queue[qi].state = TASK_FREE;
            }
            if (slot_id >= 0 && slot_id < GPU_SLOT_COUNT) {
                sched.slots[slot_id].busy = false;
                sched.slots[slot_id].ticket_id = 0;
            }

            if (success) {
                sched.tasks_completed++;
                log_drain_write(15, 15, "[gpu_sched] COMPLETE: ticket=");
            } else {
                sched.tasks_failed++;
                log_drain_write(15, 15, "[gpu_sched] FAILED: ticket=");
            }
            put_dec(ticket);
            log_drain_write(15, 15, "\n");

            /* Publish to EventBus */
            if (sched.eventbus_ready) {
                publish_completion(ticket, success);
            }

            /* Try to dispatch next pending task */
            dispatch_pending();
        }
        return;
    }

    /* Slot done notifications (CH_SLOT_BASE .. CH_SLOT_BASE + GPU_SLOT_COUNT - 1) */
    if (ch >= CH_SLOT_BASE && ch < CH_SLOT_BASE + GPU_SLOT_COUNT) {
        int slot = (int)(ch - CH_SLOT_BASE);
        uint32_t ticket = sched.slots[slot].ticket_id;

        log_drain_write(15, 15, "[gpu_sched] Slot ");
        put_dec((uint32_t)slot);
        log_drain_write(15, 15, " done, ticket=");
        put_dec(ticket);
        log_drain_write(15, 15, "\n");

        int qi = queue_find(ticket);
        if (qi >= 0) {
            /* Assume success on clean slot notification */
            sched.queue[qi].state = TASK_FREE;
            sched.queue[qi].ticket_id = 0;
        }
        sched.slots[slot].busy = false;
        sched.slots[slot].ticket_id = 0;
        sched.tasks_completed++;

        if (sched.eventbus_ready) {
            publish_completion(ticket, true);
        }
        dispatch_pending();
        return;
    }

    if (ch == CH_EVENTBUS) {
        sched.eventbus_ready = true;
        log_drain_write(15, 15, "[gpu_sched] EventBus ready\n");

        /* Subscribe to EventBus */
        microkit_mr_set(0, (uint64_t)CH_EVENTBUS);
        microkit_mr_set(1, 0);
        microkit_ppcall(CH_EVENTBUS, microkit_msginfo_new(MSG_EVENTBUS_SUBSCRIBE, 2));
    }
}

/* ── init() ─────────────────────────────────────────────────────────────────── */

void init(void) {
    /* Zero-initialise scheduler state */
    for (int i = 0; i < GPU_QUEUE_DEPTH; i++) {
        sched.queue[i].state = TASK_FREE;
        sched.queue[i].ticket_id = 0;
        sched.queue[i].slot_id = -1;
    }
    for (int i = 0; i < GPU_SLOT_COUNT; i++) {
        sched.slots[i].busy = false;
        sched.slots[i].ticket_id = 0;
    }
    sched.ticket_seq       = 0;
    sched.tasks_submitted  = 0;
    sched.tasks_completed  = 0;
    sched.tasks_failed     = 0;
    sched.eventbus_ready   = false;

    log_drain_write(15, 15, "[gpu_sched] GPU Scheduler PD online\n[gpu_sched]   queue_depth=16, slots=4, arch=GB10-Blackwell\n");

    /* Probe for a virtio-gpu device at the MMIO region */
    probe_virtio_gpu();

    /* Signal controller: GPU scheduler ready */
    microkit_notify(CH_CONTROLLER);
}
