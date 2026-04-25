/*
 * fdt_builder.c — minimal Flattened Device Tree serializer
 *
 * Emits a standards-compliant DTB (Device Tree Blob) into a caller-supplied
 * buffer.  No dynamic allocation.  Big-endian wire format as required by
 * the DTB/FDT specification (v17).
 *
 * Layout in the output buffer:
 *   [0]             fdt_header_t (40 bytes)
 *   [40]            memory reservation map: one empty entry (16 bytes)
 *   [56]            struct section (FDT_TOKEN_* tokens and data)
 *   [56 + struct]   string table (property name strings)
 *
 * All integers in the struct section are big-endian uint32_t.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "fdt_builder.h"

/* ── Big-endian helpers ──────────────────────────────────────────────────── */

static void wr_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >>  8);
    p[3] = (uint8_t)(v);
}

static void wr_be64(uint8_t *p, uint64_t v)
{
    wr_be32(p,     (uint32_t)(v >> 32));
    wr_be32(p + 4, (uint32_t)(v & 0xFFFFFFFFU));
}

/* ── Internal emit helpers ───────────────────────────────────────────────── */

/* emit_u32 — append one big-endian uint32_t to the struct section */
static void emit_u32(fdt_ctx_t *ctx, uint32_t v)
{
    if (ctx->error) return;
    size_t pos = ctx->struct_start + ctx->struct_off;
    if (pos + 4u > ctx->str_start) {
        ctx->error = -1;
        return;
    }
    wr_be32(ctx->buf + pos, v);
    ctx->struct_off += 4u;
}

/* emit_bytes — append raw bytes (no padding) */
static void emit_bytes(fdt_ctx_t *ctx, const uint8_t *data, size_t len)
{
    if (ctx->error || len == 0u) return;
    size_t pos = ctx->struct_start + ctx->struct_off;
    if (pos + len > ctx->str_start) {
        ctx->error = -1;
        return;
    }
    for (size_t i = 0u; i < len; i++) {
        ctx->buf[pos + i] = data[i];
    }
    ctx->struct_off += len;
}

/* emit_str — append NUL-terminated string including the NUL byte */
static void emit_str(fdt_ctx_t *ctx, const char *s)
{
    if (ctx->error || !s) return;
    for (; *s; s++) {
        emit_bytes(ctx, (const uint8_t *)s, 1u);
    }
    uint8_t nul = 0u;
    emit_bytes(ctx, &nul, 1u);
}

/* emit_align4 — pad struct section to a 4-byte boundary with zeros */
static void emit_align4(fdt_ctx_t *ctx)
{
    while (!ctx->error && (ctx->struct_off & 3u)) {
        uint8_t z = 0u;
        emit_bytes(ctx, &z, 1u);
    }
}

/* ── String table helpers ─────────────────────────────────────────────────── */

/*
 * str_intern — return the offset within the string table of a NUL-terminated
 * string, adding it if not already present.
 */
