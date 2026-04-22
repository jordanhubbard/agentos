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
#include "barrier.h"
#include "cap_policy.h"
#include "contracts/vibe_engine_contract.h"
#include "contracts/vibeos_contract.h"
#include <stdint.h>
#include <stdbool.h>

/* Forward declaration */
static bool validate_wasm_header(const uint8_t *data, uint32_t size);

/* ── Channel IDs (must match agentos.system) ────────────────────────── */
#define CH_AGENT   0   /* Agents PPC in here */
#define CH_CTRL    1   /* Notify controller for swap execution */
#define CH_VIBEOS  2   /* VibeOS lifecycle management (controller/callers) */

/* ── Op codes ───────────────────────────────────────────────────────── */
#define OP_VIBE_PROPOSE           0x40
#define OP_VIBE_VALIDATE          0x41
#define OP_VIBE_EXECUTE           0x42
#define OP_VIBE_STATUS            0x43
#define OP_VIBE_ROLLBACK          0x44
#define OP_VIBE_HEALTH            0x45
#define OP_VIBE_REGISTER_SERVICE  0x46   /* Register a new swappable service */
#define OP_VIBE_LIST_SERVICES     0x47   /* List all registered services */

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

/* ── VOS instance table (forward declarations) ───────────────────────
 * Full definition lives below the VM manager section; these forward
 * declarations allow handlers earlier in the file to reference them. */
#define MAX_VOS_INSTANCES  4
typedef struct {
    bool     active;
    uint32_t handle;
    uint32_t vm_slot;
    uint8_t  os_type;
    uint8_t  state;
    uint32_t ram_mb;
    uint32_t dev_mask;
    uint32_t dev_handles[5];
    uint8_t  module_hash[32];
    uint32_t swap_id;
} vos_instance_t;
static vos_instance_t s_vos[MAX_VOS_INSTANCES];
static uint32_t       s_next_handle = 1;
static int vos_find(uint32_t handle);

/*
 * MSG_VIBEOS_LOAD_MODULE: Install a WASM or ELF component into a VibeOS
 * context via the vibe_swap.c hot-swap pipeline.
 *
 * The caller pre-writes the module binary into the staging region and passes
 * its size and SHA-256 hash.  vibe_engine writes the vibe_slot_header and
 * notifies the controller, which drives the swap slot load.
 *
 * Input:  MR0=opcode MR1=handle MR2=module_type MR3=module_size
 *         vibeos_load_module_req (incl. module_hash) in staging region header
 * Output: MR0=ok MR1=swap_id
 */
static microkit_msginfo handle_vibeos_load_module(void)
{
    uint32_t handle      = (uint32_t)microkit_mr_get(1);
    uint32_t module_type = (uint32_t)microkit_mr_get(2);
    uint32_t module_size = (uint32_t)microkit_mr_get(3);

    int vslot = vos_find(handle);
    if (vslot < 0) {
        log_drain_write(7, 7, "[vibe_engine] VIBEOS_LOAD_MODULE: bad handle\n");
        microkit_mr_set(0, VIBEOS_ERR_BAD_HANDLE);
        microkit_mr_set(1, 0);
        return microkit_msginfo_new(0, 2);
    }

    if (module_type != VIBEOS_MODULE_TYPE_WASM && module_type != VIBEOS_MODULE_TYPE_ELF) {
        log_drain_write(7, 7, "[vibe_engine] VIBEOS_LOAD_MODULE: bad module_type\n");
        microkit_mr_set(0, VIBEOS_ERR_BAD_MODULE_TYPE);
        microkit_mr_set(1, 0);
        return microkit_msginfo_new(0, 2);
    }

    if (module_type == VIBEOS_MODULE_TYPE_WASM) {
        /* Validate WASM magic header */
        const uint8_t *staged = (const uint8_t *)vibe_staging_vaddr;
        if (!validate_wasm_header(staged, module_size)) {
            log_drain_write(7, 7, "[vibe_engine] VIBEOS_LOAD_MODULE: bad WASM magic\n");
            microkit_mr_set(0, VIBEOS_ERR_WASM_LOAD_FAIL);
            microkit_mr_set(1, 0);
            return microkit_msginfo_new(0, 2);
        }
    }

    /*
     * Write swap metadata to the staging region tail (last 64 bytes).
     * Layout (LE uint32): service_id, wasm_offset, wasm_size, swap_id.
     * This matches the format consumed by the controller in handle_execute().
     */
    uint32_t swap_id = next_proposal_id++;
    s_vos[vslot].swap_id = swap_id;
    {
        const struct vibeos_load_module_req *lmreq =
            (const struct vibeos_load_module_req *)vibe_staging_vaddr;
        for (int i = 0; i < 32; i++) s_vos[vslot].module_hash[i] = lmreq->module_hash[i];
    }
    volatile uint8_t *meta = (volatile uint8_t *)(vibe_staging_vaddr + STAGING_SIZE - 64);

    uint32_t svc = (uint32_t)handle;  /* use vibeos handle as service id for swap */
    uint32_t off = 0;
    uint32_t sz  = module_size;
    uint32_t pid = swap_id;

    meta[0]  = svc & 0xff; meta[1]  = (svc >> 8) & 0xff;
    meta[2]  = (svc >> 16) & 0xff; meta[3]  = (svc >> 24) & 0xff;
    meta[4]  = off & 0xff; meta[5]  = (off >> 8) & 0xff;
    meta[6]  = (off >> 16) & 0xff; meta[7]  = (off >> 24) & 0xff;
    meta[8]  = sz & 0xff;  meta[9]  = (sz >> 8) & 0xff;
    meta[10] = (sz >> 16) & 0xff;  meta[11] = (sz >> 24) & 0xff;
    meta[12] = pid & 0xff; meta[13] = (pid >> 8) & 0xff;
    meta[14] = (pid >> 16) & 0xff; meta[15] = (pid >> 24) & 0xff;

    agentos_wmb();

    log_drain_write(7, 7, "[vibe_engine] VIBEOS_LOAD_MODULE: notifying controller\n");
    microkit_notify(CH_CTRL);

    microkit_mr_set(0, VIBEOS_OK);
    microkit_mr_set(1, swap_id);
    return microkit_msginfo_new(0, 2);
}

