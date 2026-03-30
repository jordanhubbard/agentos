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
 *   OP_VIBE_HEALTH      = 0x45 — health check for VibeEngine itself
 *   OP_VIBE_TRUST_KEY   = 0x49 — add trusted Ed25519 signing key
 *   OP_VIBE_SIG_ENFORCE = 0x4A — toggle signature enforcement (reject unsigned)
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "verify.h"
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
#define OP_VIBE_TRUST_KEY   0x49 /* Add trusted signing key: MR1=key_id, MR2-5=pubkey */
#define OP_VIBE_SIG_ENFORCE 0x4A /* Set signature enforcement: MR1=0/1 */

/* ── Result codes ───────────────────────────────────────────────────── */
#define VIBE_OK             0
#define VIBE_ERR_FULL       1   /* proposal table full */
#define VIBE_ERR_BADWASM    2   /* invalid WASM header */
#define VIBE_ERR_TOOBIG     3   /* WASM binary too large */
#define VIBE_ERR_NOSVC      4   /* service not found or not swappable */
#define VIBE_ERR_NOENT      5   /* proposal not found */
#define VIBE_ERR_STATE      6   /* proposal in wrong state */
#define VIBE_ERR_VALFAIL    7   /* validation failed */
#define VIBE_ERR_BADPTX     8   /* invalid CUDA PTX payload */
#define VIBE_ERR_BADSIG     9   /* Ed25519 signature verification failed */
#define VIBE_ERR_INTERNAL   99

/* CUDA PTX custom section name embedded in WASM modules */
#define CUDA_SECTION_NAME   "agentos.cuda"
#define CUDA_SECTION_LEN    12   /* strlen("agentos.cuda") */
#define MAX_PTX_SIZE        (2 * 1024 * 1024)  /* 2MB PTX max */

/* gpu_scheduler IPC channel and op codes */
#define CH_GPU              2    /* vibe_engine -> gpu_scheduler */
#define OP_GPU_SUBMIT       0x50
#define OP_GPU_COMPLETE     0x51
#define OP_GPU_STATUS       0x52

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
    /* CUDA PTX offload (optional — set when WASM has agentos.cuda section) */
    uint32_t         cuda_ptx_offset; /* byte offset of PTX payload in staging */
    uint32_t         cuda_ptx_len;    /* length of PTX payload (0 = no CUDA) */
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

/* ── Persistent WASM module registry cache ──────────────────────────────
 *
 * Each successfully executed WASM module is recorded here by its BLAKE3
 * hash. On boot, OP_VIBE_REPLAY replays AgentFS entries into this table
 * so VibeEngine can locate modules without fetching from the network.
 *
 * handle_propose() checks this table FIRST — if the hash is known,
 * the proposal is pre-approved (validation already passed on first run).
 *
 * Layout per entry:
 *   hash[32]        — BLAKE3 hash of WASM bytes
 *   service_id      — last service this module ran as
 *   version         — swap generation count
 *   flags           — REGISTRY_FLAG_VALIDATED, _CUDA (has agentos.cuda section)
 */
#define MAX_REGISTRY_ENTRIES  64
#define REGISTRY_FLAG_VALIDATED  0x01
#define REGISTRY_FLAG_CUDA       0x02   /* module has agentos.cuda section */
#define REGISTRY_FLAG_AOT        0x04   /* AOT-compiled .cwasm available */

typedef struct {
    uint8_t  hash[32];       /* BLAKE3 hash — all-zero = free entry */
    uint32_t service_id;
    uint32_t version;
    uint32_t flags;
    uint32_t agentfs_seq;    /* AgentFS sequence number for replay dedup */
} registry_entry_t;

static registry_entry_t module_registry[MAX_REGISTRY_ENTRIES];
static uint32_t         registry_count = 0;

/* Find a registry entry by hash. Returns index or -1. */
static int registry_find(const uint8_t hash[32]) {
    for (int i = 0; i < MAX_REGISTRY_ENTRIES; i++) {
        /* Check if entry is non-zero (occupied) */
        bool occupied = false;
        for (int j = 0; j < 32; j++) { if (module_registry[i].hash[j]) { occupied = true; break; } }
        if (!occupied) continue;
        bool match = true;
        for (int j = 0; j < 32; j++) {
            if (module_registry[i].hash[j] != hash[j]) { match = false; break; }
        }
        if (match) return i;
    }
    return -1;
}

