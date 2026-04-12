/*
 * agentOS Vibe Swap — Kernel-side hot-swap support
 *
 * This module enables the vibe-coding loop:
 *   Agent generates new service code → validates → swaps live service
 *
 * Architecture:
 *   The VibeEngine (userspace Rust server) handles the protocol:
 *   propose/validate/sandbox/approve. Once approved, it PPCs into
 *   the controller PD, which executes the swap at the kernel level.
 *
 * Swap mechanism (per Natasha's agent-pool insight):
 *   Microkit PDs are static — we can't create new ones at runtime.
 *   Instead, we use a "swap slot" model:
 *   
 *   1. The system description pre-allocates N swap-slot PDs
 *   2. Each slot is an idle worker PD with channels to the controller
 *   3. On swap: controller loads new code into a swap slot PD
 *   4. Controller redirects the service's channels to the new PD
 *   5. Old PD becomes the rollback slot (kept warm for quick revert)
 *
 *   This stays within seL4's verified TCB — no dynamic PD creation,
 *   no hypervisor layer, just channel redirection.
 *
 * Channel assignments:
 *   The controller has channels to all service PDs AND all swap slots.
 *   Swap slots are numbered SWAP_SLOT_0..SWAP_SLOT_N.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "barrier.h"
#include "prio_inherit.h"

/* Swap slot configuration */
#define MAX_SWAP_SLOTS       4
#define SWAP_SLOT_BASE_CH    30  /* Channel IDs 30-33: swap slot PDs */

/*
 * Swap code regions — shared memory mapped by the controller (rw).
 * These are patched by Microkit via setvar_vaddr in agentos.system.
 * Each region is 4MB; the swap slot PD sees it read-only at 0x2000000.
 *
 * Layout per region:
 *   [0x000] vibe_slot_header_t (64 bytes, written last to commit)
 *   [0x040] WASM binary bytes  (up to ~4MB - 64 bytes)
 */
extern uintptr_t swap_code_ctrl_0;
extern uintptr_t swap_code_ctrl_1;
extern uintptr_t swap_code_ctrl_2;
extern uintptr_t swap_code_ctrl_3;

#define SWAP_CODE_REGION_SIZE   0x400000UL   /* 4MB per slot */
#define SWAP_HEADER_SIZE        64           /* Cache-line aligned */
#define SWAP_MAGIC              0x56494245   /* "VIBE" */

/* Swap slot header written by controller, read by swap_slot PD */
typedef struct __attribute__((packed)) {
    uint32_t    magic;           /* SWAP_MAGIC — set LAST to commit write */
    uint32_t    version;         /* Monotonic version counter */
    uint32_t    code_format;     /* 1 = WASM */
    uint32_t    code_offset;     /* Offset from region base to code bytes */
    uint32_t    code_size;       /* WASM binary size in bytes */
    uint32_t    service_id;      /* Target service ID */
    char        service_name[32];
    uint8_t     _pad[4];         /* Pad to 64 bytes */
} vibe_slot_header_t;

/* Service IDs (index into the service table) */
#define SVC_EVENTBUS    0
#define SVC_MEMFS       1
#define SVC_TOOLSVC     2
#define SVC_MODELSVC    3
#define SVC_AGENTFS     4
#define SVC_LOGSVC      5
#define MAX_SERVICES    8

/* Swap states */
typedef enum {
    SWAP_STATE_IDLE,        /* Slot is empty/available */
    SWAP_STATE_LOADING,     /* New code being loaded into slot */
    SWAP_STATE_TESTING,     /* Running health checks */
    SWAP_STATE_ACTIVE,      /* Slot is serving as the live service */
    SWAP_STATE_ROLLBACK,    /* Previous version kept warm for rollback */
} swap_state_t;

/* Swap slot descriptor */
typedef struct {
    swap_state_t    state;
    uint32_t        channel;        /* Microkit channel to this slot's PD */
    uint32_t        service_id;     /* Which service this slot currently serves */
    uint32_t        version;        /* Implementation version */
    uint64_t        activated_at;   /* When this slot became active */
    uint64_t        health_checks;  /* Number of health checks passed */
    bool            healthy;
} swap_slot_t;