/*
 * MSG_VIBEOS_CHECK_SERVICE_EXISTS: Query cap_policy for an existing ring-0
 * service PD for a given function class.
 *
 * Input:  MR0=opcode MR1=func_class
 * Output: MR0=ok MR1=exists MR2=pd_handle MR3=channel_id
 */
static microkit_msginfo handle_vibeos_check_service_exists(void)
{
    uint32_t func_class = (uint32_t)microkit_mr_get(1);

    if (func_class < 1 || func_class > CAP_POLICY_FUNC_CLASS_MAX) {
        log_drain_write(7, 7, "[vibe_engine] CHECK_SERVICE_EXISTS: bad func_class\n");
        microkit_mr_set(0, VIBEOS_ERR_BAD_FUNC_CLASS);
        microkit_mr_set(1, 0);
        microkit_mr_set(2, 0);
        microkit_mr_set(3, 0);
        return microkit_msginfo_new(0, 4);
    }

    uint32_t pd_handle  = 0;
    uint32_t channel_id = 0;
    uint32_t exists = (uint32_t)cap_policy_find_ring0_service(func_class, &pd_handle, &channel_id);

    microkit_mr_set(0, VIBEOS_OK);
    microkit_mr_set(1, exists);
    microkit_mr_set(2, pd_handle);
    microkit_mr_set(3, channel_id);
    return microkit_msginfo_new(0, 4);
}

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

    log_drain_write(7, 7, "[vibe_engine] Proposal received: service=");
    if (service_id < service_count && services[service_id].name) {
        log_drain_write(7, 7, services[service_id].name);
    } else {
        log_drain_write(7, 7, "?");
    }
    log_drain_write(7, 7, ", wasm_size=");
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
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = sz_buf; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = " bytes\n"; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        log_drain_write(7, 7, _cl_buf);
    }

    /* Check service exists and is swappable */
    if (service_id >= service_count) {
        log_drain_write(7, 7, "[vibe_engine] REJECT: unknown service\n");
        microkit_mr_set(0, VIBE_ERR_NOSVC);
        return microkit_msginfo_new(0, 1);
    }
    if (!services[service_id].swappable) {
        log_drain_write(7, 7, "[vibe_engine] REJECT: service not swappable\n");
        microkit_mr_set(0, VIBE_ERR_NOSVC);
        return microkit_msginfo_new(0, 1);
    }

    /* Check WASM size */
    if (wasm_size > MAX_WASM_SIZE || wasm_size > services[service_id].max_wasm_bytes) {
        log_drain_write(7, 7, "[vibe_engine] REJECT: WASM too large\n");
        microkit_mr_set(0, VIBE_ERR_TOOBIG);
        return microkit_msginfo_new(0, 1);
    }

    /* Validate WASM magic header from staging region */
    const uint8_t *staged = (const uint8_t *)vibe_staging_vaddr;
    if (!validate_wasm_header(staged, wasm_size)) {
        log_drain_write(7, 7, "[vibe_engine] REJECT: bad WASM magic\n");
        microkit_mr_set(0, VIBE_ERR_BADWASM);
        return microkit_msginfo_new(0, 1);
    }

    /* Find a free proposal slot */
    int slot = find_free_proposal();
    if (slot < 0) {
        log_drain_write(7, 7, "[vibe_engine] REJECT: proposal table full\n");
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

    log_drain_write(7, 7, "[vibe_engine] Proposal accepted: id=");
    char id_buf[4];
    id_buf[0] = '0' + (proposals[slot].version % 10);
    id_buf[1] = '\0';
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = id_buf; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = "\n"; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        log_drain_write(7, 7, _cl_buf);
    }

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

    log_drain_write(7, 7, "[vibe_engine] Validating proposal ");
    char id_buf[4];
    id_buf[0] = '0' + (proposal_id % 10);
    id_buf[1] = '\0';
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = id_buf; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = "...\n"; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        log_drain_write(7, 7, _cl_buf);
    }

    uint32_t checks = 0;
    bool all_pass = true;

    /* Check 0: WASM magic header */
    const uint8_t *staged = (const uint8_t *)(vibe_staging_vaddr + proposals[slot].wasm_offset);
    if (validate_wasm_header(staged, proposals[slot].wasm_size)) {
        checks |= (1 << 0);
        log_drain_write(7, 7, "[vibe_engine]   ✓ WASM magic valid\n");
    } else {
        all_pass = false;
        log_drain_write(7, 7, "[vibe_engine]   ✗ WASM magic INVALID\n");
    }

    /* Check 1: Size within service limits */
    uint32_t svc_id = proposals[slot].service_id;
    if (proposals[slot].wasm_size <= services[svc_id].max_wasm_bytes) {
        checks |= (1 << 1);
        log_drain_write(7, 7, "[vibe_engine]   ✓ Size within limits\n");
    } else {
        all_pass = false;
        log_drain_write(7, 7, "[vibe_engine]   ✗ Size exceeds limit\n");
    }

    /* Check 2: Service is swappable */
    if (services[svc_id].swappable) {
        checks |= (1 << 2);
        log_drain_write(7, 7, "[vibe_engine]   ✓ Service is swappable\n");
    } else {
        all_pass = false;
        log_drain_write(7, 7, "[vibe_engine]   ✗ Service NOT swappable\n");
    }

    /* Check 3: Capability tag is non-zero (basic auth) */
    if (proposals[slot].cap_tag != 0) {
        checks |= (1 << 3);
        log_drain_write(7, 7, "[vibe_engine]   ✓ Capability tag present\n");
    } else {
        all_pass = false;
        log_drain_write(7, 7, "[vibe_engine]   ✗ No capability tag\n");
    }

    proposals[slot].val_checks = checks;
    proposals[slot].val_passed = all_pass;

    if (all_pass) {
        proposals[slot].state = PROP_STATE_VALIDATED;
        log_drain_write(7, 7, "[vibe_engine] Validation PASSED (all 4 checks)\n");
    } else {
        proposals[slot].state = PROP_STATE_REJECTED;
        total_rejections++;
        log_drain_write(7, 7, "[vibe_engine] Validation FAILED\n");
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

    log_drain_write(7, 7, "[vibe_engine] Executing swap for proposal ");
    char id_buf[4];
    id_buf[0] = '0' + (proposal_id % 10);
    id_buf[1] = '\0';
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = id_buf; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = ": service='"; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = services[proposals[slot].service_id].name; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = "'\n"; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        log_drain_write(7, 7, _cl_buf);
    }

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
    agentos_wmb();

    log_drain_write(7, 7, "[vibe_engine] *** SWAP APPROVED — notifying controller ***\n");

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

    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = "[vibe_engine] Rollback requested for '"; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = services[service_id].name; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = "'\n"; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        log_drain_write(7, 7, _cl_buf);
    }

    /* Write rollback command to staging metadata
     * service_id at offset, 0xFFFFFFFF for wasm_size = rollback signal */
    volatile uint8_t *meta = (volatile uint8_t *)(vibe_staging_vaddr + STAGING_SIZE - 64);
    uint32_t svc = service_id;
    meta[0]  = svc & 0xff; meta[1]  = (svc >> 8) & 0xff;
    meta[2]  = (svc >> 16) & 0xff; meta[3]  = (svc >> 24) & 0xff;
    meta[4]  = 0; meta[5] = 0; meta[6] = 0; meta[7] = 0;
    /* wasm_size = 0xFFFFFFFF means rollback */
    meta[8]  = 0xFF; meta[9]  = 0xFF; meta[10] = 0xFF; meta[11] = 0xFF;

    agentos_wmb();

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

