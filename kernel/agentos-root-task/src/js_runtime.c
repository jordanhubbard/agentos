/*
 * agentOS JS Runtime Protection Domain
 *
 * Hosts QuickJS JavaScript execution contexts for agents and services.
 * Provides eval, function call, module loading, and context lifecycle
 * management through a typed PPC interface.
 *
 * Priority: 150  (passive="true" — only executes on PPC)
 * Shmem:    js_staging, 4MB, mapped at runtime via setvar_vaddr (rw)
 *
 * QuickJS integration:
 *   Actual JS engine calls are stubbed.  Every stub site is tagged:
 *     QUICKJS_INTEGRATION_POINT
 *   Wire up QuickJS by implementing the JS_NewRuntime / JS_NewContext /
 *   JS_Eval / JS_Call / JS_FreeContext / JS_FreeRuntime calls at those
 *   sites and linking against quickjs.a (built freestanding, no libc).
 *
 * Opcode dispatch:
 *   OP_JS_EVAL        (0xC0) — evaluate a script, auto-create context if needed
 *   OP_JS_CALL        (0xC1) — call a named global function
 *   OP_JS_LOAD_MODULE (0xC2) — register an ES module in a context
 *   OP_JS_DESTROY     (0xC3) — free a context slot
 *   OP_JS_HEALTH      (0xC4) — liveness check; returns active count + version
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "js_runtime.h"
#include "../vendor/quickjs/quickjs_stub.h"

/* ── Shared staging region (set by Microkit via setvar_vaddr) ────────────── */
uintptr_t js_staging_vaddr;

#define JS_STAGING ((uint8_t *)js_staging_vaddr)

/* ── Module state — all in static BSS (no heap) ──────────────────────────── */
static js_context_t contexts[JS_MAX_CONTEXTS];
static uint32_t     active_context_count = 0;
static JSRuntime   *context_js_rt[JS_MAX_CONTEXTS];
static JSContext   *context_js_ctx[JS_MAX_CONTEXTS];

/* ── Minimal string helpers (no libc) ───────────────────────────────────── */

