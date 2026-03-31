/*
 * agentOS Fault Handler Protection Domain
 *
 * Passive PD (priority 250 — highest in system) that receives seL4 fault
 * notifications for registered PDs and handles them gracefully instead of
 * allowing the kernel to silently abort the offending thread.
 *
 * Fault types handled:
 *   VM_FAULT    — invalid memory access (null deref, stack overflow, OOB)
 *   CAP_FAULT   — capability violation (bad cap, wrong rights, revoked cap)
 *   UNKNOWN_SYS — unknown syscall number
 *   USER_EXC    — userspace exception (undefined instruction, etc.)
 *
 * On any fault:
 *   1. Log fault context to fault_ring shared memory (256KB ring buffer)
 *   2. For CAP_FAULT: also write to cap_audit_log via IPC
 *   3. Notify watchdog channel so the offending agent can be respawned
 *   4. Reply to the faulting thread's fault endpoint (allows seL4 to resume
 *      or terminate it cleanly — we terminate by not providing a resume token)
 *
 * IPC operations (from controller/init_agent):
 *   OP_FAULT_STATUS (0x60) — query ring buffer state
 *   OP_FAULT_DUMP   (0x61) — read recent fault entries
 *   OP_FAULT_CLEAR  (0x62) — clear the fault ring buffer
 *
 * Channels:
 *   id=0: controller    -> fault_handler (PPC: status/dump queries)
 *   id=1: init_agent    -> fault_handler (PPC: status/dump queries)
 *   id=2: fault_handler -> controller    (notify: trigger respawn)
 *   (cap faults logged in ring — controller forwards to cap_audit_log)
 *
 * seL4 Microkit fault handling:
 *   When a PD registers fault_handler as its fault endpoint, seL4 delivers
 *   fault messages to fault_handler's protected procedure channel.
 *   The fault message format (seL4 IPC):
 *     MR0: fault_type (seL4_Fault_VMFault=1, seL4_Fault_CapFault=2, etc.)
 *     MR1: fault_addr (for VM/cap faults)
 *     MR2: fault_ip   (instruction pointer at fault)
 *     MR3: fault_data (FSR/cause word)
 *
 * Memory:
 *   fault_ring (256KB shared MR): ring buffer for fault log entries
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"

/* ── Opcodes ──────────────────────────────────────────────────────────────── */
#define OP_FAULT_STATUS   0x60   /* Query ring state: MR0=count, MR1=head, MR2=cap */
#define OP_FAULT_DUMP     0x61   /* Read entries: MR1=start_idx, MR2=count */
#define OP_FAULT_CLEAR    0x62   /* Clear ring buffer */

/* ── Fault type constants (mirrors seL4 fault codes) ─────────────────────── */
#define FAULT_VM_FAULT    1      /* seL4_Fault_VMFault */
#define FAULT_CAP_FAULT   2      /* seL4_Fault_CapFault */
#define FAULT_UNKNOWN_SYS 7      /* seL4_Fault_UnknownSyscall */
#define FAULT_USER_EXC    8      /* seL4_Fault_UserException */

/* ── Fault log entry (48 bytes) ───────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint64_t seq;           /* monotonic sequence */
    uint64_t tick;          /* boot tick at fault time */
    uint32_t fault_type;    /* FAULT_VM_FAULT, FAULT_CAP_FAULT, etc. */
    uint32_t pd_id;         /* faulting protection domain id (channel that faulted) */
    uint64_t fault_addr;    /* faulting address (VMFault) or cap slot (CapFault) */
    uint64_t fault_ip;      /* instruction pointer at time of fault */
    uint32_t fault_data;    /* arch-specific fault status register / cause word */
    uint32_t flags;         /* VM: is_write bit; Cap: is_receive bit */
    uint8_t  _pad[4];       /* pad to 48 bytes */
} fault_entry_t;

/* ── Ring buffer header (64 bytes) ───────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t magic;         /* 0xFA17DEAD ("FAULT DEAD") */
    uint32_t version;       /* 1 */
    uint64_t capacity;      /* number of entry slots */
    uint64_t head;          /* next write index */
    uint64_t count;         /* total faults logged */
    uint64_t drops;         /* entries overwritten (ring full) */
    uint8_t  _pad[16];      /* pad to 64 bytes */
} fault_ring_header_t;

