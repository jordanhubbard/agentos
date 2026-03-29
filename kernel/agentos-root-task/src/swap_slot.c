/*
 * agentOS Swap Slot — Generic Worker Protection Domain
 *
 * A swap slot is a pre-allocated PD that can be loaded with
 * new service code at runtime. This enables the vibe-coding loop
 * without requiring dynamic PD creation (staying in seL4's verified TCB).
 *
 * Lifecycle:
 *   1. Boot: idle, waiting for controller notification
 *   2. Load: controller writes WASM/bytecode to shared memory
 *   3. Init: slot interprets the code and initializes
 *   4. Test: controller runs health checks
 *   5. Active: slot serves as the live service (proxied by controller)
 *   6. Rollback: slot is replaced by a newer version, kept warm
 *   7. Idle: slot is freed and available for reuse
 *
 * Channel:
 *   CH 0: controller (PPC for commands, notifications for lifecycle)
 *
 * Shared memory:
 *   The swap slot has a mapped memory region where the controller
 *   places the service code and configuration.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "wasm3_host.h"

#define CH_CONTROLLER 0

/* Microkit setvar_vaddr: patched to the mapped virtual address of our code region.
 * We declare it so Microkit can patch it; the actual value matches CODE_REGION_BASE. */
uintptr_t swap_code_vaddr;

/* Shared memory layout for code loading */
#define CODE_REGION_BASE  0x2000000UL
#define CODE_REGION_SIZE  0x400000UL   /* 4MB for service code + data */

/* Swap slot header (at start of code region, written by controller) */
typedef struct __attribute__((packed)) {
    uint32_t    magic;          /* 0xVIBE = 0x56494245 */
    uint32_t    version;
    uint32_t    code_format;    /* 0 = idle, 1 = WASM, 2 = bytecode */
    uint32_t    code_offset;    /* Offset from region base to code start */
    uint32_t    code_size;      /* Code size in bytes */
    uint32_t    service_id;     /* Which service this replaces */
    char        service_name[32];
} swap_slot_header_t;

#define SWAP_MAGIC 0x56494245  /* "VIBE" */

/* Slot state */
typedef enum {
    SLOT_IDLE,
    SLOT_LOADING,
    SLOT_READY,
    SLOT_ACTIVE,
    SLOT_FAILED,
} slot_state_t;

static slot_state_t state = SLOT_IDLE;
static uint64_t     requests_served = 0;
static uint64_t     health_checks_passed = 0;
static wasm3_host_t *wasm_host = NULL;  /* Active WASM runtime (if loaded) */

/*
 * Check if the code region has been loaded with new code
 */
static bool check_code_region(void) {
    volatile swap_slot_header_t *hdr = (volatile swap_slot_header_t *)CODE_REGION_BASE;
    return (hdr->magic == SWAP_MAGIC && hdr->code_size > 0);
}

/*
 * Initialize from the loaded code
 */
static bool load_service(void) {
    volatile swap_slot_header_t *hdr = (volatile swap_slot_header_t *)CODE_REGION_BASE;
    
    if (hdr->magic != SWAP_MAGIC) {
        microkit_dbg_puts("[swap_slot] ERROR: bad magic in code region\n");
        return false;
    }
    
    microkit_dbg_puts("[swap_slot] Loading service: ");
    /* Can't use hdr->service_name directly with dbg_puts (volatile) */
    /* TODO: Copy to local buffer and print */
    microkit_dbg_puts("(service)\n");
    
    switch (hdr->code_format) {
        case 1: /* WASM */
            microkit_dbg_puts("[swap_slot] Format: WASM module\n");
            
            /* Destroy any previously loaded runtime */
            if (wasm_host) {
                wasm3_host_destroy(wasm_host);
                wasm_host = NULL;
            }
            
            /* The WASM binary starts after the header */
            const uint8_t *wasm_bytes = (const uint8_t *)(CODE_REGION_BASE + hdr->code_offset);
            uint32_t wasm_size = hdr->code_size;
            
            /* Initialize wasm3 runtime and load the module */
            wasm_host = wasm3_host_init(wasm_bytes, wasm_size);
            if (!wasm_host) {
                microkit_dbg_puts("[swap_slot] ERROR: WASM runtime init failed\n");
                return false;
            }
            
            /* Call the module's init() export */
            if (!wasm3_host_call_init(wasm_host)) {
                microkit_dbg_puts("[swap_slot] WARNING: WASM init() failed or not found\n");
                /* Non-fatal — module may not have init() */
            }
            
            microkit_dbg_puts("[swap_slot] *** WASM module loaded and running ***\n");
            return true;
            
        case 2: /* Bytecode (simple custom format for prototyping) */
            microkit_dbg_puts("[swap_slot] Format: bytecode\n");
            /* Simple stack-based bytecode for early prototyping */
            return false;  /* Not yet implemented */
            
        default:
            microkit_dbg_puts("[swap_slot] ERROR: unknown code format\n");
            return false;
    }
}