/*
 * OP_VIBE_REGISTER_SERVICE: Dynamically register a new swappable service
 *
 * Input:  MR0=op, MR1=name_ptr, MR2=name_len, MR3=max_wasm_bytes (0=default 2MB)
 * Output: MR0=status, MR1=new service_id (on success)
 *
 * Only the root/init caller (badge == 0) is permitted in v0.1; non-zero
 * badge agents are expected to have elevated trust checked by CapStore.
 * The slot's name is copied out of the staging region at the given offset.
 */
static microkit_msginfo handle_register_service(microkit_channel ch) {
    uint32_t name_ptr      = (uint32_t)microkit_mr_get(1);
    uint32_t name_len      = (uint32_t)microkit_mr_get(2);
    uint32_t max_wasm      = (uint32_t)microkit_mr_get(3);

    /* For v0.1, ch==CH_AGENT (badge==0) is root/init — allow all. */
    (void)ch;

    /* Validate name length */
    if (name_len == 0 || name_len > 31) {
        log_drain_write(7, 7, "[vibe_engine] REGISTER: invalid name length\n");
        microkit_mr_set(0, VIBE_ERR_INTERNAL);
        return microkit_msginfo_new(0, 1);
    }

    /* Check capacity */
    if (service_count >= MAX_SERVICES) {
        log_drain_write(7, 7, "[vibe_engine] REGISTER: service table full\n");
        microkit_mr_set(0, VIBE_ERR_FULL);
        return microkit_msginfo_new(0, 1);
    }

    /* Resolve the name pointer: treat as offset into staging region */
    if (name_ptr + name_len > STAGING_SIZE - 64) {
        log_drain_write(7, 7, "[vibe_engine] REGISTER: name out of staging bounds\n");
        microkit_mr_set(0, VIBE_ERR_INTERNAL);
        return microkit_msginfo_new(0, 1);
    }
    const char *src_name = (const char *)(vibe_staging_vaddr + name_ptr);

    /* Check for duplicate name */
    for (uint32_t i = 0; i < service_count; i++) {
        if (!services[i].name) continue;
        const char *existing = services[i].name;
        bool match = true;
        uint32_t j = 0;
        for (; j < name_len && existing[j]; j++) {
            if (existing[j] != src_name[j]) { match = false; break; }
        }
        /* also ensure lengths match: existing[name_len] must be NUL */
        if (match && existing[j] == '\0' && j == name_len) {
            log_drain_write(7, 7, "[vibe_engine] REGISTER: duplicate service name\n");
            microkit_mr_set(0, VIBE_ERR_NOSVC);  /* repurpose: "already exists" */
            return microkit_msginfo_new(0, 1);
        }
    }

    /*
     * Copy the name into a static pool so services[].name remains valid
     * for the lifetime of the engine.  We use a fixed char pool appended
     * in order of registration.
     */
    static char name_pool[MAX_SERVICES * 32];
    static uint32_t pool_next = 0;

    /* Guard against pool overflow (32 bytes per entry, including NUL) */
    if (pool_next + 32 > (uint32_t)sizeof(name_pool)) {
        log_drain_write(7, 7, "[vibe_engine] REGISTER: name pool exhausted\n");
        microkit_mr_set(0, VIBE_ERR_INTERNAL);
        return microkit_msginfo_new(0, 1);
    }

    char *dst = &name_pool[pool_next];
    for (uint32_t i = 0; i < name_len; i++) dst[i] = src_name[i];
    dst[name_len] = '\0';
    pool_next += 32;  /* Fixed stride keeps slots aligned and stable */

    /* Default WASM size limit: 2MB */
    if (max_wasm == 0) max_wasm = 2 * 1024 * 1024;

    uint32_t new_id = service_count;
    services[new_id] = (service_entry_t){
        .name             = dst,
        .swappable        = true,
        .current_version  = 1,
        .max_wasm_bytes   = max_wasm,
    };
    service_count++;

    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = "[vibe_engine] Registered new service '"; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = dst; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = "' id="; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p++ = '0' + (char)(new_id % 10);
        for (const char *_s = "\n"; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        log_drain_write(7, 7, _cl_buf);
    }

    microkit_mr_set(0, VIBE_OK);
    microkit_mr_set(1, new_id);
    return microkit_msginfo_new(0, 2);
}