#define FAULT_RING_MAGIC  0xFA17DEAD

/* ── Shared memory ────────────────────────────────────────────────────────── */
uintptr_t fault_ring_vaddr;

#define FAULT_HDR     ((volatile fault_ring_header_t *)fault_ring_vaddr)
#define FAULT_ENTRIES ((volatile fault_entry_t *) \
    ((uint8_t *)fault_ring_vaddr + sizeof(fault_ring_header_t)))

/* Channel IDs from fault_handler's perspective */
#define FH_CH_CONTROLLER    0   /* controller queries */
#define FH_CH_INIT_AGENT    1   /* init_agent queries */
#define FH_CH_WATCHDOG      2   /* notify controller/watchdog to respawn */

static uint64_t boot_tick = 0;

/* ── Init ─────────────────────────────────────────────────────────────────── */
static void fault_handler_init(void) {
    volatile fault_ring_header_t *hdr = FAULT_HDR;

    uint64_t region_size = 0x40000;  /* 256KB */
    uint64_t entry_space = region_size - sizeof(fault_ring_header_t);
    uint64_t cap = entry_space / sizeof(fault_entry_t);

    hdr->magic    = FAULT_RING_MAGIC;
    hdr->version  = 1;
    hdr->capacity = cap;
    hdr->head     = 0;
    hdr->count    = 0;
    hdr->drops    = 0;

    console_log(13, 13, "[fault_handler] Initialized. capacity=5000+ fault entries, 48B each\n");
}

/* ── Append fault entry to ring ───────────────────────────────────────────── */
static void fault_append(uint32_t fault_type, uint32_t pd_id,
                          uint64_t fault_addr, uint64_t fault_ip,
                          uint32_t fault_data, uint32_t flags) {
    volatile fault_ring_header_t *hdr    = FAULT_HDR;
    volatile fault_entry_t       *entries = FAULT_ENTRIES;

    uint64_t idx = hdr->head % hdr->capacity;
    volatile fault_entry_t *e = &entries[idx];

    e->seq        = hdr->count;
    e->tick       = boot_tick;
    e->fault_type = fault_type;
    e->pd_id      = pd_id;
    e->fault_addr = fault_addr;
    e->fault_ip   = fault_ip;
    e->fault_data = fault_data;
    e->flags      = flags;

    hdr->head = (hdr->head + 1) % hdr->capacity;
    if (hdr->count >= hdr->capacity) {
        hdr->drops++;
    }
    hdr->count++;
}

/* ── Log a human-readable fault message ───────────────────────────────────── */
static void fault_log_msg(uint32_t fault_type, uint32_t pd_id,
                           uint64_t fault_addr, uint64_t fault_ip) {
    console_log(13, 13, "[fault_handler] FAULT pd=");

    /* Print pd_id as decimal (simple) */
    char buf[12];
    int i = 0;
    uint32_t v = pd_id;
    if (v == 0) {
        buf[i++] = '0';
    } else {
        int start = i;
        while (v > 0) { buf[i++] = '0' + (v % 10); v /= 10; }
        /* reverse */
        for (int a = start, b = i - 1; a < b; a++, b--) {
            char tmp = buf[a]; buf[a] = buf[b]; buf[b] = tmp;
        }
    }
    buf[i] = '\0';
    console_log(13, 13, buf);

    switch (fault_type) {
    console_log(13, 13, " VM_FAULT CAP_FAULT UNKNOWN_SYS USER_EXC UNKNOWN");
    }

    console_log(13, 13, " addr=0x");

    /* Print fault_addr as hex */
    char hexbuf[17];
    int h = 0;
    uint64_t hv = fault_addr;
    if (hv == 0) {
        hexbuf[h++] = '0';
    } else {
        int hstart = h;
        while (hv > 0) {
            int nibble = hv & 0xF;
            hexbuf[h++] = nibble < 10 ? '0' + nibble : 'a' + nibble - 10;
            hv >>= 4;
        }
        for (int a = hstart, b = h - 1; a < b; a++, b--) {
            char tmp = hexbuf[a]; hexbuf[a] = hexbuf[b]; hexbuf[b] = tmp;
        }
    }
    hexbuf[h] = '\0';
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = hexbuf; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = " ip=0x"; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        console_log(13, 13, _cl_buf);
    }

    /* Print fault_ip as hex */
    h = 0;
    hv = fault_ip;
    if (hv == 0) {
        hexbuf[h++] = '0';
    } else {
        int hstart = h;
        while (hv > 0) {
            int nibble = hv & 0xF;
            hexbuf[h++] = nibble < 10 ? '0' + nibble : 'a' + nibble - 10;
            hv >>= 4;
        }
        for (int a = hstart, b = h - 1; a < b; a++, b--) {
            char tmp = hexbuf[a]; hexbuf[a] = hexbuf[b]; hexbuf[b] = tmp;
        }
    }
    hexbuf[h] = '\0';
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = hexbuf; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = "\n"; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        console_log(13, 13, _cl_buf);
    }
    (void)fault_ip; /* already used above */
}