void init(void) {
    microkit_dbg_puts("[swap_slot] Swap slot PD initialized (idle)\n");
    state = SLOT_IDLE;
}

void notified(microkit_channel ch) {
    if (ch != CH_CONTROLLER) return;
    
    switch (state) {
        case SLOT_IDLE:
            /* Controller is telling us to load new code */
            microkit_dbg_puts("[swap_slot] Received load notification\n");
            state = SLOT_LOADING;
            
            if (check_code_region()) {
                if (load_service()) {
                    state = SLOT_READY;
                    microkit_dbg_puts("[swap_slot] Service loaded successfully\n");
                    /* Notify controller we're ready */
                    microkit_notify(CH_CONTROLLER);
                } else {
                    state = SLOT_FAILED;
                    microkit_dbg_puts("[swap_slot] Service load FAILED\n");
                    microkit_notify(CH_CONTROLLER);
                }
            } else {
                microkit_dbg_puts("[swap_slot] No code in region yet\n");
                state = SLOT_IDLE;
            }
            break;
            
        case SLOT_READY:
            /* Controller is activating us */
            microkit_dbg_puts("[swap_slot] Activated as live service\n");
            state = SLOT_ACTIVE;
            break;
            
        case SLOT_ACTIVE:
            /* Async event while active */
            break;
            
        default:
            break;
    }
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo) {
    if (ch != CH_CONTROLLER) {
        return microkit_msginfo_new(0xDEAD, 0);
    }
    
    uint64_t label = microkit_msginfo_get_label(msginfo);
    
    switch (label) {
        case MSG_VIBE_SWAP_HEALTH:
            /* Health check — ask the WASM module if it's healthy */
            health_checks_passed++;
            if (state == SLOT_ACTIVE) {
                bool healthy = true;
                if (wasm_host) {
                    healthy = wasm3_host_call_health_check(wasm_host);
                }
                if (healthy) {
                    microkit_mr_set(0, requests_served);
                    microkit_mr_set(1, health_checks_passed);
                    return microkit_msginfo_new(MSG_VIBE_SLOT_HEALTHY, 2);
                } else {
                    return microkit_msginfo_new(MSG_VIBE_SLOT_FAILED, 0);
                }
            } else {
                return microkit_msginfo_new(MSG_VIBE_SLOT_FAILED, 0);
            }
            
        case MSG_VIBE_SWAP_STATUS:
            /* Status query */
            microkit_mr_set(0, (uint64_t)state);
            microkit_mr_set(1, requests_served);
            microkit_mr_set(2, health_checks_passed);
            return microkit_msginfo_new(MSG_VIBE_SWAP_STATUS, 3);
            
        default:
            if (state == SLOT_ACTIVE) {
                requests_served++;
                
                /* Dispatch to WASM module if loaded */
                if (wasm_host) {
                    uint64_t mr0 = microkit_mr_get(0);
                    uint64_t mr1 = microkit_mr_get(1);
                    uint64_t mr2 = microkit_mr_get(2);
                    uint64_t mr3 = microkit_mr_get(3);
                    
                    wasm_ppc_result_t result;
                    if (wasm3_host_call_ppc(wasm_host, label, mr0, mr1, mr2, mr3, &result)) {
                        microkit_mr_set(0, result.mr0);
                        microkit_mr_set(1, result.mr1);
                        microkit_mr_set(2, result.mr2);
                        microkit_mr_set(3, result.mr3);
                        return microkit_msginfo_new(result.label, 4);
                    }
                    /* WASM call failed — fall through to error */
                    microkit_dbg_puts("[swap_slot] WASM handle_ppc() failed\n");
                    return microkit_msginfo_new(MSG_VIBE_SLOT_FAILED, 0);
                }
                
                /* No WASM module — echo back (legacy behavior) */
                microkit_mr_set(0, 0);  /* AOS_OK */
                microkit_mr_set(1, requests_served);
                return microkit_msginfo_new(label, 2);
            }
            
            return microkit_msginfo_new(MSG_VIBE_SLOT_FAILED, 0);
    }
}