/* Service descriptor */
typedef struct {
    const char     *name;
    uint32_t        active_channel;  /* Current channel serving this service */
    uint32_t        primary_channel; /* Original (static) PD channel */
    uint32_t        version;
    bool            swappable;
    /* Rollback info */
    uint32_t        rollback_slot;   /* Which swap slot has the previous version */
    bool            has_rollback;
} service_desc_t;

/* State */
static swap_slot_t   slots[MAX_SWAP_SLOTS];
static service_desc_t services[MAX_SERVICES];
static int           service_count = 0;
static uint64_t      swap_sequence = 0;

/* Forward declarations */
int vibe_swap_activate(int slot);

/*
 * Initialize the vibe swap subsystem
 */
void vibe_swap_init(void) {
    console_log(7, 7, "[vibe_swap] Initializing swap slot manager\n");
    
    /* Initialize swap slots */
    for (int i = 0; i < MAX_SWAP_SLOTS; i++) {
        slots[i].state = SWAP_STATE_IDLE;
        slots[i].channel = SWAP_SLOT_BASE_CH + i;
        slots[i].service_id = 0;
        slots[i].version = 0;
        slots[i].activated_at = 0;
        slots[i].health_checks = 0;
        slots[i].healthy = false;
    }
    
    /* Register swappable services */
    /* These must match the PD channels in agentos.system */
    services[SVC_EVENTBUS] = (service_desc_t){
        .name = "event_bus",
        .active_channel = 0,  /* CH_EVENTBUS from monitor perspective */
        .primary_channel = 0,
        .version = 1,
        .swappable = false,   /* EventBus is too critical to swap in v0.1 */
        .has_rollback = false,
    };
    
    services[SVC_MEMFS] = (service_desc_t){
        .name = "memfs",
        .active_channel = 2,  /* Placeholder — needs system.xml update */
        .primary_channel = 2,
        .version = 1,
        .swappable = true,
        .has_rollback = false,
    };
    
    services[SVC_TOOLSVC] = (service_desc_t){
        .name = "toolsvc",
        .active_channel = 3,
        .primary_channel = 3,
        .version = 1,
        .swappable = true,
        .has_rollback = false,
    };
    
    services[SVC_MODELSVC] = (service_desc_t){
        .name = "modelsvc",
        .active_channel = 4,
        .primary_channel = 4,
        .version = 1,
        .swappable = true,
        .has_rollback = false,
    };
    
    services[SVC_AGENTFS] = (service_desc_t){
        .name = "agentfs",
        .active_channel = 5,
        .primary_channel = 5,
        .version = 1,
        .swappable = true,
        .has_rollback = false,
    };
    
    service_count = 6;
    
    console_log(7, 7, "[vibe_swap] Swap slots: ");
    char buf[4];
    buf[0] = '0' + MAX_SWAP_SLOTS;
    buf[1] = '\0';
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = buf; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = " available\n"; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = "[vibe_swap] Ready for vibe-coded service proposals\n"; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        console_log(7, 7, _cl_buf);
    }
}

/*
 * Find a free swap slot
 */
static int find_free_slot(void) {
    for (int i = 0; i < MAX_SWAP_SLOTS; i++) {
        if (slots[i].state == SWAP_STATE_IDLE) {
            return i;
        }
    }
    return -1;  /* All slots in use */
}

/*
 * Find the rollback slot for a service
 */
static int find_rollback_slot(uint32_t service_id) {
    for (int i = 0; i < MAX_SWAP_SLOTS; i++) {
        if (slots[i].state == SWAP_STATE_ROLLBACK && 
            slots[i].service_id == service_id) {
            return i;
        }
    }
    return -1;
}

/* Forward declaration: defined below after vibe_swap_begin */
static int vibe_swap_run_tests(int slot, uint32_t service_id);

/*
 * Begin a service swap
 *
 * Called by the controller when the VibeEngine approves a proposal.
 * The flow:
 *   1. Find a free swap slot
 *   2. Load the new code into the slot's PD (via shared memory)
 *   3. Run conformance tests on the new slot (vibe_swap_run_tests)
 *   4. If all tests pass: redirect service channel to the new PD
 *   5. Move old PD to rollback state
 *
 * Returns: slot index (>= 0) on success, negative on error
 */