/* ── Forward cap fault to cap_audit_log ───────────────────────────────────── */
static void forward_cap_fault_to_audit(uint32_t pd_id, uint64_t cap_slot) {
    /*
     * NOTE: fault_handler (priority 250) cannot PPC into cap_audit_log
     * (priority 120) — seL4 Microkit requires callers to be strictly lower
     * priority than callee for PPCs.
     *
     * Instead, we log the cap fault into our own ring buffer with type
     * FAULT_CAP_FAULT, and include a flag so controller can forward it to
     * cap_audit_log after receiving the watchdog notification.
     *
     * The fault entry already captures pd_id, fault_addr (cap slot), and
     * fault_type=FAULT_CAP_FAULT, so this is handled at query time.
     */
    (void)pd_id;
    (void)cap_slot;
    console_log(13, 13, "[fault_handler] CAP_FAULT logged in ring — controller will forward to cap_audit_log\n");
}

/* ── Notify watchdog (controller) to consider respawn ─────────────────────── */
static void notify_watchdog(uint32_t pd_id) {
    /* Send pd_id in MR0 so controller knows which PD faulted */
    microkit_mr_set(0, pd_id);
    microkit_notify(FH_CH_WATCHDOG);
}

/* ── Handle a fault notification from seL4 ───────────────────────────────── */
static void handle_fault(microkit_channel channel, microkit_msginfo msg) {
    /*
     * seL4 Microkit delivers faults as protected procedure calls on the
     * fault endpoint channel. The fault info is in the message registers:
     *   MR0: fault_type
     *   MR1: fault_addr (or cap slot for CapFault)
     *   MR2: fault_ip
     *   MR3: fault_data (FSR / exception cause)
     *   MR4: flags (is_write for VMFault, is_receive for CapFault)
     */
    uint32_t fault_type = (uint32_t)microkit_mr_get(0);
    uint64_t fault_addr = (uint64_t)microkit_mr_get(1);
    uint64_t fault_ip   = (uint64_t)microkit_mr_get(2);
    uint32_t fault_data = (uint32_t)microkit_mr_get(3);
    uint32_t flags      = (uint32_t)microkit_mr_get(4);

    /* The channel number tells us which PD faulted (each PD gets its own channel) */
    uint32_t pd_id = (uint32_t)channel;

    /* 1. Log to fault ring */
    fault_append(fault_type, pd_id, fault_addr, fault_ip, fault_data, flags);

    /* 2. Print debug output */
    fault_log_msg(fault_type, pd_id, fault_addr, fault_ip);

    /* 3. Forward cap faults to cap_audit_log */
    if (fault_type == FAULT_CAP_FAULT) {
        forward_cap_fault_to_audit(pd_id, fault_addr);
    }

    /* 4. Notify watchdog/controller to trigger respawn */
    notify_watchdog(pd_id);

    (void)msg;
}