static uint32_t str_intern(fdt_ctx_t *ctx, const char *s)
{
    if (!s || ctx->error) return 0u;

    /* Search for existing entry */
    size_t str_base = ctx->str_start;
    size_t str_used = ctx->str_off;
    const char *p   = (const char *)(ctx->buf + str_base);
    size_t i = 0u;
    while (i < str_used) {
        const char *entry = p + i;
        /* Compare s with this entry */
        size_t j = 0u;
        while (entry[j] != '\0' && s[j] != '\0' && entry[j] == s[j]) {
            j++;
        }
        if (entry[j] == '\0' && s[j] == '\0') {
            return (uint32_t)i;
        }
        /* Advance past this string */
        while (i < str_used && ctx->buf[str_base + i] != '\0') {
            i++;
        }
        i++;  /* skip NUL */
    }

    /* Not found: append */
    uint32_t off = (uint32_t)str_used;
    size_t pos = str_base + str_used;
    for (; *s; s++, pos++) {
        if (pos >= ctx->cap) {
            ctx->error = -2;
            return 0u;
        }
        ctx->buf[pos] = (uint8_t)*s;
        ctx->str_off++;
    }
    if (pos >= ctx->cap) {
        ctx->error = -2;
        return 0u;
    }
    ctx->buf[pos] = 0u;  /* NUL terminator */
    ctx->str_off++;
    return off;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void fdt_init(fdt_ctx_t *ctx, void *buf, size_t cap)
{
    ctx->buf        = (uint8_t *)buf;
    ctx->cap        = cap;
    ctx->error      = 0;
    ctx->struct_off = 0u;
    ctx->str_off    = 0u;

    /* Header: 40 bytes.  Memory reservation map: one 16-byte empty entry. */
    ctx->hdr_size    = sizeof(fdt_header_t) + 16u;  /* 56 bytes */
    ctx->struct_start = ctx->hdr_size;

    /* Reserve back half of the buffer for the string table.
     * Struct and string sections grow toward each other. */
    ctx->str_start   = cap / 2u;

    if (cap < ctx->hdr_size + 64u) {
        ctx->error = -3;
        return;
    }

    /* Zero the header and reservation map */
    for (size_t i = 0u; i < ctx->hdr_size; i++) {
        ctx->buf[i] = 0u;
    }

    /* Write memory reservation map: one empty entry (address=0, size=0) */
    /* (already zeroed above — two 64-bit zeros) */
}

void fdt_begin_node(fdt_ctx_t *ctx, const char *name)
{
    emit_u32(ctx, FDT_TOKEN_BEGIN_NODE);
    emit_str(ctx, name ? name : "");
    emit_align4(ctx);
}

void fdt_end_node(fdt_ctx_t *ctx)
{
    emit_u32(ctx, FDT_TOKEN_END_NODE);
}

/*
 * emit_prop_header — emit FDT_TOKEN_PROP, len, nameoff for a property.
 * The caller must emit exactly `len` bytes of data afterward.
 */
static void emit_prop_header(fdt_ctx_t *ctx, const char *name, uint32_t len)
{
    uint32_t nameoff = str_intern(ctx, name);
    emit_u32(ctx, FDT_TOKEN_PROP);
    emit_u32(ctx, len);
    emit_u32(ctx, nameoff);
}

void fdt_prop_u32(fdt_ctx_t *ctx, const char *name, uint32_t val)
{
    emit_prop_header(ctx, name, 4u);
    emit_u32(ctx, val);
}

void fdt_prop_u64(fdt_ctx_t *ctx, const char *name, uint64_t val)
{
    emit_prop_header(ctx, name, 8u);
    emit_u32(ctx, (uint32_t)(val >> 32));
    emit_u32(ctx, (uint32_t)(val & 0xFFFFFFFFU));
}

void fdt_prop_string(fdt_ctx_t *ctx, const char *name, const char *val)
{
    size_t slen = 0u;
    if (val) {
        for (const char *p = val; *p; p++) slen++;
    }
    uint32_t len = (uint32_t)(slen + 1u);  /* include NUL */
    emit_prop_header(ctx, name, len);
    emit_str(ctx, val ? val : "");
    /* FDT struct section must remain 4-byte aligned after properties */
    emit_align4(ctx);
}

void fdt_prop_reg64(fdt_ctx_t *ctx, uint64_t base, uint64_t size)
{
    emit_prop_header(ctx, "reg", 16u);
    emit_u32(ctx, (uint32_t)(base >> 32));
    emit_u32(ctx, (uint32_t)(base & 0xFFFFFFFFU));
    emit_u32(ctx, (uint32_t)(size >> 32));
    emit_u32(ctx, (uint32_t)(size & 0xFFFFFFFFU));
}

void fdt_prop_reg64_n(fdt_ctx_t *ctx,
                       const uint64_t *bases,
                       const uint64_t *sizes,
                       uint32_t n)
{
    emit_prop_header(ctx, "reg", n * 16u);
    for (uint32_t i = 0u; i < n; i++) {
        emit_u32(ctx, (uint32_t)(bases[i] >> 32));
        emit_u32(ctx, (uint32_t)(bases[i] & 0xFFFFFFFFU));
        emit_u32(ctx, (uint32_t)(sizes[i] >> 32));
        emit_u32(ctx, (uint32_t)(sizes[i] & 0xFFFFFFFFU));
    }
}

void fdt_prop_u32_array(fdt_ctx_t *ctx, const char *name,
                         const uint32_t *vals, uint32_t n)
{
    emit_prop_header(ctx, name, n * 4u);
    for (uint32_t i = 0u; i < n; i++) {
        emit_u32(ctx, vals[i]);
    }
}

size_t fdt_finish(fdt_ctx_t *ctx)
{
    if (ctx->error) return 0u;

    /* Emit end token */
    emit_u32(ctx, FDT_TOKEN_END);

    if (ctx->error) return 0u;

    size_t struct_size = ctx->struct_off;
    size_t str_size    = ctx->str_off;

    /* Move string table immediately after the struct section */
    size_t str_new_start = ctx->struct_start + struct_size;
    /* Align to 4 bytes */
    while (str_new_start & 3u) str_new_start++;

    if (str_new_start + str_size > ctx->cap) {
        ctx->error = -4;
        return 0u;
    }

    /* Copy string table to its final position */
    uint8_t *src = ctx->buf + ctx->str_start;
    uint8_t *dst = ctx->buf + str_new_start;
    for (size_t i = 0u; i < str_size; i++) {
        dst[i] = src[i];
    }

    size_t total = str_new_start + str_size;
    /* Align total to 4 bytes */
    while (total & 3u) {
        if (total < ctx->cap) ctx->buf[total] = 0u;
        total++;
    }

    /* Fill header */
    fdt_header_t *h = (fdt_header_t *)ctx->buf;
    wr_be32((uint8_t *)&h->magic,              FDT_MAGIC);
    wr_be32((uint8_t *)&h->totalsize,          (uint32_t)total);
    wr_be32((uint8_t *)&h->off_dt_struct,      (uint32_t)ctx->struct_start);
    wr_be32((uint8_t *)&h->off_dt_strings,     (uint32_t)str_new_start);
    wr_be32((uint8_t *)&h->off_mem_rsvmap,     (uint32_t)sizeof(fdt_header_t));
    wr_be32((uint8_t *)&h->version,            17u);
    wr_be32((uint8_t *)&h->last_comp_version,  16u);
    wr_be32((uint8_t *)&h->boot_cpuid_phys,    0u);
    wr_be32((uint8_t *)&h->size_dt_strings,    (uint32_t)str_size);
    wr_be32((uint8_t *)&h->size_dt_struct,     (uint32_t)struct_size);

    return total;
}
