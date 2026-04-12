/*
 * agentOS JS Runtime — Public Interface
 *
 * js_runtime is a passive PD (priority 150) that hosts QuickJS JavaScript
 * execution contexts for agents and services running under agentOS.
 *
 * Architecture:
 *   - Up to JS_MAX_CONTEXTS (8) concurrent JS execution contexts
 *   - Each context is independently sandboxed within the QuickJS runtime
 *   - Script source and output travel through js_staging (shared 4MB region)
 *   - Contexts may be auto-created on first OP_JS_EVAL with context_id=0xFF
 *
 * QuickJS integration:
 *   The JS engine calls are stubbed.  Every integration point is marked with:
 *     QUICKJS_INTEGRATION_POINT
 *   To wire up QuickJS, implement the JS_Eval / JS_Call callbacks at those
 *   marked sites and link against the QuickJS static library (quickjs.a).
 *
 * Shared memory layout (js_staging, 4MB = 0x400000):
 *   js_staging is a flat staging region used for both input and output.
 *   script_offset / output_offset are byte offsets into this region.
 *   Callers write script source before invoking OP_JS_EVAL; js_runtime
 *   writes result text at the output_offset it reports in the reply MRs.
 *
 *   Recommended sub-region layout (not enforced by hardware, by convention):
 *     [0x000000 .. 0x1FFFFF]  Script / module source input (2MB)
 *     [0x200000 .. 0x3FFFFF]  Evaluation output / function call results (2MB)
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── Version ─────────────────────────────────────────────────────────────── */
#define JS_RUNTIME_VERSION      1u

/* Encoded QuickJS version: 2.3.3 → 0x20303 */
#define JS_QUICKJS_VERSION      0x20303u

/* ── Limits ──────────────────────────────────────────────────────────────── */
#define JS_MAX_CONTEXTS         8u    /* hard limit on concurrent JS contexts */

/* ── shmem staging geometry ──────────────────────────────────────────────── */
#define JS_STAGING_TOTAL        0x400000u         /* 4MB staging region */
#define JS_STAGING_INPUT_BASE   0x000000u         /* script/module input area */
#define JS_STAGING_INPUT_SIZE   0x200000u         /* 2MB for input */
#define JS_STAGING_OUTPUT_BASE  0x200000u         /* evaluation output area */
#define JS_STAGING_OUTPUT_SIZE  0x200000u         /* 2MB for output */

/* ── Channel IDs from js_runtime's own perspective ───────────────────────── */
#define JS_CH_CONTROLLER        0u   /* pp=true inbound from controller */
#define JS_CH_INIT_AGENT        1u   /* pp=true inbound from init_agent */
#define JS_CH_TIMER             11u  /* 10ms periodic tick from controller */

/* ── IPC Opcodes ─────────────────────────────────────────────────────────── */

/*
 * OP_JS_EVAL (0xC0) — evaluate a JavaScript script in a context
 *
 * Inputs (MRs set by caller):
 *   MR0 = OP_JS_EVAL (0xC0)
 *   MR1 = context_id  (0xFF = auto-create a new context)
 *   MR2 = script_offset  (byte offset into js_staging for script source)
 *   MR3 = script_len     (length of script source in bytes, excluding NUL)
 *
 * Script source must be placed in the js_staging shared region at script_offset
 * before invoking this call.  The source need not be NUL-terminated; js_runtime
 * uses script_len to delimit it.
 *
 * Outputs (MRs written by js_runtime):
 *   MR0 = result (JS_OK or JS_ERR_*)
 *   MR1 = context_id  (assigned or echoed; valid on JS_OK)
 *   MR2 = output_offset  (byte offset into js_staging where result text begins)
 *   MR3 = output_len     (length of result text in bytes)
 *
 * On error (MR0 != JS_OK), MR2/MR3 describe the error string written to staging.
 * The result text is the JSON-stringified return value of the script's last
 * expression (QUICKJS_INTEGRATION_POINT: JS_ToCString on the eval result).
 */
#define OP_JS_EVAL              0xC0u