int vibe_swap_begin(uint32_t service_id, const void *code, uint32_t code_len) {
    if (service_id >= MAX_SERVICES) {
        console_log(7, 7, "[vibe_swap] ERROR: invalid service_id\n");
        return -1;
    }
    
    service_desc_t *svc = &services[service_id];
    
    if (!svc->swappable) {
        {
            char _cl_buf[256] = {};
            char *_cl_p = _cl_buf;
            for (const char *_s = "[vibe_swap] ERROR: service '"; *_s; _s++) *_cl_p++ = *_s;
            for (const char *_s = svc->name; *_s; _s++) *_cl_p++ = *_s;
            for (const char *_s = "' is not swappable\n"; *_s; _s++) *_cl_p++ = *_s;
            *_cl_p = 0;
            console_log(7, 7, _cl_buf);
        }
        return -2;
    }
    
    int slot = find_free_slot();
    if (slot < 0) {
        console_log(7, 7, "[vibe_swap] ERROR: no free swap slots\n");
        return -3;
    }
    
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = "[vibe_swap] Beginning swap for '"; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = svc->name; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = "' into slot "; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        console_log(7, 7, _cl_buf);
    }
    char slot_str[4];
    slot_str[0] = '0' + slot;
    slot_str[1] = '\0';
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = slot_str; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = "\n"; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        console_log(7, 7, _cl_buf);
    }
    
    /* Mark slot as loading */
    slots[slot].state = SWAP_STATE_LOADING;
    slots[slot].service_id = service_id;
    slots[slot].version = svc->version + 1;
    
    /*
     * Load code into the swap slot's PD memory region.
     *
     * In the full implementation:
     *   1. The VibeEngine writes the WASM/ELF binary to a shared memory
     *      region visible to the swap slot PD
     *   2. The controller notifies the swap slot PD to load and initialize
     *   3. The swap slot PD runs its init() and reports back
     *
     * For v0.1: The swap slot PD is a generic "service worker" that
     * interprets a simple bytecode or WASM module from shared memory.
     * This avoids the need to compile native code at runtime.
     */
    
    /* Get controller-side virtual address for this slot's code region */
    static uintptr_t * const slot_regions[MAX_SWAP_SLOTS] = {
        &swap_code_ctrl_0,
        &swap_code_ctrl_1,
        &swap_code_ctrl_2,
        &swap_code_ctrl_3,
    };
    uintptr_t region_base = *slot_regions[slot];

    /* Validate code fits in region */
    if (code_len + SWAP_HEADER_SIZE > SWAP_CODE_REGION_SIZE) {
        console_log(7, 7, "[vibe_swap] ERROR: code too large for slot region\n");
        slots[slot].state = SWAP_STATE_IDLE;
        return -4;
    }

    /* Write header: clear magic first, set it last after data is written */
    volatile vibe_slot_header_t *hdr = (volatile vibe_slot_header_t *)region_base;
    hdr->magic       = 0;
    hdr->version     = slots[slot].version;
    hdr->code_format = 1;  /* WASM */
    hdr->code_offset = SWAP_HEADER_SIZE;
    hdr->code_size   = code_len;
    hdr->service_id  = service_id;

    /* Copy service name */
    const char *sname = svc->name ? svc->name : "unknown";
    int ni = 0;
    while (sname[ni] && ni < 31) { hdr->service_name[ni] = sname[ni]; ni++; }
    hdr->service_name[ni] = '\0';

    /* Copy WASM bytes after header */
    volatile uint8_t *dst = (volatile uint8_t *)(region_base + SWAP_HEADER_SIZE);
    const uint8_t *src = (const uint8_t *)code;
    for (uint32_t i = 0; i < code_len; i++) dst[i] = src[i];

    /* Memory barrier: all writes visible before setting magic */
    agentos_wmb();

    /* Commit: set magic to signal the swap slot that data is ready */
    hdr->magic = SWAP_MAGIC;

    agentos_wmb();

    console_log(7, 7, "[vibe_swap] WASM image written, notifying slot\n");

    /* Notify the swap slot to initialize (loads and runs service_init) */
    microkit_notify(slots[slot].channel);

    /*
     * Run conformance tests against the newly-loaded slot.
     * vibe_swap_run_tests sets state to SWAP_STATE_TESTING internally.
     * If tests fail, the slot is reset to SWAP_STATE_IDLE and we return -5.
     */
    if (vibe_swap_run_tests(slot, service_id) != 0) {
        console_log(7, 7, "[vibe_swap] Conformance tests FAILED — slot not activated\n");
        return -5;
    }

    console_log(7, 7, "[vibe_swap] Conformance tests passed — activating slot\n");

    /*
     * Tests passed: redirect channels and activate.
     * vibe_swap_activate transitions TESTING -> ACTIVE and updates the
     * service routing table.
     */
    if (vibe_swap_activate(slot) != 0) {
        console_log(7, 7, "[vibe_swap] Activation failed after successful tests\n");
        slots[slot].state = SWAP_STATE_IDLE;
        return -6;
    }

    return slot;
}