static uint32_t js_strlen(const char *s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

static void js_strcpy(char *dst, const char *src, uint32_t max) {
    uint32_t i = 0;
    while (i + 1 < max && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* ── Decimal / hex logging helpers ──────────────────────────────────────── */

static void log_dec(uint32_t v) {
    if (v == 0) { console_log(16, 16, "0"); return; }
    char buf[12];
    int  i = 11;
    buf[i] = '\0';
    while (v > 0 && i > 0) { buf[--i] = '0' + (char)(v % 10); v /= 10; }
    console_log(16, 16, &buf[i]);
}

static void log_hex(uint32_t v) {
    static const char hex[] = "0123456789abcdef";
    char buf[11];
    buf[0]  = '0'; buf[1]  = 'x';
    buf[2]  = hex[(v >> 28) & 0xf]; buf[3]  = hex[(v >> 24) & 0xf];
    buf[4]  = hex[(v >> 20) & 0xf]; buf[5]  = hex[(v >> 16) & 0xf];
    buf[6]  = hex[(v >> 12) & 0xf]; buf[7]  = hex[(v >>  8) & 0xf];
    buf[8]  = hex[(v >>  4) & 0xf]; buf[9]  = hex[ v        & 0xf];
    buf[10] = '\0';
    console_log(16, 16, buf);
}

/* ── Context table helpers ───────────────────────────────────────────────── */

/* Returns a free context slot index, or -1 if the table is full. */
static int alloc_context_slot(void) {
    for (int i = 0; i < (int)JS_MAX_CONTEXTS; i++) {
        if (!contexts[i].active)
            return i;
    }
    return -1;
}

/* Find an active context by context_id. Returns pointer or NULL. */
static js_context_t *find_context(uint32_t context_id) {
    if (context_id >= JS_MAX_CONTEXTS)
        return NULL;
    if (!contexts[context_id].active)
        return NULL;
    return &contexts[context_id];
}

/*
 * Allocate and initialise a new context slot.
 * Returns the new js_context_t* on success, NULL if table is full.
 * The caller_pd argument is recorded as the owner.
 */
static js_context_t *create_context(uint32_t caller_pd) {
    int slot = alloc_context_slot();
    if (slot < 0)
        return NULL;

    js_context_t *ctx  = &contexts[slot];
    ctx->context_id    = (uint32_t)slot;
    ctx->active        = true;
    ctx->owner_pd      = caller_pd;
    ctx->eval_count    = 0;
    ctx->last_error[0] = '\0';

    /* QUICKJS_INTEGRATION_POINT — allocate QuickJS runtime + context */
    context_js_rt[slot]  = JS_NewRuntime();
    context_js_ctx[slot] = JS_NewContext(context_js_rt[slot]);

    active_context_count++;

    console_log(16, 16, "[js_runtime] context created: id=");
    log_dec(ctx->context_id);
    console_log(16, 16, " pd=");
    log_dec(caller_pd);
    console_log(16, 16, "\n");

    return ctx;
}

/*
 * Free a context slot and its underlying QuickJS resources.
 */
static void free_context(js_context_t *ctx) {
    console_log(16, 16, "[js_runtime] context destroyed: id=");
    log_dec(ctx->context_id);
    console_log(16, 16, " evals=");
    log_dec((uint32_t)ctx->eval_count);
    console_log(16, 16, "\n");

    /* QUICKJS_INTEGRATION_POINT — release QuickJS context + runtime */
    JS_FreeContext(context_js_ctx[ctx->context_id]);
    JS_FreeRuntime(context_js_rt[ctx->context_id]);
    context_js_ctx[ctx->context_id] = NULL;
    context_js_rt[ctx->context_id]  = NULL;

    ctx->active        = false;
    ctx->context_id    = 0;
    ctx->owner_pd      = 0;
    ctx->eval_count    = 0;
    ctx->last_error[0] = '\0';

    if (active_context_count > 0)
        active_context_count--;
}

/* ── Staging bounds check ────────────────────────────────────────────────── */
static bool staging_range_valid(uint32_t offset, uint32_t len) {
    if (!js_staging_vaddr)        return false;
    if (len == 0)                 return false;
    if (offset >= JS_STAGING_TOTAL) return false;
    if (offset + len > JS_STAGING_TOTAL) return false;
    return true;
}

/*
 * Write a short error string into the staging output area and return its
 * offset / length through out_offset / out_len.  Uses the fixed output base.
 */
static void write_error_to_staging(const char *msg,
                                   uint32_t *out_offset, uint32_t *out_len) {
    uint32_t base = JS_STAGING_OUTPUT_BASE;
    uint32_t max  = JS_STAGING_OUTPUT_SIZE;
    uint8_t *dst  = JS_STAGING + base;
    uint32_t i    = 0;
    while (msg[i] && i < max - 1u) {
        dst[i] = (uint8_t)msg[i];
        i++;
    }
    dst[i] = '\0';
    *out_offset = base;
    *out_len    = i;
}

/* ── OP_JS_EVAL ──────────────────────────────────────────────────────────── */
static microkit_msginfo handle_js_eval(void) {
    uint32_t req_ctx_id   = (uint32_t)microkit_mr_get(1);
    uint32_t script_off   = (uint32_t)microkit_mr_get(2);
    uint32_t script_len   = (uint32_t)microkit_mr_get(3);

    /* Validate staging bounds */
    if (!staging_range_valid(script_off, script_len)) {
        console_log(16, 16, "[js_runtime] EVAL: script out of staging bounds\n");
        uint32_t eoff = 0, elen = 0;
        write_error_to_staging("script offset/len out of staging bounds", &eoff, &elen);
        microkit_mr_set(0, JS_ERR_RUNTIME);
        microkit_mr_set(1, 0xFFFFFFFFu);
        microkit_mr_set(2, eoff);
        microkit_mr_set(3, elen);
        return microkit_msginfo_new(0, 4);
    }

    /* Resolve or auto-create context */
    js_context_t *ctx = NULL;
    if (req_ctx_id == 0xFFu) {
        ctx = create_context(0u /* anonymous caller */);
        if (!ctx) {
            console_log(16, 16, "[js_runtime] EVAL: context table full\n");
            uint32_t eoff = 0, elen = 0;
            write_error_to_staging("context table full", &eoff, &elen);
            microkit_mr_set(0, JS_ERR_NOMEM);
            microkit_mr_set(1, 0xFFFFFFFFu);
            microkit_mr_set(2, eoff);
            microkit_mr_set(3, elen);
            return microkit_msginfo_new(0, 4);
        }
    } else {
        ctx = find_context(req_ctx_id);
        if (!ctx) {
            console_log(16, 16, "[js_runtime] EVAL: context not found id=");
            log_dec(req_ctx_id);
            console_log(16, 16, "\n");
            uint32_t eoff = 0, elen = 0;
            write_error_to_staging("context not found", &eoff, &elen);
            microkit_mr_set(0, JS_ERR_NOTFOUND);
            microkit_mr_set(1, 0xFFFFFFFFu);
            microkit_mr_set(2, eoff);
            microkit_mr_set(3, elen);
            return microkit_msginfo_new(0, 4);
        }
    }

    const char *script = (const char *)(js_staging_vaddr + script_off);
    uint32_t out_offset = JS_STAGING_OUTPUT_BASE;
    uint32_t out_len    = 0;

    console_log(16, 16, "[js_runtime] EVAL: ctx=");
    log_dec(ctx->context_id);
    console_log(16, 16, " script_len=");
    log_dec(script_len);
    console_log(16, 16, "\n");

    /* QUICKJS_INTEGRATION_POINT — evaluate script with QuickJS */
    {
        JSContext *qctx  = context_js_ctx[ctx->context_id];
        JSValue    result = JS_Eval(qctx, script, (size_t)script_len,
                                    "<eval>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(result)) {
            JSValue     exc = JS_GetException(qctx);
            const char *err = JS_ToCString(qctx, exc);
            js_strcpy(ctx->last_error, err, sizeof(ctx->last_error));
            JS_FreeCString(qctx, err);
            JS_FreeValue(qctx, exc);
            JS_FreeValue(qctx, result);
            write_error_to_staging(ctx->last_error, &out_offset, &out_len);
            microkit_mr_set(0, JS_ERR_RUNTIME);
            microkit_mr_set(1, ctx->context_id);
            microkit_mr_set(2, out_offset);
            microkit_mr_set(3, out_len);
            return microkit_msginfo_new(0, 4);
        }
        JSValue     str     = JS_ToString(qctx, result);
        const char *res_str = JS_ToCString(qctx, str);
        out_len = js_strlen(res_str);
        if (out_len >= JS_STAGING_OUTPUT_SIZE)
            out_len = JS_STAGING_OUTPUT_SIZE - 1u;
        uint8_t *dst = JS_STAGING + out_offset;
        for (uint32_t i = 0; i < out_len; i++)
            dst[i] = (uint8_t)res_str[i];
        dst[out_len] = '\0';
        JS_FreeCString(qctx, res_str);
        JS_FreeValue(qctx, str);
        JS_FreeValue(qctx, result);
    }

    ctx->eval_count++;

    microkit_mr_set(0, JS_OK);
    microkit_mr_set(1, ctx->context_id);
    microkit_mr_set(2, out_offset);
    microkit_mr_set(3, out_len);
    return microkit_msginfo_new(0, 4);
}

/* ── OP_JS_CALL ──────────────────────────────────────────────────────────── */
static microkit_msginfo handle_js_call(void) {
    uint32_t ctx_id         = (uint32_t)microkit_mr_get(1);
    uint32_t fname_off      = (uint32_t)microkit_mr_get(2);
    uint32_t fname_len      = (uint32_t)microkit_mr_get(3);
    uint32_t args_off       = (uint32_t)microkit_mr_get(4);
    uint32_t args_len       = (uint32_t)microkit_mr_get(5);

    js_context_t *ctx = find_context(ctx_id);
    if (!ctx) {
        console_log(16, 16, "[js_runtime] CALL: context not found id=");
        log_dec(ctx_id);
        console_log(16, 16, "\n");
        uint32_t eoff = 0, elen = 0;
        write_error_to_staging("context not found", &eoff, &elen);
        microkit_mr_set(0, JS_ERR_NOTFOUND);
        microkit_mr_set(1, eoff);
        microkit_mr_set(2, elen);
        return microkit_msginfo_new(0, 3);
    }

    /* Validate function name bounds */
    if (!staging_range_valid(fname_off, fname_len)) {
        console_log(16, 16, "[js_runtime] CALL: func_name out of staging bounds\n");
        uint32_t eoff = 0, elen = 0;
        write_error_to_staging("func_name offset/len out of staging bounds", &eoff, &elen);
        microkit_mr_set(0, JS_ERR_RUNTIME);
        microkit_mr_set(1, eoff);
        microkit_mr_set(2, elen);
        return microkit_msginfo_new(0, 3);
    }

    /* Validate args bounds (args_len==0 is permitted: no arguments) */
    if (args_len > 0 && !staging_range_valid(args_off, args_len)) {
        console_log(16, 16, "[js_runtime] CALL: args out of staging bounds\n");
        uint32_t eoff = 0, elen = 0;
        write_error_to_staging("args offset/len out of staging bounds", &eoff, &elen);
        microkit_mr_set(0, JS_ERR_RUNTIME);
        microkit_mr_set(1, eoff);
        microkit_mr_set(2, elen);
        return microkit_msginfo_new(0, 3);
    }

    const char *fname = (const char *)(js_staging_vaddr + fname_off);
    /* args pointer (may be NULL/unused when args_len==0) */
    const char *args  = args_len > 0
                        ? (const char *)(js_staging_vaddr + args_off)
                        : NULL;

    console_log(16, 16, "[js_runtime] CALL: ctx=");
    log_dec(ctx_id);
    console_log(16, 16, " func_len=");
    log_dec(fname_len);
    console_log(16, 16, " args_len=");
    log_dec(args_len);
    console_log(16, 16, "\n");

    uint32_t out_offset = JS_STAGING_OUTPUT_BASE;
    uint32_t out_len    = 0;

    /* QUICKJS_INTEGRATION_POINT — call named function with QuickJS */
    {
        JSContext *qctx   = context_js_ctx[ctx->context_id];
        JSValue    global = JS_GetGlobalObject(qctx);
        char       fname_buf[256];
        uint32_t   copy_len = fname_len < 255u ? fname_len : 255u;
        js_strcpy(fname_buf, fname, copy_len + 1u);
        fname_buf[copy_len] = '\0';
        JSValue func = JS_GetPropertyStr(qctx, global, fname_buf);
        JSValue ret  = JS_Call(qctx, func, JS_UNDEFINED, 0, NULL);
        (void)args;
        if (JS_IsException(ret)) {
            JSValue     exc = JS_GetException(qctx);
            const char *err = JS_ToCString(qctx, exc);
            js_strcpy(ctx->last_error, err, sizeof(ctx->last_error));
            JS_FreeCString(qctx, err);
            JS_FreeValue(qctx, exc);
            JS_FreeValue(qctx, ret);
            JS_FreeValue(qctx, func);
            JS_FreeValue(qctx, global);
            write_error_to_staging(ctx->last_error, &out_offset, &out_len);
            microkit_mr_set(0, JS_ERR_RUNTIME);
            microkit_mr_set(1, out_offset);
            microkit_mr_set(2, out_len);
            return microkit_msginfo_new(0, 3);
        }
        JSValue     str = JS_ToString(qctx, ret);
        const char *res = JS_ToCString(qctx, str);
        out_len = js_strlen(res);
        if (out_len >= JS_STAGING_OUTPUT_SIZE)
            out_len = JS_STAGING_OUTPUT_SIZE - 1u;
        uint8_t *dst = JS_STAGING + out_offset;
        for (uint32_t i = 0; i < out_len; i++)
            dst[i] = (uint8_t)res[i];
        dst[out_len] = '\0';
        JS_FreeCString(qctx, res);
        JS_FreeValue(qctx, str);
        JS_FreeValue(qctx, ret);
        JS_FreeValue(qctx, func);
        JS_FreeValue(qctx, global);
    }

    microkit_mr_set(0, JS_OK);
    microkit_mr_set(1, out_offset);
    microkit_mr_set(2, out_len);
    return microkit_msginfo_new(0, 3);
}

/* ── OP_JS_LOAD_MODULE ───────────────────────────────────────────────────── */
static microkit_msginfo handle_js_load_module(void) {
    uint32_t req_ctx_id   = (uint32_t)microkit_mr_get(1);
    uint32_t src_off      = (uint32_t)microkit_mr_get(2);
    uint32_t src_len      = (uint32_t)microkit_mr_get(3);
    uint32_t name_off     = (uint32_t)microkit_mr_get(4);
    uint32_t name_len     = (uint32_t)microkit_mr_get(5);

    /* Validate staging bounds */
    if (!staging_range_valid(src_off, src_len)
            || !staging_range_valid(name_off, name_len)) {
        console_log(16, 16, "[js_runtime] LOAD_MODULE: invalid staging offsets\n");
        microkit_mr_set(0, JS_ERR_RUNTIME);
        microkit_mr_set(1, 0xFFFFFFFFu);
        return microkit_msginfo_new(0, 2);
    }

    /* Resolve or auto-create context */
    js_context_t *ctx = NULL;
    if (req_ctx_id == 0xFFu) {
        ctx = create_context(0u);
        if (!ctx) {
            console_log(16, 16, "[js_runtime] LOAD_MODULE: context table full\n");
            microkit_mr_set(0, JS_ERR_NOMEM);
            microkit_mr_set(1, 0xFFFFFFFFu);
            return microkit_msginfo_new(0, 2);
        }
    } else {
        ctx = find_context(req_ctx_id);
        if (!ctx) {
            console_log(16, 16, "[js_runtime] LOAD_MODULE: context not found id=");
            log_dec(req_ctx_id);
            console_log(16, 16, "\n");
            microkit_mr_set(0, JS_ERR_NOTFOUND);
            microkit_mr_set(1, 0xFFFFFFFFu);
            return microkit_msginfo_new(0, 2);
        }
    }

    const char *src  = (const char *)(js_staging_vaddr + src_off);
    const char *name = (const char *)(js_staging_vaddr + name_off);

    console_log(16, 16, "[js_runtime] LOAD_MODULE: ctx=");
    log_dec(ctx->context_id);
    console_log(16, 16, " src_len=");
    log_dec(src_len);
    console_log(16, 16, " name_len=");
    log_dec(name_len);
    console_log(16, 16, "\n");

    /* QUICKJS_INTEGRATION_POINT — register ES module with QuickJS */
    {
        JSContext *qctx     = context_js_ctx[ctx->context_id];
        char       name_buf[256];
        uint32_t   copy_len = name_len < 255u ? name_len : 255u;
        js_strcpy(name_buf, name, copy_len + 1u);
        name_buf[copy_len] = '\0';
        JSValue mod = JS_Eval(qctx, src, (size_t)src_len, name_buf,
                              JS_EVAL_TYPE_MODULE);
        if (JS_IsException(mod)) {
            JSValue     exc = JS_GetException(qctx);
            const char *err = JS_ToCString(qctx, exc);
            js_strcpy(ctx->last_error, err, sizeof(ctx->last_error));
            JS_FreeCString(qctx, err);
            JS_FreeValue(qctx, exc);
            JS_FreeValue(qctx, mod);
            microkit_mr_set(0, JS_ERR_SYNTAX);
            microkit_mr_set(1, ctx->context_id);
            return microkit_msginfo_new(0, 2);
        }
        JS_FreeValue(qctx, mod);
    }

    microkit_mr_set(0, JS_OK);
    microkit_mr_set(1, ctx->context_id);
    return microkit_msginfo_new(0, 2);
}

/* ── OP_JS_DESTROY ───────────────────────────────────────────────────────── */
static microkit_msginfo handle_js_destroy(void) {
    uint32_t ctx_id = (uint32_t)microkit_mr_get(1);

    js_context_t *ctx = find_context(ctx_id);
    if (!ctx) {
        console_log(16, 16, "[js_runtime] DESTROY: context not found id=");
        log_dec(ctx_id);
        console_log(16, 16, "\n");
        microkit_mr_set(0, JS_ERR_NOTFOUND);
        return microkit_msginfo_new(0, 1);
    }

    free_context(ctx);

    microkit_mr_set(0, JS_OK);
    return microkit_msginfo_new(0, 1);
}

/* ── OP_JS_HEALTH ────────────────────────────────────────────────────────── */
static microkit_msginfo handle_js_health(void) {
    microkit_mr_set(0, JS_OK);
    microkit_mr_set(1, active_context_count);
    microkit_mr_set(2, JS_QUICKJS_VERSION);
    return microkit_msginfo_new(0, 3);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Microkit entry points
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * init() — called once at boot before any PPCs arrive.
 *
 * Zeroes the context table and logs the staging region address.
 * No QuickJS global runtime is allocated here; each context gets its own
 * JS_NewRuntime() at create_context() time (see QUICKJS_INTEGRATION_POINT
 * inside create_context()).
 */
void init(void) {
    agentos_log_boot("js_runtime");
    console_log(16, 16, "[js_runtime] Initialising JS Runtime PD (priority 150, passive)\n");

    /* Zero context table */
    for (int i = 0; i < (int)JS_MAX_CONTEXTS; i++) {
        contexts[i].context_id    = 0;
        contexts[i].active        = false;
        contexts[i].owner_pd      = 0;
        contexts[i].eval_count    = 0;
        contexts[i].last_error[0] = '\0';
        context_js_rt[i]          = NULL;
        context_js_ctx[i]         = NULL;
    }
    active_context_count = 0;

    console_log(16, 16, "[js_runtime] context table cleared: max_contexts=");
    log_dec(JS_MAX_CONTEXTS);
    console_log(16, 16, "\n");

    console_log(16, 16, "[js_runtime] staging region: 4MB at ");
    log_hex((uint32_t)js_staging_vaddr);
    console_log(16, 16, "\n");

    console_log(16, 16, "[js_runtime] QuickJS version: ");
    log_hex(JS_QUICKJS_VERSION);
    console_log(16, 16, " (stub — replace vendor/quickjs/quickjs_stub.c for production)\n");

    console_log(16, 16, "[js_runtime] READY — accepting JS eval/call requests\n");
}

/*
 * notified() — called when js_runtime receives an asynchronous notification.
 *
 * JS_CH_TIMER (ch 11): periodic 10ms tick from controller.
 *   Used to advance any per-context eval timeout counters.
 *
 * QUICKJS_INTEGRATION_POINT: if QuickJS exposes a JS_ExecutePendingJob()
 * interface, call it here on each active context so that promise microtasks
 * and async jobs are flushed between synchronous PPCs.
 */
void notified(microkit_channel ch) {
    switch (ch) {
    case JS_CH_TIMER:
        /* QUICKJS_INTEGRATION_POINT: flush pending microtasks on each active context */
        for (int i = 0; i < (int)JS_MAX_CONTEXTS; i++) {
            if (!contexts[i].active || !context_js_ctx[i])
                continue;
            (void)context_js_ctx[i];
        }
        break;
    default:
        console_log(16, 16, "[js_runtime] unexpected notify ch=");
        log_dec((uint32_t)ch);
        console_log(16, 16, "\n");
        break;
    }
}

/*
 * protected() — PPC handler; dispatches all JS Runtime IPC operations.
 *
 * Callers: controller (JS_CH_CONTROLLER=0), init_agent (JS_CH_INIT_AGENT=1).
 * The opcode is carried in MR0 (not in the msginfo label), matching the
 * pattern established by vibe_engine.c.
 */
microkit_msginfo protected(microkit_channel ch, microkit_msginfo msginfo) {
    (void)ch;      /* all channels share the same dispatch table */
    (void)msginfo; /* op code is in MR0, not the label */

    uint32_t op = (uint32_t)microkit_mr_get(0);

    switch (op) {
    case OP_JS_EVAL:        return handle_js_eval();
    case OP_JS_CALL:        return handle_js_call();
    case OP_JS_LOAD_MODULE: return handle_js_load_module();
    case OP_JS_DESTROY:     return handle_js_destroy();
    case OP_JS_HEALTH:      return handle_js_health();
    default:
        console_log(16, 16, "[js_runtime] unknown op=");
        log_hex(op);
        console_log(16, 16, " ch=");
        log_dec((uint32_t)ch);
        console_log(16, 16, "\n");
        microkit_mr_set(0, JS_ERR_RUNTIME);
        return microkit_msginfo_new(0, 1);
    }
}
