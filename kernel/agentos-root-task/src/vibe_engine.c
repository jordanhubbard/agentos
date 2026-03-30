/*
 * agentOS VibeEngine Protection Domain
 *
 * The VibeEngine is the userspace service that manages the hot-swap
 * lifecycle: agents submit WASM proposals, VibeEngine validates them,
 * and triggers the kernel-side vibe_swap pipeline via the controller.
 *
 * This closes the end-to-end loop:
 *   Agent → VibeEngine (propose/validate/execute) → Controller →
 *   vibe_swap_begin → swap_slot loads WASM via wasm3 → health check →
 *   service live
 *
 * Passive PD at priority 140. Clients PPC in with typed requests.
 *
 * Channel assignments:
 *   CH_AGENT    = 0  (agents PPC in with proposals)
 *   CH_CTRL     = 1  (notify controller when swap is approved)
 *
 * Shared memory:
 *   vibe_staging (4MB): WASM binary staging area
 *     - Agent writes WASM bytes here before calling OP_VIBE_PROPOSE
 *     - VibeEngine reads + validates from here
 *     - Controller reads from here for vibe_swap_begin
 *
 * IPC operations (MR0 = op code):
 *   OP_VIBE_PROPOSE  = 0x40 — submit WASM for a target service
 *   OP_VIBE_VALIDATE = 0x41 — run validation on a proposal
 *   OP_VIBE_EXECUTE  = 0x42 — approve + trigger swap via controller
 *   OP_VIBE_STATUS   = 0x43 — query proposal or engine status
 *   OP_VIBE_ROLLBACK = 0x44 — request rollback of a service
 *   OP_VIBE_HEALTH   = 0x45 — health check for VibeEngine itself
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include <stdint.h>
#include <stdbool.h>

/* ── Channel IDs (must match agentos.system) ────────────────────────── */
#define CH_AGENT   0   /* Agents PPC in here */
#define CH_CTRL    1   /* Notify controller for swap execution */

/* ── Op codes ───────────────────────────────────────────────────────── */
#define OP_VIBE_PROPOSE   0x40
#define OP_VIBE_VALIDATE  0x41
#define OP_VIBE_EXECUTE   0x42
#define OP_VIBE_STATUS    0x43
#define OP_VIBE_ROLLBACK  0x44
#define OP_VIBE_HEALTH    0x45

/* ── Result codes ───────────────────────────────────────────────────── */
#define VIBE_OK             0
#define VIBE_ERR_FULL       1   /* proposal table full */
#define VIBE_ERR_BADWASM    2   /* invalid WASM header */
#define VIBE_ERR_TOOBIG     3   /* WASM binary too large */
#define VIBE_ERR_NOSVC      4   /* service not found or not swappable */
#define VIBE_ERR_NOENT      5   /* proposal not found */
#define VIBE_ERR_STATE      6   /* proposal in wrong state */
#define VIBE_ERR_VALFAIL    7   /* validation failed */
#define VIBE_ERR_INTERNAL   99

/* ── WASM validation ────────────────────────────────────────────────── */
#define WASM_MAGIC_0  0x00
#define WASM_MAGIC_1  0x61   /* 'a' */
#define WASM_MAGIC_2  0x73   /* 's' */
#define WASM_MAGIC_3  0x6D   /* 'm' */

/* ── Staging region ─────────────────────────────────────────────────── */
/* Microkit setvar_vaddr: patched to the mapped virtual address */
uintptr_t vibe_staging_vaddr;

#define STAGING_SIZE      0x400000UL  /* 4MB staging region */
#define MAX_WASM_SIZE     (STAGING_SIZE - 64)  /* leave room for metadata */

/* ── Proposal management ────────────────────────────────────────────── */
#define MAX_PROPOSALS     8
#define MAX_SERVICES      8

typedef enum {
    PROP_STATE_FREE,       /* Slot available */
    PROP_STATE_PENDING,    /* Submitted, awaiting validation */
    PROP_STATE_VALIDATED,  /* Passed validation */
    PROP_STATE_APPROVED,   /* Approved, ready for swap */
    PROP_STATE_ACTIVE,     /* Swap executed, service live */
    PROP_STATE_REJECTED,   /* Validation or execution failed */
    PROP_STATE_ROLLEDBACK, /* Was active, then rolled back */
} proposal_state_t;