/*
 * OP_VIBE_LIST_SERVICES: List all registered services
 *
 * Input:  MR0=op
 * Output: MR0=count, MR1=offset_into_staging, MR2=total_bytes
 *
 * Writes a packed array of null-terminated service names into the staging
 * region starting at offset 0.  The caller reads them back from the staging
 * shared-memory window.
 */
static microkit_msginfo handle_list_services(void) {
    /*
     * We write names into the START of the staging region (offset 0).
     * The metadata handoff used by OP_VIBE_EXECUTE lives at the END
     * (last 64 bytes) so there is no conflict as long as a listing is not
     * issued concurrently with an active swap — this is a single-threaded
     * passive PD, so that cannot happen.
     */
    uint8_t *out = (uint8_t *)vibe_staging_vaddr;
    uint32_t out_max = STAGING_SIZE - 64;  /* keep metadata area safe */
    uint32_t pos = 0;

    for (uint32_t i = 0; i < service_count; i++) {
        const char *n = services[i].name;
        if (!n) continue;
        uint32_t j = 0;
        while (n[j] && pos + j + 1 < out_max) {
            out[pos + j] = (uint8_t)n[j];
            j++;
        }
        /* Null-terminate */
        if (pos + j < out_max) out[pos + j] = 0;
        pos += j + 1;
    }

    agentos_wmb();

    microkit_mr_set(0, service_count);
    microkit_mr_set(1, 0);    /* offset: starts at beginning of staging */
    microkit_mr_set(2, pos);  /* total bytes written */
    return microkit_msginfo_new(0, 3);
}

/* ═══════════════════════════════════════════════════════════════════════
 * VibeOS Lifecycle API — VIBEOS_OP_CREATE through VIBEOS_OP_MIGRATE
 *
 * Wire format: MR0 = opcode (0xB001..0xB009), remaining MRs per op.
 * All handlers return MR0=vibeos_error_t, additional MRs on success.
 *
 * Shared memory:
 *   vm_list_ro_vaddr — read-only view of vm_manager's vm_list_shmem.
 *     vm_manager writes vm_list_entry_t[] there on OP_VM_LIST; we read it.
 * ═══════════════════════════════════════════════════════════════════════ */

/* vm_list_shmem (r) set by Microkit linker */
uintptr_t vm_list_ro_vaddr;

/* ── VOS instance table ─────────────────────────────────────────────── */
/* (typedef, s_vos, s_next_handle declared earlier; vos_find defined below) */

static int vos_find(uint32_t handle)
{
    for (int i = 0; i < MAX_VOS_INSTANCES; i++)
        if (s_vos[i].active && s_vos[i].handle == handle) return i;
    return -1;
}

static int vos_alloc(void)
{
    for (int i = 0; i < MAX_VOS_INSTANCES; i++)
        if (!s_vos[i].active) return i;
    return -1;
}

/* ── VIBEOS_OP_CREATE ──────────────────────────────────────────────────
 *
 * Input:  MR0=VIBEOS_OP_CREATE  MR1=os_type  MR2=ram_mb  MR3=dev_flags
 * Output: MR0=vibeos_error_t    MR1=handle (on VIBEOS_OK)
 *
 * Sequence:
 *   1. Validate inputs.
 *   2. Allocate VOS instance slot.
 *   3. PPC to vm_manager: OP_VM_CREATE → vm_slot.
 *   4. PPC to vm_manager: OP_VM_START  → transitions guest to BOOTING.
 *   5. Record instance; return handle.
 *
 * Guest reaches VIBEOS_STATE_RUNNING asynchronously when linux_vmm fires
 * EVENT_GUEST_READY on the EventBus.  The caller must poll STATUS or
 * subscribe to that event to detect readiness.
 * ─────────────────────────────────────────────────────────────────────── */