/* ── ABI label constants (mirrors agentos_service_abi.h) ─────────────── */
#define AOS_LABEL_HEALTH    0xFFFFu  /* Health probe — must reply 0 */
#define STORAGE_OP_WRITE    0x30u    /* storage.v1 write            */
#define STORAGE_OP_READ     0x31u    /* storage.v1 read             */

/*
 * vibe_swap_run_tests — run conformance tests on a freshly-loaded swap slot
 *
 * Sets the slot to SWAP_STATE_TESTING, exercises service-specific IPC
 * operations against the slot's PD channel, and returns 0 if all tests
 * pass or -1 on the first failure.
 *
 * On failure the slot is reset to SWAP_STATE_IDLE so it is available for
 * the next proposal.
 *
 * For SVC_MEMFS (service_id == SVC_MEMFS): three tests are run:
 *   1. Write test  — STORAGE_OP_WRITE with a known key/value pair
 *   2. Read test   — STORAGE_OP_READ for the same key; expects non-NULL ptr
 *   3. Health test — AOS_LABEL_HEALTH; expects MR0 == 0
 *
 * For all other services: only the health check is run.
 */
static int vibe_swap_run_tests(int slot, uint32_t service_id) {
    slots[slot].state = SWAP_STATE_TESTING;

    /* ── Helper: log "Testing slot N: <test_name>... <result>" ───────── */
#define TEST_LOG(tname, result) do { \
        char _cl_buf[256] = {}; \
        char *_cl_p = _cl_buf; \
        for (const char *_s = "[vibe_swap] Testing slot "; *_s; _s++) *_cl_p++ = *_s; \
        *_cl_p++ = '0' + (char)(slot); \
        for (const char *_s = ": "; *_s; _s++) *_cl_p++ = *_s; \
        for (const char *_s = (tname); *_s; _s++) *_cl_p++ = *_s; \
        for (const char *_s = "... "; *_s; _s++) *_cl_p++ = *_s; \
        for (const char *_s = (result); *_s; _s++) *_cl_p++ = *_s; \
        for (const char *_s = "\n"; *_s; _s++) *_cl_p++ = *_s; \
        *_cl_p = 0; \
        console_log(7, 7, _cl_buf); \
    } while (0)

    if (service_id == SVC_MEMFS) {
        /*
         * Test 1: Write — store "testval" under key "test/key".
         * MR0=STORAGE_OP_WRITE MR1=key_ptr MR2=key_len
         * MR3=data_ptr MR4=data_len
         *
         * The test key and value live in static storage so the pointer
         * values are stable across the PPC.  The swap slot PD interprets
         * them as offsets into its own WASM linear memory via the wasm3
         * runtime, but for conformance testing we pass literal addresses
         * that the stub implementation can resolve.  A zero return in MR0
         * signals success.
         */
        static const char test_key[] = "test/key";
        static const char test_val[] = "testval\0";

        microkit_mr_set(0, STORAGE_OP_WRITE);
        microkit_mr_set(1, (seL4_Word)(uintptr_t)test_key);
        microkit_mr_set(2, 8);  /* length of "test/key" */
        microkit_mr_set(3, (seL4_Word)(uintptr_t)test_val);
        microkit_mr_set(4, 7);  /* length of "testval" */

        microkit_msginfo write_result = PPCALL_DONATE(
            slots[slot].channel,
            microkit_msginfo_new(STORAGE_OP_WRITE, 5),
            PRIO_CONTROLLER, PRIO_SWAP_SLOT
        );
        (void)write_result;

        uint32_t write_status = (uint32_t)microkit_mr_get(0);
        if (write_status != 0) {
            TEST_LOG("write", "FAIL");
            slots[slot].state = SWAP_STATE_IDLE;
            return -1;
        }
        TEST_LOG("write", "PASS");

        /*
         * Test 2: Read — retrieve the key written above.
         * Expect MR0 (data_ptr) to be non-zero.
         */
        microkit_mr_set(0, STORAGE_OP_READ);
        microkit_mr_set(1, (seL4_Word)(uintptr_t)test_key);
        microkit_mr_set(2, 8);

        microkit_msginfo read_result = PPCALL_DONATE(
            slots[slot].channel,
            microkit_msginfo_new(STORAGE_OP_READ, 3),
            PRIO_CONTROLLER, PRIO_SWAP_SLOT
        );
        (void)read_result;

        uint32_t read_ptr = (uint32_t)microkit_mr_get(0);
        if (read_ptr == 0) {
            TEST_LOG("read", "FAIL");
            slots[slot].state = SWAP_STATE_IDLE;
            return -1;
        }
        TEST_LOG("read", "PASS");
    }

    /*
     * Health test (all services): send AOS_LABEL_HEALTH, expect MR0 == 0.
     */
    microkit_mr_set(0, 0);  /* no input MRs needed */

    microkit_msginfo health_result = PPCALL_DONATE(
        slots[slot].channel,
        microkit_msginfo_new(AOS_LABEL_HEALTH, 0),
        PRIO_CONTROLLER, PRIO_SWAP_SLOT
    );
    (void)health_result;

    uint32_t health_status = (uint32_t)microkit_mr_get(0);
    if (health_status != 0) {
        TEST_LOG("health", "FAIL");
        slots[slot].state = SWAP_STATE_IDLE;
        return -1;
    }
    TEST_LOG("health", "PASS");