typedef struct {
    proposal_state_t state;
    uint32_t         service_id;
    uint32_t         wasm_offset;  /* Offset into staging region */
    uint32_t         wasm_size;
    uint32_t         cap_tag;      /* Proposer's capability badge */
    uint32_t         version;      /* Proposal version (monotonic) */
    /* Validation results (bitmap) */
    uint32_t         val_checks;   /* bits: 0=magic, 1=size, 2=svc, 3=cap */
    bool             val_passed;
} proposal_t;

typedef struct {
    const char *name;
    bool        swappable;
    uint32_t    current_version;
    uint32_t    max_wasm_bytes;
} service_entry_t;

/* ── State ──────────────────────────────────────────────────────────── */
static proposal_t     proposals[MAX_PROPOSALS];
static service_entry_t services[MAX_SERVICES];
static uint32_t       service_count = 0;
static uint32_t       next_proposal_id = 1;  /* 0 = invalid */
static uint64_t       total_proposals = 0;
static uint64_t       total_swaps = 0;
static uint64_t       total_rejections = 0;

/* ── Service registry ───────────────────────────────────────────────── */
static void register_services(void) {
    services[0] = (service_entry_t){
        .name = "event_bus", .swappable = false,
        .current_version = 1, .max_wasm_bytes = 0,
    };
    services[1] = (service_entry_t){
        .name = "memfs", .swappable = true,
        .current_version = 1, .max_wasm_bytes = 2 * 1024 * 1024,
    };
    services[2] = (service_entry_t){
        .name = "toolsvc", .swappable = true,
        .current_version = 1, .max_wasm_bytes = 2 * 1024 * 1024,
    };
    services[3] = (service_entry_t){
        .name = "modelsvc", .swappable = true,
        .current_version = 1, .max_wasm_bytes = 4 * 1024 * 1024,
    };
    services[4] = (service_entry_t){
        .name = "agentfs", .swappable = true,
        .current_version = 1, .max_wasm_bytes = 2 * 1024 * 1024,
    };
    services[5] = (service_entry_t){
        .name = "logsvc", .swappable = true,
        .current_version = 1, .max_wasm_bytes = 1 * 1024 * 1024,
    };
    service_count = 6;
}

/* ── Helpers ────────────────────────────────────────────────────────── */

static int find_free_proposal(void) {
    for (int i = 0; i < MAX_PROPOSALS; i++) {
        if (proposals[i].state == PROP_STATE_FREE) return i;
    }
    return -1;
}

static bool validate_wasm_header(const uint8_t *data, uint32_t size) {
    if (size < 8) return false;
    return (data[0] == WASM_MAGIC_0 &&
            data[1] == WASM_MAGIC_1 &&
            data[2] == WASM_MAGIC_2 &&
            data[3] == WASM_MAGIC_3);
}

/* ── IPC Handlers ───────────────────────────────────────────────────── */

/*
 * OP_VIBE_PROPOSE: Agent submits a WASM binary for hot-swap
 *
 * Input:  MR0=op, MR1=service_id, MR2=wasm_size, MR3=cap_tag
 *         WASM binary must be pre-written to the staging region at offset 0
 *
 * Output: MR0=status, MR1=proposal_id (on success)
 */