static microkit_msginfo handle_vos_create(void)
{
    uint32_t os_type   = (uint32_t)microkit_mr_get(1);
    uint32_t ram_mb    = (uint32_t)microkit_mr_get(2);
    uint32_t dev_flags = (uint32_t)microkit_mr_get(3);

    if (os_type > 1) {
        microkit_mr_set(0, VIBEOS_ERR_BAD_TYPE);
        return microkit_msginfo_new(0, 1);
    }
    if (ram_mb == 0 || ram_mb > 8192) {
        microkit_mr_set(0, VIBEOS_ERR_OOM);
        return microkit_msginfo_new(0, 1);
    }

    int slot = vos_alloc();
    if (slot < 0) {
        microkit_mr_set(0, VIBEOS_ERR_OOM);
        return microkit_msginfo_new(0, 1);
    }

    /* ── PPC to vm_manager: create VM slot ──────────────────────────── */
    microkit_mr_set(0, OP_VM_CREATE);
    microkit_mr_set(1, 0);       /* label_vaddr: unnamed at creation time */
    microkit_mr_set(2, ram_mb);
    microkit_msginfo vm_r = microkit_ppcall(CH_VMM,
                                microkit_msginfo_new(OP_VM_CREATE, 3));
    (void)vm_r;
    uint32_t vm_ok   = (uint32_t)microkit_mr_get(0);
    uint32_t vm_slot = (uint32_t)microkit_mr_get(1);

    if (vm_ok != 0) {
        log_drain_write(7, 7, "[vibe_engine] VOS_CREATE: vm_manager rejected CREATE\n");
        microkit_mr_set(0, VIBEOS_ERR_OOM);
        return microkit_msginfo_new(0, 1);
    }

    /* ── PPC to vm_manager: start the VM ────────────────────────────── */
    microkit_mr_set(0, OP_VM_START);
    microkit_mr_set(1, vm_slot);
    (void)microkit_ppcall(CH_VMM, microkit_msginfo_new(OP_VM_START, 2));

    /* ── Record instance ─────────────────────────────────────────────── */
    uint32_t handle = s_next_handle++;
    s_vos[slot].active   = true;
    s_vos[slot].handle   = handle;
    s_vos[slot].vm_slot  = vm_slot;
    s_vos[slot].os_type  = (uint8_t)os_type;
    s_vos[slot].state    = (uint8_t)VIBEOS_STATE_BOOTING;
    s_vos[slot].ram_mb   = ram_mb;
    s_vos[slot].dev_mask = 0;
    s_vos[slot].swap_id  = 0;
    for (int i = 0; i < 5; i++)  s_vos[slot].dev_handles[i] = 0;
    for (int i = 0; i < 32; i++) s_vos[slot].module_hash[i] = 0;

    /* ── Open device handles (guest binding protocol §3.1 step 2) ────── */
    if (dev_flags & VIBEOS_DEV_SERIAL) {
        microkit_mr_set(0, 0);  /* port_id */
        (void)microkit_ppcall((microkit_channel)CH_SERIAL_PD,
                              microkit_msginfo_new(MSG_SERIAL_OPEN, 1));
        if ((uint32_t)microkit_mr_get(0) == 0) {
            s_vos[slot].dev_handles[0] = (uint32_t)microkit_mr_get(1);
            s_vos[slot].dev_mask |= VIBEOS_DEV_SERIAL;
        }
    }
    if (dev_flags & VIBEOS_DEV_NET) {
        microkit_mr_set(0, 0);  /* iface_id */
        (void)microkit_ppcall((microkit_channel)CH_NET_PD,
                              microkit_msginfo_new(MSG_NET_OPEN, 1));
        if ((uint32_t)microkit_mr_get(0) == 0) {
            s_vos[slot].dev_handles[1] = (uint32_t)microkit_mr_get(1);
            s_vos[slot].dev_mask |= VIBEOS_DEV_NET;
        }
    }
    if (dev_flags & VIBEOS_DEV_BLOCK) {
        microkit_mr_set(0, 0);  /* dev_id */
        microkit_mr_set(1, 0);  /* partition */
        (void)microkit_ppcall((microkit_channel)CH_BLOCK_PD,
                              microkit_msginfo_new(MSG_BLOCK_OPEN, 2));
        if ((uint32_t)microkit_mr_get(0) == 0) {
            s_vos[slot].dev_handles[2] = (uint32_t)microkit_mr_get(1);
            s_vos[slot].dev_mask |= VIBEOS_DEV_BLOCK;
        }
    }

    log_drain_write(7, 7, "[vibe_engine] VOS_CREATE: ok\n");

    /* ── Publish EVENT_VIBEOS_READY (deferred if CH_VIBE_EVENTBUS unwired) */
#ifdef CH_VIBE_EVENTBUS
    microkit_mr_set(0, MSG_EVENTBUS_PUBLISH_BATCH);
    microkit_mr_set(1, EVENT_VIBEOS_READY);
    microkit_mr_set(2, handle);
    microkit_mr_set(3, (uint64_t)os_type);
    microkit_mr_set(4, vm_slot);
    (void)microkit_ppcall((microkit_channel)CH_VIBE_EVENTBUS,
                          microkit_msginfo_new(MSG_EVENTBUS_PUBLISH_BATCH, 5));
#endif

    microkit_mr_set(0, VIBEOS_OK);
    microkit_mr_set(1, handle);
    return microkit_msginfo_new(0, 2);
}

/* ── VIBEOS_OP_DESTROY ─────────────────────────────────────────────────
 *
 * Input:  MR0=VIBEOS_OP_DESTROY  MR1=handle
 * Output: MR0=vibeos_error_t
 * ─────────────────────────────────────────────────────────────────────── */
static microkit_msginfo handle_vos_destroy(void)
{
    uint32_t handle = (uint32_t)microkit_mr_get(1);
    int slot = vos_find(handle);
    if (slot < 0) {
        microkit_mr_set(0, VIBEOS_ERR_NO_HANDLE);
        return microkit_msginfo_new(0, 1);
    }

    uint32_t vm_slot = s_vos[slot].vm_slot;

    /* Stop then destroy in vm_manager */
    microkit_mr_set(0, OP_VM_STOP);
    microkit_mr_set(1, vm_slot);
    (void)microkit_ppcall(CH_VMM, microkit_msginfo_new(OP_VM_STOP, 2));

    microkit_mr_set(0, OP_VM_DESTROY);
    microkit_mr_set(1, vm_slot);
    (void)microkit_ppcall(CH_VMM, microkit_msginfo_new(OP_VM_DESTROY, 2));

    s_vos[slot].active = false;
    s_vos[slot].state  = (uint8_t)VIBEOS_STATE_DEAD;

    log_drain_write(7, 7, "[vibe_engine] VOS_DESTROY: ok\n");

    microkit_mr_set(0, VIBEOS_OK);
    return microkit_msginfo_new(0, 1);
}