#undef TEST_LOG

    return 0;
}

/*
 * Activate a swap slot as the live service
 *
 * Called after health checks pass.
 * This is the atomic moment: one channel redirect.
 */
int vibe_swap_activate(int slot) {
    if (slot < 0 || slot >= MAX_SWAP_SLOTS) return -1;
    if (slots[slot].state != SWAP_STATE_TESTING) return -2;
    
    uint32_t svc_id = slots[slot].service_id;
    service_desc_t *svc = &services[svc_id];
    
    console_log(7, 7, "[vibe_swap] Activating slot ");
    char buf[4];
    buf[0] = '0' + slot;
    buf[1] = '\0';
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = buf; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = " for '"; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = svc->name; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = "'\n"; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        console_log(7, 7, _cl_buf);
    }
    
    /*
     * The swap operation:
     *
     * 1. Save current active channel as rollback
     * 2. Redirect: agents calling this service now talk to the swap slot
     * 3. Mark old PD as rollback
     *
     * Channel redirection in Microkit:
     *   Microkit channels are set up at boot by the system description.
     *   True dynamic channel redirection isn't supported.
     *
     *   Our workaround: The controller acts as a PROXY.
     *   - Agents PPC to the controller for service requests
     *   - Controller forwards to whichever PD is currently active
     *   - Swap = controller changes its internal routing table
     *
     *   This adds one IPC hop but keeps us in the verified TCB.
     *   In a future Microkit version with dynamic channel management,
     *   we could eliminate the proxy hop.
     */
    
    /* Move current active to rollback */
    int old_rollback = find_rollback_slot(svc_id);
    if (old_rollback >= 0) {
        /* Free the old rollback slot */
        slots[old_rollback].state = SWAP_STATE_IDLE;
    }
    
    /* If current service is in a swap slot, move it to rollback */
    for (int i = 0; i < MAX_SWAP_SLOTS; i++) {
        if (slots[i].state == SWAP_STATE_ACTIVE && slots[i].service_id == svc_id) {
            slots[i].state = SWAP_STATE_ROLLBACK;
            svc->rollback_slot = i;
            svc->has_rollback = true;
            break;
        }
    }
    
    /* Activate the new slot */
    slots[slot].state = SWAP_STATE_ACTIVE;
    slots[slot].activated_at = swap_sequence++;
    slots[slot].health_checks = 0;
    slots[slot].healthy = true;
    
    /* Update the service's active channel to point to the swap slot */
    svc->active_channel = slots[slot].channel;
    svc->version = slots[slot].version;
    
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = "[vibe_swap] *** SERVICE SWAPPED: '"; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = svc->name; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = "' now at version "; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        console_log(7, 7, _cl_buf);
    }
    char ver[4];
    ver[0] = '0' + (svc->version % 10);
    ver[1] = '\0';
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = ver; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = " ***\n"; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        console_log(7, 7, _cl_buf);
    }
    
    return 0;
}