static microkit_msginfo handle_propose(void) {
    uint32_t service_id = (uint32_t)microkit_mr_get(1);
    uint32_t wasm_size  = (uint32_t)microkit_mr_get(2);
    uint32_t cap_tag    = (uint32_t)microkit_mr_get(3);

    microkit_dbg_puts("[vibe_engine] Proposal received: service=");
    if (service_id < service_count && services[service_id].name) {
        microkit_dbg_puts(services[service_id].name);
    } else {
        microkit_dbg_puts("?");
    }
    microkit_dbg_puts(", wasm_size=");
    /* Print size as rough decimal */
    char sz_buf[8];
    uint32_t s = wasm_size;
    int pos = 0;
    if (s == 0) { sz_buf[pos++] = '0'; }
    else {
        char tmp[8]; int t = 0;
        while (s > 0 && t < 7) { tmp[t++] = '0' + (s % 10); s /= 10; }
        while (t > 0 && pos < 7) sz_buf[pos++] = tmp[--t];
    }
    sz_buf[pos] = '\0';
    microkit_dbg_puts(sz_buf);
    microkit_dbg_puts(" bytes\n");

    /* Check service exists and is swappable */
    if (service_id >= service_count) {
        microkit_dbg_puts("[vibe_engine] REJECT: unknown service\n");
        microkit_mr_set(0, VIBE_ERR_NOSVC);
        return microkit_msginfo_new(0, 1);
    }
    if (!services[service_id].swappable) {
        microkit_dbg_puts("[vibe_engine] REJECT: service not swappable\n");
        microkit_mr_set(0, VIBE_ERR_NOSVC);
        return microkit_msginfo_new(0, 1);
    }

    /* Check WASM size */
    if (wasm_size > MAX_WASM_SIZE || wasm_size > services[service_id].max_wasm_bytes) {
        microkit_dbg_puts("[vibe_engine] REJECT: WASM too large\n");
        microkit_mr_set(0, VIBE_ERR_TOOBIG);
        return microkit_msginfo_new(0, 1);
    }

    /* Validate WASM magic header from staging region */
    const uint8_t *staged = (const uint8_t *)vibe_staging_vaddr;
    if (!validate_wasm_header(staged, wasm_size)) {
        microkit_dbg_puts("[vibe_engine] REJECT: bad WASM magic\n");
        microkit_mr_set(0, VIBE_ERR_BADWASM);
        return microkit_msginfo_new(0, 1);
    }

    /* Find a free proposal slot */
    int slot = find_free_proposal();
    if (slot < 0) {
        microkit_dbg_puts("[vibe_engine] REJECT: proposal table full\n");
        microkit_mr_set(0, VIBE_ERR_FULL);
        return microkit_msginfo_new(0, 1);
    }

    /* Record the proposal */
    proposals[slot].state       = PROP_STATE_PENDING;
    proposals[slot].service_id  = service_id;
    proposals[slot].wasm_offset = 0;  /* Always at start of staging for now */
    proposals[slot].wasm_size   = wasm_size;
    proposals[slot].cap_tag     = cap_tag;
    proposals[slot].version     = next_proposal_id++;
    proposals[slot].val_checks  = 0;
    proposals[slot].val_passed  = false;
    total_proposals++;

    microkit_dbg_puts("[vibe_engine] Proposal accepted: id=");
    char id_buf[4];
    id_buf[0] = '0' + (proposals[slot].version % 10);
    id_buf[1] = '\0';
    microkit_dbg_puts(id_buf);
    microkit_dbg_puts("\n");

    microkit_mr_set(0, VIBE_OK);
    microkit_mr_set(1, proposals[slot].version);  /* proposal_id */
    return microkit_msginfo_new(0, 2);
}

/*
 * OP_VIBE_VALIDATE: Run validation checks on a proposal
 *
 * Input:  MR0=op, MR1=proposal_id
 * Output: MR0=status, MR1=check_bitmap (on success)
 *
 * Check bitmap: bit 0 = WASM magic, bit 1 = size, bit 2 = service, bit 3 = cap
 */