/* ── VIBEOS_OP_STATUS ──────────────────────────────────────────────────
 *
 * Input:  MR0=VIBEOS_OP_STATUS  MR1=handle
 * Output: MR0=result  MR1=handle  MR2=state  MR3=os_type
 *         MR4=ram_mb  MR5=dev_mask
 *
 * Also queries vm_manager (OP_VM_INFO) to get fresh vm_state in MR1;
 * if the VM reports running we promote the local state to RUNNING.
 * ─────────────────────────────────────────────────────────────────────── */
static microkit_msginfo handle_vos_status(void)
{
    uint32_t handle = (uint32_t)microkit_mr_get(1);
    int slot = vos_find(handle);
    if (slot < 0) {
        microkit_mr_set(0, VIBEOS_ERR_NO_HANDLE);
        return microkit_msginfo_new(0, 1);
    }

    /* Refresh state from vm_manager */
    microkit_mr_set(0, OP_VM_INFO);
    microkit_mr_set(1, s_vos[slot].vm_slot);
    (void)microkit_ppcall(CH_VMM, microkit_msginfo_new(OP_VM_INFO, 2));
    uint32_t vm_ok    = (uint32_t)microkit_mr_get(0);
    uint32_t vm_state = (uint32_t)microkit_mr_get(1);

    /* VM_SLOT_RUNNING (3 in vmm_mux) → VIBEOS_STATE_RUNNING */
    if (vm_ok == 0 && vm_state == 3 &&
        s_vos[slot].state == (uint8_t)VIBEOS_STATE_BOOTING) {
        s_vos[slot].state = (uint8_t)VIBEOS_STATE_RUNNING;
    }

    microkit_mr_set(0, VIBEOS_OK);
    microkit_mr_set(1, handle);
    microkit_mr_set(2, s_vos[slot].state);
    microkit_mr_set(3, s_vos[slot].os_type);
    microkit_mr_set(4, s_vos[slot].ram_mb);
    microkit_mr_set(5, s_vos[slot].dev_mask);
    return microkit_msginfo_new(0, 6);
}

/* ── VIBEOS_OP_LIST ────────────────────────────────────────────────────
 *
 * Input:  MR0=VIBEOS_OP_LIST  MR1=offset
 * Output: MR0=result  MR1=count  MR2..MR(1+count)=handles[]
 *
 * Returns up to 16 active handles per call starting at offset.
 * ─────────────────────────────────────────────────────────────────────── */
static microkit_msginfo handle_vos_list(void)
{
    uint32_t offset = (uint32_t)microkit_mr_get(1);
    uint32_t count  = 0;
    uint32_t seen   = 0;

    for (int i = 0; i < MAX_VOS_INSTANCES && count < 16; i++) {
        if (!s_vos[i].active) continue;
        if (seen < offset) { seen++; continue; }
        microkit_mr_set(2 + count, s_vos[i].handle);
        count++;
        seen++;
    }

    microkit_mr_set(0, VIBEOS_OK);
    microkit_mr_set(1, count);
    return microkit_msginfo_new(0, 2 + count);
}

/* ── VIBEOS_OP_BIND_DEVICE / VIBEOS_OP_UNBIND_DEVICE ──────────────────
 *
 * Input:  MR0=op  MR1=handle  MR2=dev_type (exactly one GUEST_DEV_* bit)
 * Output: MR0=vibeos_error_t
 * ─────────────────────────────────────────────────────────────────────── */
static microkit_msginfo handle_vos_device(uint32_t op)
{
    uint32_t handle   = (uint32_t)microkit_mr_get(1);
    uint32_t dev_type = (uint32_t)microkit_mr_get(2);

    int slot = vos_find(handle);
    if (slot < 0) {
        microkit_mr_set(0, VIBEOS_ERR_NO_HANDLE);
        return microkit_msginfo_new(0, 1);
    }
    /* dev_type must be exactly one bit */
    if (dev_type == 0 || (dev_type & (dev_type - 1)) != 0) {
        microkit_mr_set(0, VIBEOS_ERR_BAD_TYPE);
        return microkit_msginfo_new(0, 1);
    }
    if (s_vos[slot].state == (uint8_t)VIBEOS_STATE_DEAD ||
        s_vos[slot].state == (uint8_t)VIBEOS_STATE_CREATING) {
        microkit_mr_set(0, VIBEOS_ERR_WRONG_STATE);
        return microkit_msginfo_new(0, 1);
    }

    if (op == VIBEOS_OP_BIND_DEVICE)
        s_vos[slot].dev_mask |= dev_type;
    else
        s_vos[slot].dev_mask &= ~dev_type;

    microkit_mr_set(0, VIBEOS_OK);
    return microkit_msginfo_new(0, 1);
}

/* ── VIBEOS_OP_SNAPSHOT ────────────────────────────────────────────────
 *
 * Input:  MR0=VIBEOS_OP_SNAPSHOT  MR1=handle
 * Output: MR0=result  MR1=handle  MR2=snap_hash_lo  MR3=snap_hash_hi
 *
 * Forwards to vm_manager OP_VM_SNAPSHOT.
 * ─────────────────────────────────────────────────────────────────────── */
