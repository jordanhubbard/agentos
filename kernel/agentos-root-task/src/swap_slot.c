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
#include "sel4_server.h"
#include "contracts/swap_slot_contract.h"
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
        log_drain_write(15, 15, "[swap_slot] ERROR: bad magic in code region\n");
        return false;
    }
    
    log_drain_write(15, 15, "[swap_slot] Loading service: ");
    /* Can't use hdr->service_name directly with dbg_puts (volatile) */
    /* TODO: Copy to local buffer and print */
    log_drain_write(15, 15, "(service)\n");
    
    switch (hdr->code_format) {
        case 1: /* WASM */
            log_drain_write(15, 15, "[swap_slot] Format: WASM module\n");
            
            /* Destroy any previously loaded runtime */
            if (wasm_host) {
                wasm3_host_destroy(wasm_host);
                wasm_host = NULL;
            }

            /* Reset the wasm3 fixed-heap bump allocator.  The PD may have
             * been restarted by the seL4 fault handler (which does NOT re-zero
             * BSS), so the heap pointer could be anywhere from the previous
             * failed attempt.  Resetting here guarantees a clean slate. */
            wasm3_heap_reset();

            /* The WASM binary starts after the header */
            const uint8_t *wasm_bytes = (const uint8_t *)(CODE_REGION_BASE + hdr->code_offset);
            uint32_t wasm_size = hdr->code_size;

            /* Quick sanity-check: log first 4 bytes so we can see whether the
             * region contains a real WASM module ("\0asm") or garbage/zeros. */
            {
                char _cl_buf[80] = {};
                char *_cl_p = _cl_buf;
                for (const char *_s = "[swap_slot] WASM header bytes: "; *_s; _s++) *_cl_p++ = *_s;
                const char hex[] = "0123456789abcdef";
                uint32_t n = (wasm_size < 8) ? wasm_size : 8;
                for (uint32_t i = 0; i < n; i++) {
                    *_cl_p++ = hex[(wasm_bytes[i] >> 4) & 0xf];
                    *_cl_p++ = hex[wasm_bytes[i] & 0xf];
                    *_cl_p++ = ' ';
                }
                for (const char *_s = "\n"; *_s; _s++) *_cl_p++ = *_s;
                *_cl_p = 0;
                log_drain_write(15, 15, _cl_buf);
            }

            /* Initialize wasm3 runtime and load the module */
            wasm_host = wasm3_host_init(wasm_bytes, wasm_size);
            if (!wasm_host) {
                log_drain_write(15, 15, "[swap_slot] ERROR: WASM runtime init failed\n");
                return false;
            }
            
            /* Call the module's init() export */
            if (!wasm3_host_call_init(wasm_host)) {
                log_drain_write(15, 15, "[swap_slot] WARNING: WASM init() failed or not found\n");
                /* Non-fatal — module may not have init() */
            }
            
            log_drain_write(15, 15, "[swap_slot] *** WASM module loaded and running ***\n");
            return true;
            
        case 2: /* Bytecode (simple custom format for prototyping) */
            log_drain_write(15, 15, "[swap_slot] Format: bytecode\n");
            /* Simple stack-based bytecode for early prototyping */
            return false;  /* Not yet implemented */
            
        default:
            log_drain_write(15, 15, "[swap_slot] ERROR: unknown code format\n");
            return false;
    }
}

static void swap_slot_pd_init(void) {
    log_drain_write(15, 15, "[swap_slot] Swap slot PD initialized (idle)\n");
    state = SLOT_IDLE;
}

static void swap_slot_pd_notified(uint32_t ch) {
    if (ch != CH_CONTROLLER) return;
    
    switch (state) {
        case SLOT_IDLE:
            /* Controller is telling us to load new code */
            log_drain_write(15, 15, "[swap_slot] Received load notification\n");
            state = SLOT_LOADING;
            
            if (check_code_region()) {
                if (load_service()) {
                    state = SLOT_READY;
                    log_drain_write(15, 15, "[swap_slot] Service loaded successfully\n");
                    /* Notify controller we're ready */
                    sel4_dbg_puts("[E5-S8] notify-stub
");
                } else {
                    state = SLOT_FAILED;
                    log_drain_write(15, 15, "[swap_slot] Service load FAILED\n");
                    sel4_dbg_puts("[E5-S8] notify-stub
");
                }
            } else {
                log_drain_write(15, 15, "[swap_slot] No code in region yet\n");
                state = SLOT_IDLE;
            }
            break;
            
        case SLOT_READY:
            /* Controller is activating us */
            log_drain_write(15, 15, "[swap_slot] Activated as live service\n");
            state = SLOT_ACTIVE;
            break;
            
        case SLOT_ACTIVE:
            /* Async event while active */
            break;
            
        default:
            break;
    }
}

static uint32_t swap_slot_h_dispatch(sel4_badge_t b, const sel4_msg_t *req, sel4_msg_t *rep, void *ctx) {
    if (ch != CH_CONTROLLER) {
        rep->length = 0;
        return SEL4_ERR_OK;
    }
    
    uint64_t label = msg_u32(req, 0);
    
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
                    rep_u32(rep, 0, requests_served);
                    rep_u32(rep, 4, health_checks_passed);
                    rep->length = 8;
        return SEL4_ERR_OK;
                } else {
                    rep->length = 0;
        return SEL4_ERR_OK;
                }
            } else {
                rep->length = 0;
        return SEL4_ERR_OK;
            }
            
        case MSG_VIBE_SWAP_STATUS:
            /* Status query */
            rep_u32(rep, 0, (uint64_t)state);
            rep_u32(rep, 4, requests_served);
            rep_u32(rep, 8, health_checks_passed);
            rep->length = 12;
        return SEL4_ERR_OK;
            
        default:
            if (state == SLOT_ACTIVE) {
                requests_served++;
                
                /* Dispatch to WASM module if loaded */
                if (wasm_host) {
                    uint64_t mr0 = msg_u32(req, 0);
                    uint64_t mr1 = msg_u32(req, 4);
                    uint64_t mr2 = msg_u32(req, 8);
                    uint64_t mr3 = msg_u32(req, 12);
                    
                    wasm_ppc_result_t result;
                    if (wasm3_host_call_ppc(wasm_host, label, mr0, mr1, mr2, mr3, &result)) {
                        rep_u32(rep, 0, result.mr0);
                        rep_u32(rep, 4, result.mr1);
                        rep_u32(rep, 8, result.mr2);
                        rep_u32(rep, 12, result.mr3);
                        rep->length = 16;
        return SEL4_ERR_OK;
                    }
                    /* WASM call failed — fall through to error */
                    log_drain_write(15, 15, "[swap_slot] WASM handle_ppc() failed\n");
                    rep->length = 0;
        return SEL4_ERR_OK;
                }
                
                /* No WASM module — echo back (legacy behavior) */
                rep_u32(rep, 0, 0);  /* AOS_OK */
                rep_u32(rep, 4, requests_served);
                rep->length = 8;
        return SEL4_ERR_OK;
            }
            
            rep->length = 0;
        return SEL4_ERR_OK;
    }
}

/* ── E5-S8: Entry point ─────────────────────────────────────────────────── */
void swap_slot_main(seL4_CPtr my_ep, seL4_CPtr ns_ep)
{
    (void)ns_ep;
    swap_slot_pd_init();
    static sel4_server_t srv;
    sel4_server_init(&srv, my_ep);
    /* Dispatch all opcodes through the generic handler */
    sel4_server_register(&srv, SEL4_SERVER_OPCODE_ANY, swap_slot_h_dispatch, (void *)0);
    sel4_server_run(&srv);
}