static microkit_msginfo handle_validate(void) {
    uint32_t proposal_id = (uint32_t)microkit_mr_get(1);

    /* Find proposal by version/id */
    int slot = -1;
    for (int i = 0; i < MAX_PROPOSALS; i++) {
        if (proposals[i].state != PROP_STATE_FREE &&
            proposals[i].version == proposal_id) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        microkit_mr_set(0, VIBE_ERR_NOENT);
        return microkit_msginfo_new(0, 1);
    }
    if (proposals[slot].state != PROP_STATE_PENDING) {
        microkit_mr_set(0, VIBE_ERR_STATE);
        return microkit_msginfo_new(0, 1);
    }

    microkit_dbg_puts("[vibe_engine] Validating proposal ");
    char id_buf[4];
    id_buf[0] = '0' + (proposal_id % 10);
    id_buf[1] = '\0';
    microkit_dbg_puts(id_buf);
    microkit_dbg_puts("...\n");

    uint32_t checks = 0;
    bool all_pass = true;

    /* Check 0: WASM magic header */
    const uint8_t *staged = (const uint8_t *)(vibe_staging_vaddr + proposals[slot].wasm_offset);
    if (validate_wasm_header(staged, proposals[slot].wasm_size)) {
        checks |= (1 << 0);
        microkit_dbg_puts("[vibe_engine]   ✓ WASM magic valid\n");
    } else {
        all_pass = false;
        microkit_dbg_puts("[vibe_engine]   ✗ WASM magic INVALID\n");
    }

    /* Check 1: Size within service limits */
    uint32_t svc_id = proposals[slot].service_id;
    if (proposals[slot].wasm_size <= services[svc_id].max_wasm_bytes) {
        checks |= (1 << 1);
        microkit_dbg_puts("[vibe_engine]   ✓ Size within limits\n");
    } else {
        all_pass = false;
        microkit_dbg_puts("[vibe_engine]   ✗ Size exceeds limit\n");
    }

    /* Check 2: Service is swappable */
    if (services[svc_id].swappable) {
        checks |= (1 << 2);
        microkit_dbg_puts("[vibe_engine]   ✓ Service is swappable\n");
    } else {
        all_pass = false;
        microkit_dbg_puts("[vibe_engine]   ✗ Service NOT swappable\n");
    }

    /* Check 3: Capability tag is non-zero (basic auth) */
    if (proposals[slot].cap_tag != 0) {
        checks |= (1 << 3);
        microkit_dbg_puts("[vibe_engine]   ✓ Capability tag present\n");
    } else {
        all_pass = false;
        microkit_dbg_puts("[vibe_engine]   ✗ No capability tag\n");
    }

    proposals[slot].val_checks = checks;
    proposals[slot].val_passed = all_pass;

    if (all_pass) {
        proposals[slot].state = PROP_STATE_VALIDATED;
        microkit_dbg_puts("[vibe_engine] Validation PASSED (all 4 checks)\n");
    } else {
        proposals[slot].state = PROP_STATE_REJECTED;
        total_rejections++;
        microkit_dbg_puts("[vibe_engine] Validation FAILED\n");
    }

    microkit_mr_set(0, all_pass ? VIBE_OK : VIBE_ERR_VALFAIL);
    microkit_mr_set(1, checks);
    return microkit_msginfo_new(0, 2);
}

/*
 * OP_VIBE_EXECUTE: Execute the swap — trigger controller pipeline
 *
 * Input:  MR0=op, MR1=proposal_id
 * Output: MR0=status
 *
 * On success: VibeEngine notifies the controller (CH_CTRL) with:
 *   MR0 = service_id
 *   MR1 = wasm_size
 *   MR2 = wasm_offset (into shared staging region)
 *
 * The controller then calls vibe_swap_begin to load WASM into a swap slot.
 */