static microkit_msginfo handle_vos_snapshot(void)
{
    uint32_t handle = (uint32_t)microkit_mr_get(1);
    int slot = vos_find(handle);
    if (slot < 0) {
        microkit_mr_set(0, VIBEOS_ERR_NO_HANDLE);
        return microkit_msginfo_new(0, 1);
    }
    if (s_vos[slot].state != (uint8_t)VIBEOS_STATE_RUNNING &&
        s_vos[slot].state != (uint8_t)VIBEOS_STATE_PAUSED) {
        microkit_mr_set(0, VIBEOS_ERR_WRONG_STATE);
        return microkit_msginfo_new(0, 1);
    }

    s_vos[slot].state = (uint8_t)VIBEOS_STATE_SNAPSHOT;

    microkit_mr_set(0, OP_VM_SNAPSHOT);
    microkit_mr_set(1, s_vos[slot].vm_slot);
    (void)microkit_ppcall(CH_VMM, microkit_msginfo_new(OP_VM_SNAPSHOT, 2));
    uint32_t ok   = (uint32_t)microkit_mr_get(0);
    uint32_t h_lo = (uint32_t)microkit_mr_get(1);
    uint32_t h_hi = (uint32_t)microkit_mr_get(2);

    s_vos[slot].state = (uint8_t)VIBEOS_STATE_RUNNING;

    if (ok != 0) {
        microkit_mr_set(0, VIBEOS_ERR_NOT_IMPL);
        return microkit_msginfo_new(0, 1);
    }

    microkit_mr_set(0, VIBEOS_OK);
    microkit_mr_set(1, handle);
    microkit_mr_set(2, h_lo);
    microkit_mr_set(3, h_hi);
    return microkit_msginfo_new(0, 4);
}

/* ── VIBEOS_OP_RESTORE ─────────────────────────────────────────────────
 *
 * Input:  MR0=VIBEOS_OP_RESTORE  MR1=handle  MR2=snap_lo  MR3=snap_hi
 * Output: MR0=vibeos_error_t
 * ─────────────────────────────────────────────────────────────────────── */
static microkit_msginfo handle_vos_restore(void)
{
    uint32_t handle  = (uint32_t)microkit_mr_get(1);
    uint32_t snap_lo = (uint32_t)microkit_mr_get(2);
    uint32_t snap_hi = (uint32_t)microkit_mr_get(3);

    int slot = vos_find(handle);
    if (slot < 0) {
        microkit_mr_set(0, VIBEOS_ERR_NO_HANDLE);
        return microkit_msginfo_new(0, 1);
    }

    microkit_mr_set(0, OP_VM_RESTORE);
    microkit_mr_set(1, s_vos[slot].vm_slot);
    microkit_mr_set(2, snap_lo);
    microkit_mr_set(3, snap_hi);
    (void)microkit_ppcall(CH_VMM, microkit_msginfo_new(OP_VM_RESTORE, 4));
    uint32_t ok = (uint32_t)microkit_mr_get(0);

    if (ok != 0) {
        microkit_mr_set(0, VIBEOS_ERR_NOT_IMPL);
        return microkit_msginfo_new(0, 1);
    }

    s_vos[slot].state = (uint8_t)VIBEOS_STATE_BOOTING;
    microkit_mr_set(0, VIBEOS_OK);
    return microkit_msginfo_new(0, 1);
}

/* ── VIBEOS_OP_MIGRATE — Phase 4+ placeholder ──────────────────────── */
static microkit_msginfo handle_vos_migrate(void)
{
    microkit_mr_set(0, VIBEOS_ERR_NOT_IMPL);
    return microkit_msginfo_new(0, 1);
}

/* ── VIBEOS_OP_BOOT ─────────────────────────────────────────────────────
 *
 * Explicit CREATING→BOOTING transition for callers that pre-configure a
 * slot before starting the VM.  Most callers use MSG_VIBEOS_CREATE which
 * starts the VM directly; this op exists for two-phase create workflows.
 *
 * Input:  MR0=MSG_VIBEOS_BOOT  MR1=handle
 * Output: MR0=vibeos_error_t
 * ─────────────────────────────────────────────────────────────────────── */
static microkit_msginfo handle_vos_boot(void)
{
    uint32_t handle = (uint32_t)microkit_mr_get(1);
    int slot = vos_find(handle);
    if (slot < 0) {
        microkit_mr_set(0, VIBEOS_ERR_NO_HANDLE);
        return microkit_msginfo_new(0, 1);
    }
    if (s_vos[slot].state != (uint8_t)VIBEOS_STATE_CREATING) {
        microkit_mr_set(0, VIBEOS_ERR_WRONG_STATE);
        return microkit_msginfo_new(0, 1);
    }

    microkit_mr_set(0, OP_VM_START);
    microkit_mr_set(1, s_vos[slot].vm_slot);
    (void)microkit_ppcall(CH_VMM, microkit_msginfo_new(OP_VM_START, 2));

    s_vos[slot].state = (uint8_t)VIBEOS_STATE_BOOTING;
    microkit_mr_set(0, VIBEOS_OK);
    return microkit_msginfo_new(0, 1);
}

/* ── VIBEOS_OP_CONFIGURE ───────────────────────────────────────────────
 *
 * Modify OS parameters without destroying/recreating the VM.
 *
 * Input:  MR0=MSG_VIBEOS_CONFIGURE  MR1=handle
 *         MR2=ram_mb (0=no change)  MR3=cpu_budget_us (0=no change)
 *         MR4=cpu_period_us (0=no change)
 * Output: MR0=result
 *
 * Valid in any state except VIBEOS_STATE_DEAD.  Updates local s_vos[]
 * and forwards non-zero fields to vm_manager via OP_VM_CONFIGURE.
 * ─────────────────────────────────────────────────────────────────────── */