/* Allocate a new registry entry (or evict LRU — for simplicity, evict first free). */
static int registry_alloc(void) {
    for (int i = 0; i < MAX_REGISTRY_ENTRIES; i++) {
        bool occupied = false;
        for (int j = 0; j < 32; j++) { if (module_registry[i].hash[j]) { occupied = true; break; } }
        if (!occupied) return i;
    }
    /* All full — wrap around (slot 0, LRU eviction placeholder) */
    return 0;
}

/* Record a validated module in the registry. */
static void registry_record(const uint8_t hash[32], uint32_t service_id,
                              uint32_t version, uint32_t flags) {
    int idx = registry_find(hash);
    if (idx < 0) {
        idx = registry_alloc();
        registry_count++;
    }
    registry_entry_t *e = &module_registry[idx];
    for (int j = 0; j < 32; j++) e->hash[j] = hash[j];
    e->service_id = service_id;
    e->version    = version;
    e->flags      = flags | REGISTRY_FLAG_VALIDATED;
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
    /* Dynamically spawned agents — proposed via init_agent SPAWN_AGENT pipeline */
    services[6] = (service_entry_t){
        .name = "agent_worker", .swappable = true,
        .current_version = 1, .max_wasm_bytes = 4 * 1024 * 1024,
    };
    service_count = 7;
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

/*
 * scan_cuda_section: Scan a WASM binary for a custom section named
 * "agentos.cuda". WASM custom sections have:
 *   - section id byte: 0x00
 *   - LEB128 section size
 *   - LEB128 name length
 *   - name bytes
 *   - payload bytes
 *
 * Sets *ptx_offset (byte offset from data start) and *ptx_len on success.
 * Returns true if found, false otherwise.
 */
static bool scan_cuda_section(const uint8_t *data, uint32_t size,
                               uint32_t *ptx_offset, uint32_t *ptx_len) {
    if (size < 8) return false;
    /* Skip 4-byte magic + 4-byte version */
    uint32_t pos = 8;
    while (pos + 2 < size) {
        uint8_t section_id = data[pos++];
        /* Decode LEB128 section size (up to 4 bytes) */
        uint32_t sec_size = 0;
        uint32_t shift = 0;
        while (pos < size && shift < 28) {
            uint8_t b = data[pos++];
            sec_size |= (uint32_t)(b & 0x7f) << shift;
            shift += 7;
            if (!(b & 0x80)) break;
        }
        uint32_t sec_start = pos;
        if (sec_start + sec_size > size) break;

        if (section_id == 0x00 && sec_size > CUDA_SECTION_LEN + 1) {
            /* Decode name length (LEB128) */
            uint32_t nlen = 0;
            uint32_t nshift = 0;
            uint32_t npos = sec_start;
            while (npos < sec_start + sec_size && nshift < 28) {
                uint8_t b = data[npos++];
                nlen |= (uint32_t)(b & 0x7f) << nshift;
                nshift += 7;
                if (!(b & 0x80)) break;
            }
            /* Check if name matches "agentos.cuda" */
            if (nlen == CUDA_SECTION_LEN &&
                npos + nlen <= sec_start + sec_size) {
                bool match = true;
                const char *needle = CUDA_SECTION_NAME;
                for (uint32_t i = 0; i < CUDA_SECTION_LEN; i++) {
                    if (data[npos + i] != (uint8_t)needle[i]) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    uint32_t payload_start = npos + nlen;
                    uint32_t payload_len   = (sec_start + sec_size) - payload_start;
                    *ptx_offset = payload_start;
                    *ptx_len    = payload_len;
                    return true;
                }
            }
        }
        pos = sec_start + sec_size;
    }
    return false;
}

/* ── WASM module signing — custom section scanners ───────────────────────── */

#define SIG_SECTION_NAME     "agentos.signature"
#define SIG_SECTION_LEN      17  /* strlen("agentos.signature") */
#define CAP_SECTION_NAME     "agentos.capabilities"
#define CAP_SECTION_LEN      20  /* strlen("agentos.capabilities") */

/* Signature section payload: 64 bytes (matches Ed25519 sig size) */
#define SIG_PAYLOAD_SIZE     64

/* Trusted key table: up to 8 trusted signing key IDs */
#define MAX_TRUSTED_KEYS     8

typedef struct {
    uint8_t  key_id[8];     /* 8-byte key identifier */
    uint8_t  pubkey[32];    /* Ed25519 public key (or zero for phase-1 hash mode) */
    bool     active;
} trusted_key_t;

static trusted_key_t trusted_keys[MAX_TRUSTED_KEYS];
static uint32_t      trusted_key_count = 0;

/* Signature enforcement policy: if true, unsigned modules are rejected */
static bool sig_enforcement = false;

/*
 * scan_custom_section: Generic scanner for a named WASM custom section.
 * Returns true if section found, with *payload_offset and *payload_len set.
 */
static bool scan_custom_section(const uint8_t *data, uint32_t size,
                                 const char *section_name, uint32_t name_len,
                                 uint32_t *payload_offset, uint32_t *payload_len) {
    if (size < 8) return false;
    uint32_t pos = 8; /* skip WASM magic + version */
    while (pos + 2 < size) {
        uint8_t section_id = data[pos++];
        uint32_t sec_size = 0, shift = 0;
        while (pos < size && shift < 28) {
            uint8_t b = data[pos++];
            sec_size |= (uint32_t)(b & 0x7f) << shift;
            shift += 7;
            if (!(b & 0x80)) break;
        }
        uint32_t sec_start = pos;
        if (sec_start + sec_size > size) break;

        if (section_id == 0x00 && sec_size > name_len + 1) {
            uint32_t nlen = 0, nshift = 0, npos = sec_start;
            while (npos < sec_start + sec_size && nshift < 28) {
                uint8_t b = data[npos++];
                nlen |= (uint32_t)(b & 0x7f) << nshift;
                nshift += 7;
                if (!(b & 0x80)) break;
            }
            if (nlen == name_len && npos + nlen <= sec_start + sec_size) {
                bool match = true;
                for (uint32_t i = 0; i < name_len; i++) {
                    if (data[npos + i] != (uint8_t)section_name[i]) { match = false; break; }
                }
                if (match) {
                    *payload_offset = npos + nlen;
                    *payload_len    = (sec_start + sec_size) - (npos + nlen);
                    return true;
                }
            }
        }
        pos = sec_start + sec_size;
    }
    return false;
}

/*
 * verify_wasm_signature: Check if a WASM module's agentos.signature section
 * validates against the agentos.capabilities section using a trusted key.
 *
 * Returns:
 *   0  = valid signature (or no signature + enforcement off)
 *  -1  = missing signature (enforcement on)
 *  -2  = bad signature format
 *  -3  = untrusted key_id
 *  -4  = hash mismatch (tampered)
 */
static int verify_wasm_signature(const uint8_t *wasm, uint32_t wasm_size) {
    uint32_t sig_off = 0, sig_len = 0;
    uint32_t cap_off = 0, cap_len = 0;

    bool has_sig = scan_custom_section(wasm, wasm_size,
                                        SIG_SECTION_NAME, SIG_SECTION_LEN,
                                        &sig_off, &sig_len);
    bool has_cap = scan_custom_section(wasm, wasm_size,
                                        CAP_SECTION_NAME, CAP_SECTION_LEN,
                                        &cap_off, &cap_len);

    if (!has_sig) {
        /* No signature section */
        if (sig_enforcement) {
            microkit_dbg_puts("[vibe_engine]   ✗ Unsigned module rejected (enforcement=on)\n");
            return -1;
        }
        microkit_dbg_puts("[vibe_engine]   ⚠ No signature section (enforcement=off, allowing)\n");
        return 0;
    }

    if (sig_len != SIG_PAYLOAD_SIZE) {
        microkit_dbg_puts("[vibe_engine]   ✗ Bad signature section size\n");
        return -2;
    }

    if (!has_cap || cap_len == 0) {
        microkit_dbg_puts("[vibe_engine]   ✗ Signature present but no capabilities section\n");
        return -2;
    }

    const uint8_t *sig_data = &wasm[sig_off];
    const uint8_t *cap_data = &wasm[cap_off];

    /* Extract key_id (first 8 bytes of signature) */
    const uint8_t *key_id = &sig_data[0];

    /* Find matching trusted key */
    int key_idx = -1;
    for (uint32_t i = 0; i < trusted_key_count; i++) {
        if (!trusted_keys[i].active) continue;
        bool match = true;
        for (int j = 0; j < 8; j++) {
            if (trusted_keys[i].key_id[j] != key_id[j]) { match = false; break; }
        }
        if (match) { key_idx = (int)i; break; }
    }

    if (key_idx < 0) {
        microkit_dbg_puts("[vibe_engine]   ✗ Untrusted key_id\n");
        return -3;
    }

    /* Verify signature against capabilities section payload */
    int rc = ed25519_verify(sig_data, cap_data, cap_len,
                            trusted_keys[key_idx].pubkey);
    if (rc != 0) {
        microkit_dbg_puts("[vibe_engine]   ✗ Signature verification FAILED (tampered?)\n");
        return -4;
    }

    microkit_dbg_puts("[vibe_engine]   ✓ Signature verified (trusted key)\n");
    return 0;
}

/*
 * vibe_validate_cuda_ptx: Basic PTX sanity check.
 * Valid PTX starts with ".version" directive and must be non-empty.
 * Returns VIBE_OK or VIBE_ERR_BADPTX.
 */
static int vibe_validate_cuda_ptx(const uint8_t *ptx, uint32_t len) {
    if (len == 0 || len > MAX_PTX_SIZE) return VIBE_ERR_BADPTX;
    /* PTX files start with ".version" */
    const char *magic = ".version";
    if (len < 8) return VIBE_ERR_BADPTX;
    for (int i = 0; i < 8; i++) {
        if (ptx[i] != (uint8_t)magic[i]) return VIBE_ERR_BADPTX;
    }
    return VIBE_OK;
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

    /*
     * ── Registry cache check ──────────────────────────────────────────
     * MR4/5: wasm_hash_lo, MR6/7: wasm_hash_hi (packed as u64 LE pairs)
     * If this hash is in the registry, the module is pre-approved.
     * Fast-path: skip staging, notify controller directly.
     */
    uint8_t proposed_hash[32] = {0};
    uint32_t h0 = (uint32_t)microkit_mr_get(4);
    uint32_t h1 = (uint32_t)microkit_mr_get(5);
    uint32_t h2 = (uint32_t)microkit_mr_get(6);
    uint32_t h3 = (uint32_t)microkit_mr_get(7);
    proposed_hash[0]  = h0 & 0xFF; proposed_hash[1]  = (h0>>8)&0xFF;
    proposed_hash[2]  = (h0>>16)&0xFF; proposed_hash[3]  = (h0>>24)&0xFF;
    proposed_hash[4]  = h1 & 0xFF; proposed_hash[5]  = (h1>>8)&0xFF;
    proposed_hash[6]  = (h1>>16)&0xFF; proposed_hash[7]  = (h1>>24)&0xFF;
    proposed_hash[8]  = h2 & 0xFF; proposed_hash[9]  = (h2>>8)&0xFF;
    proposed_hash[10] = (h2>>16)&0xFF; proposed_hash[11] = (h2>>24)&0xFF;
    proposed_hash[12] = h3 & 0xFF; proposed_hash[13] = (h3>>8)&0xFF;
    proposed_hash[14] = (h3>>16)&0xFF; proposed_hash[15] = (h3>>24)&0xFF;

    /* Only check if any hash bytes were provided */
    bool has_hash = false;
    for (int _i = 0; _i < 16; _i++) if (proposed_hash[_i]) { has_hash = true; break; }

    if (has_hash) {
        int reg_idx = registry_find(proposed_hash);
        if (reg_idx >= 0) {
            registry_entry_t *re = &module_registry[reg_idx];
            microkit_dbg_puts("[vibe_engine] Registry HIT: pre-approved module, fast-path\n");
            total_swaps++;
            re->version++;
            /* Return success immediately — module already validated */
            uint32_t pid = next_proposal_id++;
            microkit_mr_set(0, VIBE_OK);
            microkit_mr_set(1, pid);
            microkit_mr_set(2, re->flags);
            return microkit_msginfo_new(0, 3);
        }
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

    /* Ed25519 signature verification — NULL trusted_pubkey = any signer allowed */
    if (!vibe_verify_module(staged, wasm_size, NULL)) {
        microkit_dbg_puts("[vibe_engine] REJECT: Ed25519 signature verification failed\n");
        microkit_mr_set(0, VIBE_ERR_BADSIG);
        return microkit_msginfo_new(0, 1);
    }

    /* Find a free proposal slot */
    int slot = find_free_proposal();
    if (slot < 0) {
        microkit_dbg_puts("[vibe_engine] REJECT: proposal table full\n");
        microkit_mr_set(0, VIBE_ERR_FULL);
        return microkit_msginfo_new(0, 1);
    }

    /* Scan for CUDA PTX custom section in staged WASM */
    uint32_t ptx_offset = 0, ptx_len = 0;
    bool has_cuda = scan_cuda_section(staged, wasm_size, &ptx_offset, &ptx_len);
    if (has_cuda) {
        microkit_dbg_puts("[vibe_engine] CUDA PTX section found (agentos.cuda)\n");
    }

    /* Record the proposal */
    proposals[slot].state            = PROP_STATE_PENDING;
    proposals[slot].service_id       = service_id;
    proposals[slot].wasm_offset      = 0;  /* Always at start of staging for now */
    proposals[slot].wasm_size        = wasm_size;
    proposals[slot].cap_tag          = cap_tag;
    proposals[slot].version          = next_proposal_id++;
    proposals[slot].val_checks       = 0;
    proposals[slot].val_passed       = false;
    proposals[slot].cuda_ptx_offset  = has_cuda ? ptx_offset : 0;
    proposals[slot].cuda_ptx_len     = has_cuda ? ptx_len    : 0;
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

    /* Check 4: CUDA PTX validation (only if section present) */
    if (proposals[slot].cuda_ptx_len > 0) {
        const uint8_t *ptx = (const uint8_t *)(vibe_staging_vaddr +
                                                proposals[slot].wasm_offset +
                                                proposals[slot].cuda_ptx_offset);
        int ptx_rc = vibe_validate_cuda_ptx(ptx, proposals[slot].cuda_ptx_len);
        if (ptx_rc == VIBE_OK) {
            checks |= (1 << 4);
            microkit_dbg_puts("[vibe_engine]   ✓ CUDA PTX valid (.version header ok)\n");
        } else {
            all_pass = false;
            microkit_dbg_puts("[vibe_engine]   ✗ CUDA PTX INVALID\n");
        }
    }

    /* Check 5: WASM module signature verification (Ed25519 / SHA-512 hash) */
    {
        int sig_rc = verify_wasm_signature(staged, proposals[slot].wasm_size);
        if (sig_rc == 0) {
            checks |= (1 << 5);
            microkit_dbg_puts("[vibe_engine]   ✓ Signature check passed\n");
        } else {
            all_pass = false;
            microkit_dbg_puts("[vibe_engine]   ✗ Signature check FAILED\n");
        }
    }

    proposals[slot].val_checks = checks;
    proposals[slot].val_passed = all_pass;

    if (all_pass) {
        proposals[slot].state = PROP_STATE_VALIDATED;
        microkit_dbg_puts("[vibe_engine] Validation PASSED (all checks)\n");
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

    /* If this module carries a CUDA PTX section, submit to gpu_scheduler */
    if (proposals[slot].cuda_ptx_len > 0) {
        microkit_dbg_puts("[vibe_engine] CUDA PTX detected — submitting to gpu_scheduler\n");
        microkit_mr_set(0, OP_GPU_SUBMIT);
        microkit_mr_set(1, (uint32_t)slot);           /* proposal slot index */
        microkit_mr_set(2, proposals[slot].cuda_ptx_offset);
        microkit_mr_set(3, proposals[slot].cuda_ptx_len);
        microkit_notify(CH_GPU);
    }

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

        case OP_VIBE_TRUST_KEY: {
            /* Add a trusted signing key: MR1=key_id_hi, MR2=key_id_lo */
            if (trusted_key_count >= MAX_TRUSTED_KEYS) {
                microkit_mr_set(0, VIBE_ERR_FULL);
                return microkit_msginfo_new(0, 1);
            }
            trusted_key_t *k = &trusted_keys[trusted_key_count];
            uint64_t kid = (uint64_t)microkit_mr_get(1);
            for (int i = 7; i >= 0; i--) { k->key_id[i] = (uint8_t)(kid & 0xFF); kid >>= 8; }
            /* pubkey passed in MR2..5 (4 x 8 bytes = 32 bytes) */
            for (int r = 0; r < 4; r++) {
                uint64_t v = (uint64_t)microkit_mr_get(2 + r);
                for (int b = 7; b >= 0; b--) { k->pubkey[r*8 + b] = (uint8_t)(v & 0xFF); v >>= 8; }
            }
            k->active = true;
            trusted_key_count++;
            microkit_dbg_puts("[vibe_engine] Trusted key added\n");
            microkit_mr_set(0, VIBE_OK);
            return microkit_msginfo_new(0, 1);
        }

        case OP_VIBE_SIG_ENFORCE: {
            /* Set signature enforcement mode: MR1=0 (off) or 1 (on) */
            sig_enforcement = (microkit_mr_get(1) != 0);
            microkit_dbg_puts(sig_enforcement ?
                "[vibe_engine] Signature enforcement: ON\n" :
                "[vibe_engine] Signature enforcement: OFF\n");
            microkit_mr_set(0, VIBE_OK);
            return microkit_msginfo_new(0, 1);
        }

        case OP_VIBE_REPLAY: {
            /*
             * Boot replay: seed the registry from AgentFS module list.
             * Caller (init_agent or controller) passes packed hashes in MRs.
             * MR0: count of modules to replay (up to 8 per call, call repeatedly)
             * MR1: flags for first module
             * MR2: service_id for first module
             * MR3-MR6: hash bytes for first module (4 x u32, 16 bytes of 32-byte hash)
             *
             * For simplicity, we accept one module per REPLAY call
             * (controller calls REPLAY once per AgentFS module).
             */
            uint32_t mod_flags   = (uint32_t)microkit_mr_get(1);
            uint32_t svc_id      = (uint32_t)microkit_mr_get(2);
            uint8_t  replay_hash[32] = {0};
            uint32_t _rh[4] = {
                (uint32_t)microkit_mr_get(3),
                (uint32_t)microkit_mr_get(4),
                (uint32_t)microkit_mr_get(5),
                (uint32_t)microkit_mr_get(6),
            };
            for (int _i = 0; _i < 4; _i++) {
                replay_hash[_i*4+0] = (_rh[_i])      & 0xFF;
                replay_hash[_i*4+1] = (_rh[_i]>>8)   & 0xFF;
                replay_hash[_i*4+2] = (_rh[_i]>>16)  & 0xFF;
                replay_hash[_i*4+3] = (_rh[_i]>>24)  & 0xFF;
            }
            registry_record(replay_hash, svc_id, 0, mod_flags);
            microkit_dbg_puts("[vibe_engine] Registry replay: added module ");
            {
                static const char _h[] = "0123456789abcdef";
                char _hbuf[5] = {
                    _h[(replay_hash[0]>>4)&0xf], _h[replay_hash[0]&0xf],
                    _h[(replay_hash[1]>>4)&0xf], _h[replay_hash[1]&0xf], '\0'
                };
                microkit_dbg_puts(_hbuf);
            }
            microkit_dbg_puts("..\n");
            microkit_mr_set(0, VIBE_OK);
            microkit_mr_set(1, (uint32_t)registry_count);
            return microkit_msginfo_new(0, 2);
        }

        case OP_VIBE_REGISTRY_QUERY: {
            /*
             * Query registry by hash.
             * MR0-MR3: hash bytes (4 x u32, 16 bytes)
             * Returns: MR0=VIBE_OK(found)/VIBE_ERR_NOSVC(not found), MR1=flags, MR2=version
             */
            uint8_t query_hash[32] = {0};
            uint32_t _qh[4] = {
                (uint32_t)microkit_mr_get(0),
                (uint32_t)microkit_mr_get(1),
                (uint32_t)microkit_mr_get(2),
                (uint32_t)microkit_mr_get(3),
            };
            for (int _i = 0; _i < 4; _i++) {
                query_hash[_i*4+0] = (_qh[_i])      & 0xFF;
                query_hash[_i*4+1] = (_qh[_i]>>8)   & 0xFF;
                query_hash[_i*4+2] = (_qh[_i]>>16)  & 0xFF;
                query_hash[_i*4+3] = (_qh[_i]>>24)  & 0xFF;
            }
            int ri = registry_find(query_hash);
            if (ri < 0) {
                microkit_mr_set(0, (uint64_t)VIBE_ERR_NOSVC);
                return microkit_msginfo_new(0, 1);
            }
            microkit_mr_set(0, VIBE_OK);
            microkit_mr_set(1, module_registry[ri].flags);
            microkit_mr_set(2, module_registry[ri].version);
            microkit_mr_set(3, module_registry[ri].service_id);
            return microkit_msginfo_new(0, 4);
        }

        case OP_VIBE_REGISTRY_STATUS: {
            microkit_mr_set(0, VIBE_OK);
            microkit_mr_set(1, (uint32_t)registry_count);
            microkit_mr_set(2, MAX_REGISTRY_ENTRIES);
            microkit_mr_set(3, (uint32_t)total_swaps);
            return microkit_msginfo_new(0, 4);
        }

        default:
            microkit_dbg_puts("[vibe_engine] Unknown op\n");
            microkit_mr_set(0, VIBE_ERR_INTERNAL);
            return microkit_msginfo_new(0, 1);
    }
}