/* ── Handle query IPC from controller/init_agent ─────────────────────────── */
static microkit_msginfo handle_query(microkit_msginfo msg) {
    uint32_t op = (uint32_t)microkit_mr_get(0);

    switch (op) {

    case OP_FAULT_STATUS: {
        volatile fault_ring_header_t *hdr = FAULT_HDR;
        microkit_mr_set(0, (uint32_t)(hdr->count & 0xFFFFFFFF));
        microkit_mr_set(1, (uint32_t)(hdr->head & 0xFFFFFFFF));
        microkit_mr_set(2, (uint32_t)(hdr->capacity & 0xFFFFFFFF));
        microkit_mr_set(3, (uint32_t)(hdr->drops & 0xFFFFFFFF));
        return microkit_msginfo_new(0, 4);
    }

    case OP_FAULT_DUMP: {
        volatile fault_ring_header_t *hdr    = FAULT_HDR;
        volatile fault_entry_t       *entries = FAULT_ENTRIES;

        uint32_t start_back = (uint32_t)microkit_mr_get(1);
        uint32_t req_count  = (uint32_t)microkit_mr_get(2);

        /* Cap at 2 entries per call (each entry needs 6 MRs = 12 MRs total) */
        if (req_count > 2) req_count = 2;

        uint64_t avail = hdr->count < hdr->capacity ? hdr->count : hdr->capacity;
        if (start_back >= avail || avail == 0) {
            microkit_mr_set(0, 0);
            return microkit_msginfo_new(0, 1);
        }

        uint32_t actual = req_count;
        if (start_back + actual > avail) {
            actual = (uint32_t)(avail - start_back);
        }

        microkit_mr_set(0, actual);
        for (uint32_t i = 0; i < actual; i++) {
            uint64_t idx = (hdr->head + hdr->capacity - 1 - start_back - i) % hdr->capacity;
            volatile fault_entry_t *e = &entries[idx];

            uint32_t mr_base = 1 + i * 6;
            microkit_mr_set(mr_base + 0, e->seq);
            microkit_mr_set(mr_base + 1, e->tick);
            microkit_mr_set(mr_base + 2, ((uint64_t)e->fault_type << 32) | e->pd_id);
            microkit_mr_set(mr_base + 3, e->fault_addr);
            microkit_mr_set(mr_base + 4, e->fault_ip);
            microkit_mr_set(mr_base + 5, ((uint64_t)e->fault_data << 32) | e->flags);
        }
        return microkit_msginfo_new(0, 1 + actual * 6);
    }

    case OP_FAULT_CLEAR: {
        volatile fault_ring_header_t *hdr = FAULT_HDR;
        hdr->head  = 0;
        hdr->count = 0;
        hdr->drops = 0;
        console_log(13, 13, "[fault_handler] Ring cleared by request\n");
        microkit_mr_set(0, 1);
        return microkit_msginfo_new(0, 1);
    }

    default:
        console_log(13, 13, "[fault_handler] WARN: unknown query opcode\n");
        microkit_mr_set(0, 0xFF);
        return microkit_msginfo_new(0, 1);
    }

    (void)msg;
}

/* ── Microkit entry points ────────────────────────────────────────────────── */
void init(void) {
    fault_handler_init();
    console_log(13, 13, "[fault_handler] Ready — priority 250, passive, monitoring all PD faults\n");
}

microkit_msginfo protected(microkit_channel channel, microkit_msginfo msg) {
    /*
     * Channels 0 and 1 are query channels from controller/init_agent.
     * Channels 60+ are fault endpoint channels for each registered PD.
     * (In seL4 Microkit, fault endpoints are delivered as PPC on the
     *  fault endpoint channel number assigned in the system description.)
     */
    if (channel <= 1) {
        /* Query from controller or init_agent */
        return handle_query(msg);
    } else {
        /* Fault notification from a PD */
        handle_fault(channel, msg);
        /* Return empty reply — seL4 will not resume the faulting thread */
        return microkit_msginfo_new(0, 0);
    }
}

void notified(microkit_channel ch) {
    (void)ch;
    boot_tick++;
}