static microkit_msginfo handle_vos_configure(void)
{
    uint32_t handle        = (uint32_t)microkit_mr_get(1);
    uint32_t new_ram       = (uint32_t)microkit_mr_get(2);
    uint32_t new_budget_us = (uint32_t)microkit_mr_get(3);
    uint32_t new_period_us = (uint32_t)microkit_mr_get(4);

    int slot = vos_find(handle);
    if (slot < 0) {
        microkit_mr_set(0, VIBEOS_ERR_NO_HANDLE);
        return microkit_msginfo_new(0, 1);
    }
    if (s_vos[slot].state == (uint8_t)VIBEOS_STATE_DEAD) {
        microkit_mr_set(0, VIBEOS_ERR_WRONG_STATE);
        return microkit_msginfo_new(0, 1);
    }

    if (new_ram)       s_vos[slot].ram_mb = new_ram;

    microkit_mr_set(0, OP_VM_CONFIGURE);
    microkit_mr_set(1, s_vos[slot].vm_slot);
    microkit_mr_set(2, new_ram);
    microkit_mr_set(3, new_budget_us);
    microkit_mr_set(4, new_period_us);
    (void)microkit_ppcall(CH_VMM, microkit_msginfo_new(OP_VM_CONFIGURE, 5));

    log_drain_write(7, 7, "[vibe_engine] VOS_CONFIGURE: ok\n");
    microkit_mr_set(0, VIBEOS_OK);
    return microkit_msginfo_new(0, 1);
}

/* ── Microkit entry points ──────────────────────────────────────────── */

void init(void) {
    log_drain_write(7, 7, "[vibe_engine] VibeEngine PD starting...\n");

    /* Initialize proposal table */
    for (int i = 0; i < MAX_PROPOSALS; i++)
        proposals[i].state = PROP_STATE_FREE;

    /* Initialize VOS instance table */
    for (int i = 0; i < MAX_VOS_INSTANCES; i++)
        s_vos[i].active = false;

    /* Register known services */
    register_services();

    log_drain_write(7, 7, "[vibe_engine] Services registered: ");
    char cnt[4];
    cnt[0] = '0' + service_count;
    cnt[1] = '\0';
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = cnt; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = " ("; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        log_drain_write(7, 7, _cl_buf);
    }
    int swappable = 0;
    for (uint32_t i = 0; i < service_count; i++) {
        if (services[i].swappable) swappable++;
    }
    char sw[4];
    sw[0] = '0' + swappable;
    sw[1] = '\0';
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = sw; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = " swappable)\n"; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = "[vibe_engine] Proposal table: "; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        log_drain_write(7, 7, _cl_buf);
    }
    char mx[4];
    mx[0] = '0' + MAX_PROPOSALS;
    mx[1] = '\0';
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = mx; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = " slots\n"; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = "[vibe_engine] Staging region: 4MB at 0x"; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        log_drain_write(7, 7, _cl_buf);
    }
    /* Print vaddr as hex */
    uintptr_t va = vibe_staging_vaddr;
    char hex[20];
    int hi = 0;
    for (int shift = 28; shift >= 0; shift -= 4) {
        uint8_t nibble = (va >> shift) & 0xF;
        hex[hi++] = nibble < 10 ? ('0' + nibble) : ('a' + nibble - 10);
    }
    hex[hi] = '\0';
    {
        char _cl_buf[256] = {};
        char *_cl_p = _cl_buf;
        for (const char *_s = hex; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = "\n"; *_s; _s++) *_cl_p++ = *_s;
        for (const char *_s = "[vibe_engine] *** VibeEngine ALIVE — accepting proposals ***\n"; *_s; _s++) *_cl_p++ = *_s;
        *_cl_p = 0;
        log_drain_write(7, 7, _cl_buf);
    }
}

/* Passive PD — only woken by PPC or notification */
void notified(microkit_channel ch) {
    if (ch == CH_CTRL) {
        log_drain_write(7, 7, "[vibe_engine] Controller ack received\n");
    }
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msg) {
    (void)msg;  /* Op code is in MR0, not the label */
    uint32_t op = (uint32_t)microkit_mr_get(0);

    switch (op) {
        /* ── Hot-swap proposal lifecycle ─────────────────────────────── */
        case OP_VIBE_PROPOSE:            return handle_propose();
        case OP_VIBE_VALIDATE:           return handle_validate();
        case OP_VIBE_EXECUTE:            return handle_execute();
        case OP_VIBE_STATUS:             return handle_status();
        case OP_VIBE_ROLLBACK:           return handle_rollback();
        case OP_VIBE_HEALTH:             return handle_health();
        case OP_VIBE_REGISTER_SERVICE:   return handle_register_service(ch);
        case OP_VIBE_LIST_SERVICES:      return handle_list_services();

        /* ── VibeOS OS lifecycle (all routed through vos_ handlers) ─────── */
        case MSG_VIBEOS_CREATE:                 return handle_vos_create();
        case MSG_VIBEOS_DESTROY:                return handle_vos_destroy();
        case MSG_VIBEOS_STATUS:                 return handle_vos_status();
        case MSG_VIBEOS_LIST:                   return handle_vos_list();
        case MSG_VIBEOS_BIND_DEVICE:            return handle_vos_device(MSG_VIBEOS_BIND_DEVICE);
        case MSG_VIBEOS_UNBIND_DEVICE:          return handle_vos_device(MSG_VIBEOS_UNBIND_DEVICE);
        case MSG_VIBEOS_BOOT:                   return handle_vos_boot();
        case MSG_VIBEOS_LOAD_MODULE:            return handle_vibeos_load_module();
        case MSG_VIBEOS_CHECK_SERVICE_EXISTS:   return handle_vibeos_check_service_exists();
        case MSG_VIBEOS_SNAPSHOT:               return handle_vos_snapshot();
        case MSG_VIBEOS_RESTORE:                return handle_vos_restore();
        case MSG_VIBEOS_MIGRATE:                return handle_vos_migrate();
        case MSG_VIBEOS_CONFIGURE:              return handle_vos_configure();

        default:
            log_drain_write(7, 7, "[vibe_engine] Unknown op\n");
            microkit_mr_set(0, VIBE_ERR_INTERNAL);
            return microkit_msginfo_new(0, 1);
    }
}