/*
 * OP_JS_CALL (0xC1) — call a named function in an existing context
 *
 * Inputs (MRs set by caller):
 *   MR0 = OP_JS_CALL (0xC1)
 *   MR1 = context_id       (must refer to an active context)
 *   MR2 = func_name_offset (byte offset into js_staging for function name)
 *   MR3 = func_name_len    (length of function name string, excluding NUL)
 *   MR4 = args_offset      (byte offset into js_staging for JSON-encoded args array)
 *   MR5 = args_len         (length of JSON args string; 0 = no args / empty array)
 *
 * The function name and JSON arguments array must be placed in js_staging before
 * this call.  Args are decoded as a JSON array: first element → first argument.
 * Pass args_len=0 to call with no arguments.
 *
 * Outputs (MRs written by js_runtime):
 *   MR0 = result (JS_OK or JS_ERR_*)
 *   MR1 = output_offset  (byte offset into js_staging where return value begins)
 *   MR2 = output_len     (length of JSON-encoded return value in bytes)
 *
 * QUICKJS_INTEGRATION_POINT: JS_GetGlobalObject → JS_GetPropertyStr → JS_Call.
 */
#define OP_JS_CALL              0xC1u

/*
 * OP_JS_LOAD_MODULE (0xC2) — load an ES module source into a context
 *
 * Inputs (MRs set by caller):
 *   MR0 = OP_JS_LOAD_MODULE (0xC2)
 *   MR1 = context_id        (must refer to an active context, or 0xFF to auto-create)
 *   MR2 = module_src_offset (byte offset into js_staging for ES module source)
 *   MR3 = module_src_len    (length of module source in bytes)
 *   MR4 = module_name_offset (byte offset into js_staging for module name string)
 *   MR5 = module_name_len   (length of module name, excluding NUL)
 *
 * Registers the module in the context's module registry; it becomes importable
 * from subsequently evaluated scripts via:  import { ... } from '<module_name>';
 *
 * Outputs (MRs written by js_runtime):
 *   MR0 = result (JS_OK or JS_ERR_*)
 *   MR1 = context_id  (assigned or echoed)
 *
 * QUICKJS_INTEGRATION_POINT: JS_Eval with JS_EVAL_TYPE_MODULE flag.
 */
#define OP_JS_LOAD_MODULE       0xC2u

/*
 * OP_JS_DESTROY (0xC3) — destroy a JS context and free its slot
 *
 * Inputs (MRs set by caller):
 *   MR0 = OP_JS_DESTROY (0xC3)
 *   MR1 = context_id  (must refer to an active context)
 *
 * Outputs (MRs written by js_runtime):
 *   MR0 = result (JS_OK or JS_ERR_NOTFOUND)
 *
 * QUICKJS_INTEGRATION_POINT: JS_FreeContext / JS_FreeRuntime.
 */
#define OP_JS_DESTROY           0xC3u

/*
 * OP_JS_HEALTH (0xC4) — liveness and capability check
 *
 * Inputs (MRs set by caller):
 *   MR0 = OP_JS_HEALTH (0xC4)
 *
 * Outputs (MRs written by js_runtime):
 *   MR0 = JS_OK (0)
 *   MR1 = active_context_count   (number of contexts currently allocated)
 *   MR2 = JS_QUICKJS_VERSION     (0x20303 = v2.3.3)
 */
#define OP_JS_HEALTH            0xC4u

/* ── Result codes (MR0 in replies) ──────────────────────────────────────── */
#define JS_OK                   0u   /* success */
#define JS_ERR_NOMEM            1u   /* context table full or allocation failure */
#define JS_ERR_SYNTAX           2u   /* script parse / syntax error */
#define JS_ERR_RUNTIME          3u   /* runtime exception during evaluation */
#define JS_ERR_NOTFOUND         4u   /* context_id not active, or function not found */

/* ── JS context table entry ──────────────────────────────────────────────── */
/*
 * Stored statically in js_runtime's BSS.  Not visible to callers;
 * exposed here for documentation and test-harness access.
 *
 * The opaque js_qctx pointer is set at the QUICKJS_INTEGRATION_POINT when
 * a real JSContext* is allocated.  In stub mode it is always NULL.
 */
typedef struct {
    uint32_t  context_id;       /* Unique context identifier (index in table) */
    bool      active;           /* true = slot in use */
    uint32_t  owner_pd;         /* PD badge / caller that created the context */
    uint64_t  eval_count;       /* number of successful OP_JS_EVAL calls */
    char      last_error[128];  /* NUL-terminated last error string, or "" */
} js_context_t;                 /* ~152 bytes */