static microkit_msginfo handle_execute(void) {
    uint32_t proposal_id = (uint32_t)microkit_mr_get(1);

    int slot = -1;
    for (int i = 0; i < MAX_PROPOSALS; i++) {
        if (proposals[i].state != PROP_STATE_FREE &&
            proposals[i].version == proposal_id) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        microkit_mr_set(0, VIBE_ERR_NOENT);
        return microkit_msginfo_new(0, 1);
    }
    if (proposals[slot].state != PROP_STATE_VALIDATED) {
        microkit_mr_set(0, VIBE_ERR_STATE);
        return microkit_msginfo_new(0, 1);
    }

    microkit_dbg_puts("[vibe_engine] Executing swap for proposal ");
    char id_buf[4];
    id_buf[0] = '0' + (proposal_id % 10);
    id_buf[1] = '\0';
    microkit_dbg_puts(id_buf);
    microkit_dbg_puts(": service='");
    microkit_dbg_puts(services[proposals[slot].service_id].name);
    microkit_dbg_puts("'\n");

    proposals[slot].state = PROP_STATE_APPROVED;

    /*
     * Set MRs for the controller to read when it handles our notification.
     * NOTE: In Microkit, notification is async — the controller won't see
     * MRs set before microkit_notify. The controller will need to PPC back
     * into us to get the proposal details, OR we use shared memory.
     *
     * We use the staging region header for handoff:
     *   staging[0..3]   = service_id (LE)
     *   staging[4..7]   = wasm_offset (LE, into this same region)
     *   staging[8..11]  = wasm_size (LE)
     *   staging[12..15] = proposal_id (LE)
     *
     * But the WASM binary is also in staging starting at wasm_offset.
     * So we write the metadata at the END of the staging region
     * (last 64 bytes) to avoid clobbering the WASM.
     */
    volatile uint8_t *meta = (volatile uint8_t *)(vibe_staging_vaddr + STAGING_SIZE - 64);
    uint32_t svc = proposals[slot].service_id;
    uint32_t off = proposals[slot].wasm_offset;
    uint32_t sz  = proposals[slot].wasm_size;
    uint32_t pid = proposals[slot].version;

    /* Write LE uint32s */
    meta[0]  = svc & 0xff; meta[1]  = (svc >> 8) & 0xff;
    meta[2]  = (svc >> 16) & 0xff; meta[3]  = (svc >> 24) & 0xff;
    meta[4]  = off & 0xff; meta[5]  = (off >> 8) & 0xff;
    meta[6]  = (off >> 16) & 0xff; meta[7]  = (off >> 24) & 0xff;
    meta[8]  = sz & 0xff;  meta[9]  = (sz >> 8) & 0xff;
    meta[10] = (sz >> 16) & 0xff;  meta[11] = (sz >> 24) & 0xff;
    meta[12] = pid & 0xff; meta[13] = (pid >> 8) & 0xff;
    meta[14] = (pid >> 16) & 0xff; meta[15] = (pid >> 24) & 0xff;

    /* Memory barrier: metadata visible before notification */
#if defined(__riscv)
    __asm__ volatile ("fence w,w" ::: "memory");
#elif defined(__aarch64__)
    __asm__ volatile ("dmb ishst" ::: "memory");
#else
    __asm__ volatile ("" ::: "memory");
#endif

    microkit_dbg_puts("[vibe_engine] *** SWAP APPROVED — notifying controller ***\n");

    /* Notify the controller to pick up the swap request */
    microkit_notify(CH_CTRL);

    total_swaps++;
    proposals[slot].state = PROP_STATE_ACTIVE;

    /* Update service version */
    services[proposals[slot].service_id].current_version++;

    microkit_mr_set(0, VIBE_OK);
    return microkit_msginfo_new(0, 1);
}

/*
 * OP_VIBE_STATUS: Query VibeEngine status
 *
 * Input:  MR0=op, MR1=proposal_id (0 for engine-wide stats)
 * Output: MR0=status, MR1=total_proposals, MR2=total_swaps,
 *         MR3=total_rejections, MR4=proposal_state (if queried)
 */
static microkit_msginfo handle_status(void) {
    uint32_t proposal_id = (uint32_t)microkit_mr_get(1);

    microkit_mr_set(0, VIBE_OK);
    microkit_mr_set(1, (uint32_t)total_proposals);
    microkit_mr_set(2, (uint32_t)total_swaps);
    microkit_mr_set(3, (uint32_t)total_rejections);

    if (proposal_id > 0) {
        for (int i = 0; i < MAX_PROPOSALS; i++) {
            if (proposals[i].version == proposal_id) {
                microkit_mr_set(4, (uint32_t)proposals[i].state);
                return microkit_msginfo_new(0, 5);
            }
        }
        microkit_mr_set(4, 0xFFFFFFFF);  /* Not found */
    }

    return microkit_msginfo_new(0, 4);
}

/*
 * OP_VIBE_ROLLBACK: Request rollback for a service
 *
 * Input:  MR0=op, MR1=service_id
 * Output: MR0=status
 *
 * Notifies controller with a rollback request.
 */
