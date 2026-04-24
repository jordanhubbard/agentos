/*
 * minimal_service.h — hand-crafted minimal WASM service module fixture
 *
 * A valid agentOS service WASM binary containing:
 *   - WASM magic + version 1
 *   - Type section: one function type () -> ()
 *   - Function section: 4 functions (init, handle_ppc, health_check, notified)
 *   - Memory section: 1 linear memory page (required by validator)
 *   - Export section: init, handle_ppc, health_check, notified (functions),
 *                     memory (linear memory) — all exports required by the
 *                     vibe-engine validator (VSWAP_VAL_EXPORTS | VSWAP_VAL_MEMORY)
 *   - Code section: 4 minimal no-op function bodies
 *   - Custom section: "agentos.capabilities" with version byte
 *                     (required for VSWAP_VAL_CAPS_SECT)
 *
 * All required validation checks (VSWAP_VAL_REQUIRED) pass on this binary.
 * Use as the WASM payload in E13 pipeline tests and agent boot integration tests.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * Minimal WASM binary (123 bytes).
 *
 * Section layout:
 *  [0x00]  magic + version (8 bytes)
 *  [0x08]  type section     (6 bytes)  — 1 func type: () -> ()
 *  [0x0E]  function section (7 bytes)  — 4 functions, all type 0
 *  [0x15]  memory section   (5 bytes)  — 1 page min, no max
 *  [0x1A]  export section   (58 bytes) — init, handle_ppc, health_check,
 *                                        notified, memory
 *  [0x54]  code section     (15 bytes) — 4 empty bodies
 *  [0x63]  custom section   (24 bytes) — "agentos.capabilities"
 */
static const uint8_t g_minimal_wasm[] = {
    /* ── Header ──────────────────────────────────────── */
    0x00, 0x61, 0x73, 0x6D,   /* magic: \0asm                */
    0x01, 0x00, 0x00, 0x00,   /* version: 1                  */

    /* ── Type section (id=1, size=4) ─────────────────── */
    0x01, 0x04,               /* id=1, size=4                */
    0x01,                     /* 1 type                      */
    0x60, 0x00, 0x00,         /* type 0: () -> ()            */

    /* ── Function section (id=3, size=5) ─────────────── */
    0x03, 0x05,               /* id=3, size=5                */
    0x04,                     /* 4 functions                 */
    0x00, 0x00, 0x00, 0x00,   /* all → type 0                */

    /* ── Memory section (id=5, size=3) ───────────────── */
    0x05, 0x03,               /* id=5, size=3                */
    0x01,                     /* 1 memory                    */
    0x00, 0x01,               /* limits: no max, min=1       */

    /* ── Export section (id=7, size=56) ──────────────── */
    0x07, 0x38,               /* id=7, size=56               */
    0x05,                     /* 5 exports                   */
    /* "init":function[0] */
    0x04, 0x69, 0x6E, 0x69, 0x74, 0x00, 0x00,
    /* "handle_ppc":function[1] */
    0x0A, 0x68, 0x61, 0x6E, 0x64, 0x6C, 0x65, 0x5F, 0x70, 0x70, 0x63, 0x00, 0x01,
    /* "health_check":function[2] */
    0x0C, 0x68, 0x65, 0x61, 0x6C, 0x74, 0x68, 0x5F, 0x63, 0x68, 0x65, 0x63, 0x6B, 0x00, 0x02,
    /* "notified":function[3] */
    0x08, 0x6E, 0x6F, 0x74, 0x69, 0x66, 0x69, 0x65, 0x64, 0x00, 0x03,
    /* "memory":memory[0] */
    0x06, 0x6D, 0x65, 0x6D, 0x6F, 0x72, 0x79, 0x02, 0x00,

    /* ── Code section (id=10, size=13) ───────────────── */
    0x0A, 0x0D,               /* id=10, size=13              */
    0x04,                     /* 4 function bodies           */
    0x02, 0x00, 0x0B,         /* body 0: size=2, 0 locals, end */
    0x02, 0x00, 0x0B,         /* body 1                      */
    0x02, 0x00, 0x0B,         /* body 2                      */
    0x02, 0x00, 0x0B,         /* body 3                      */

    /* ── Custom section "agentos.capabilities" ────────── */
    0x00, 0x16,               /* id=0 (custom), size=22      */
    0x14,                     /* name length: 20             */
    /* "agentos.capabilities" */
    0x61, 0x67, 0x65, 0x6E, 0x74, 0x6F, 0x73, 0x2E,
    0x63, 0x61, 0x70, 0x61, 0x62, 0x69, 0x6C, 0x69,
    0x74, 0x69, 0x65, 0x73,
    0x01,                     /* capability payload version  */
};

static const uint32_t g_minimal_wasm_len =
    (uint32_t)sizeof(g_minimal_wasm);   /* 123 bytes */