/*
 * Rollback a service to its previous version
 */
int vibe_swap_rollback(uint32_t service_id) {
    if (service_id >= MAX_SERVICES) return -1;

    /*
     * Race guard: if any slot for this service is still in the TESTING
     * state, it has never been activated.  Roll it back to IDLE so the
     * slot is available for the next proposal — no channel redirect is
     * needed because the service's active channel was never changed.
     */
    for (int i = 0; i < MAX_SWAP_SLOTS; i++) {
        if (slots[i].state == SWAP_STATE_TESTING &&
            slots[i].service_id == service_id) {
            console_log(7, 7, "[vibe_swap] rollback: slot still in TESTING — reset to IDLE\n");
            slots[i].state = SWAP_STATE_IDLE;
            return 0;  /* No channel redirect required; nothing was activated */
        }
    }

    service_desc_t *svc = &services[service_id];

    if (!svc->has_rollback) {
        {
            char _cl_buf[256] = {};
            char *_cl_p = _cl_buf;
            for (const char *_s = "[vibe_swap] ERROR: no rollback available for '"; *_s; _s++) *_cl_p++ = *_s;
            for (const char *_s = svc->name; *_s; _s++) *_cl_p++ = *_s;
            for (const char *_s = "'\n"; *_s; _s++) *_cl_p++ = *_s;
            *_cl_p = 0;
            console_log(7, 7, _cl_buf);
        }
        return -2;
    }
    
    int rb_slot = svc->rollback_slot;
    
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = "[vibe_swap] Rolling back '"; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = svc->name; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = "' to previous version\n"; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        console_log(7, 7, _cl_buf);
    }
    
    /* Deactivate current slot */
    for (int i = 0; i < MAX_SWAP_SLOTS; i++) {
        if (slots[i].state == SWAP_STATE_ACTIVE && slots[i].service_id == service_id) {
            slots[i].state = SWAP_STATE_IDLE;
            break;
        }
    }
    
    /* Reactivate rollback slot */
    slots[rb_slot].state = SWAP_STATE_ACTIVE;
    svc->active_channel = slots[rb_slot].channel;
    svc->version = slots[rb_slot].version;
    svc->has_rollback = false;
    
    console_log(7, 7, "[vibe_swap] Rollback complete\n");
    return 0;
}

/*
 * Health check a swap slot
 * Returns: true if healthy
 */
bool vibe_swap_health_check(int slot) {
    if (slot < 0 || slot >= MAX_SWAP_SLOTS) return false;
    if (slots[slot].state != SWAP_STATE_ACTIVE) return false;
    
    /* PPC to the swap slot's PD with a health check request */
    microkit_mr_set(0, 0x4E41);  /* Health check magic ("NA" in hex) */
    microkit_msginfo result = PPCALL_DONATE(
        slots[slot].channel,
        microkit_msginfo_new(0x0004, 1),  /* MSG_EVENTBUS_STATUS equivalent */
        PRIO_CONTROLLER, PRIO_SWAP_SLOT
    );
    
    uint64_t resp = microkit_msginfo_get_label(result);
    bool healthy = (resp != 0xDEAD);
    
    slots[slot].healthy = healthy;
    if (healthy) {
        slots[slot].health_checks++;
    }
    
    return healthy;
}

/*
 * Get swap slot status (for VibeEngine queries)
 */
void vibe_swap_status(uint32_t service_id, uint32_t *version,
                       uint32_t *active_channel, bool *has_rollback) {
    if (service_id >= MAX_SERVICES) return;
    
    service_desc_t *svc = &services[service_id];
    if (version) *version = svc->version;
    if (active_channel) *active_channel = svc->active_channel;
    if (has_rollback) *has_rollback = svc->has_rollback;
}

/*
 * Called by the controller when a swap slot sends a health-OK notification.
 * Transitions slot from TESTING → ACTIVE and redirects the service.
 */
int vibe_swap_health_notify(int slot) {
    if (slot < 0 || slot >= MAX_SWAP_SLOTS) return -1;
    if (slots[slot].state != SWAP_STATE_TESTING) {
        console_log(7, 7, "[vibe_swap] health_notify: slot not in TESTING state\n");
        return -2;
    }
    slots[slot].healthy = true;
    return vibe_swap_activate(slot);
}