static microkit_msginfo handle_rollback(void) {
    uint32_t service_id = (uint32_t)microkit_mr_get(1);

    if (service_id >= service_count || !services[service_id].swappable) {
        microkit_mr_set(0, VIBE_ERR_NOSVC);
        return microkit_msginfo_new(0, 1);
    }

    microkit_dbg_puts("[vibe_engine] Rollback requested for '");
    microkit_dbg_puts(services[service_id].name);
    microkit_dbg_puts("'\n");

    /* Write rollback command to staging metadata
     * service_id at offset, 0xFFFFFFFF for wasm_size = rollback signal */
    volatile uint8_t *meta = (volatile uint8_t *)(vibe_staging_vaddr + STAGING_SIZE - 64);
    uint32_t svc = service_id;
    meta[0]  = svc & 0xff; meta[1]  = (svc >> 8) & 0xff;
    meta[2]  = (svc >> 16) & 0xff; meta[3]  = (svc >> 24) & 0xff;
    meta[4]  = 0; meta[5] = 0; meta[6] = 0; meta[7] = 0;
    /* wasm_size = 0xFFFFFFFF means rollback */
    meta[8]  = 0xFF; meta[9]  = 0xFF; meta[10] = 0xFF; meta[11] = 0xFF;

#if defined(__riscv)
    __asm__ volatile ("fence w,w" ::: "memory");
#elif defined(__aarch64__)
    __asm__ volatile ("dmb ishst" ::: "memory");
#else
    __asm__ volatile ("" ::: "memory");
#endif

    microkit_notify(CH_CTRL);

    /* Mark proposal as rolled back */
    for (int i = 0; i < MAX_PROPOSALS; i++) {
        if (proposals[i].state == PROP_STATE_ACTIVE &&
            proposals[i].service_id == service_id) {
            proposals[i].state = PROP_STATE_ROLLEDBACK;
            break;
        }
    }

    microkit_mr_set(0, VIBE_OK);
    return microkit_msginfo_new(0, 1);
}

/*
 * OP_VIBE_HEALTH: Health check
 */
static microkit_msginfo handle_health(void) {
    microkit_mr_set(0, VIBE_OK);
    microkit_mr_set(1, (uint32_t)total_proposals);
    microkit_mr_set(2, (uint32_t)total_swaps);
    return microkit_msginfo_new(0, 3);
}

/* ── Microkit entry points ──────────────────────────────────────────── */

void init(void) {
    microkit_dbg_puts("[vibe_engine] VibeEngine PD starting...\n");

    /* Initialize proposal table */
    for (int i = 0; i < MAX_PROPOSALS; i++) {
        proposals[i].state = PROP_STATE_FREE;
    }

    /* Register known services */
    register_services();

    microkit_dbg_puts("[vibe_engine] Services registered: ");
    char cnt[4];
    cnt[0] = '0' + service_count;
    cnt[1] = '\0';
    microkit_dbg_puts(cnt);
    microkit_dbg_puts(" (");
    int swappable = 0;
    for (uint32_t i = 0; i < service_count; i++) {
        if (services[i].swappable) swappable++;
    }
    char sw[4];
    sw[0] = '0' + swappable;
    sw[1] = '\0';
    microkit_dbg_puts(sw);
    microkit_dbg_puts(" swappable)\n");

    microkit_dbg_puts("[vibe_engine] Proposal table: ");
    char mx[4];
    mx[0] = '0' + MAX_PROPOSALS;
    mx[1] = '\0';
    microkit_dbg_puts(mx);
    microkit_dbg_puts(" slots\n");

    microkit_dbg_puts("[vibe_engine] Staging region: 4MB at 0x");
    /* Print vaddr as hex */
    uintptr_t va = vibe_staging_vaddr;
    char hex[20];
    int hi = 0;
    for (int shift = 28; shift >= 0; shift -= 4) {
        uint8_t nibble = (va >> shift) & 0xF;
        hex[hi++] = nibble < 10 ? ('0' + nibble) : ('a' + nibble - 10);
    }
    hex[hi] = '\0';
    microkit_dbg_puts(hex);
    microkit_dbg_puts("\n");

    microkit_dbg_puts("[vibe_engine] *** VibeEngine ALIVE — accepting proposals ***\n");
}

/* Passive PD — only woken by PPC or notification */
void notified(microkit_channel ch) {
    if (ch == CH_CTRL) {
        microkit_dbg_puts("[vibe_engine] Controller ack received\n");
    }
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msg) {
    (void)ch;   /* Accept PPC from any connected channel */
    (void)msg;  /* Op code is in MR0, not the label */
    uint32_t op = (uint32_t)microkit_mr_get(0);

    switch (op) {
        case OP_VIBE_PROPOSE:   return handle_propose();
        case OP_VIBE_VALIDATE:  return handle_validate();
        case OP_VIBE_EXECUTE:   return handle_execute();
        case OP_VIBE_STATUS:    return handle_status();
        case OP_VIBE_ROLLBACK:  return handle_rollback();
        case OP_VIBE_HEALTH:    return handle_health();
        default:
            microkit_dbg_puts("[vibe_engine] Unknown op\n");
            microkit_mr_set(0, VIBE_ERR_INTERNAL);
            return microkit_msginfo_new(0, 1);
    }
}
