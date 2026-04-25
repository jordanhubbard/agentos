/*
 * fdt_builder.h — minimal Flattened Device Tree (FDT/DTB) builder
 *
 * Generates a standards-compliant DTB blob in a caller-supplied buffer.
 * No dynamic allocation; all state lives in fdt_ctx_t on the caller's stack.
 *
 * Subset implemented: properties (cells, strings, reg, ranges, compatible,
 * status), child nodes, and the /memory and /chosen nodes needed to boot
 * Linux or FreeBSD guests under agentOS VMMs.
 *
 * Usage:
 *   uint8_t buf[4096];
 *   fdt_ctx_t ctx;
 *   fdt_init(&ctx, buf, sizeof(buf));
 *   fdt_begin_node(&ctx, "");          // root node
 *   fdt_prop_u32(&ctx, "#address-cells", 2);
 *   fdt_prop_u32(&ctx, "#size-cells",    2);
 *   fdt_prop_string(&ctx, "compatible", "riscv-virtio");
 *   fdt_begin_node(&ctx, "memory@80000000");
 *   fdt_prop_string(&ctx, "device_type", "memory");
 *   fdt_prop_reg64(&ctx, 0x80000000, 0x08000000);   // base, size
 *   fdt_end_node(&ctx);
 *   fdt_begin_node(&ctx, "chosen");
 *   fdt_prop_string(&ctx, "bootargs", "console=hvc0 rw");
 *   fdt_end_node(&ctx);
 *   fdt_end_node(&ctx);               // end root
 *   size_t dtb_size = fdt_finish(&ctx);
 *
 * The resulting buf[0..dtb_size) is a valid DTB ready to be placed in
 * guest memory and passed to the guest kernel entry (a1 on RISC-V,
 * x0 on AArch64).
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/* ── DTB constants ────────────────────────────────────────────────────────── */

#define FDT_MAGIC         0xD00DFEEDU
#define FDT_TOKEN_BEGIN_NODE 0x00000001U
#define FDT_TOKEN_END_NODE   0x00000002U
#define FDT_TOKEN_PROP       0x00000003U
#define FDT_TOKEN_NOP        0x00000004U
#define FDT_TOKEN_END        0x00000009U

#define FDT_MAX_DEPTH     16u    /* maximum nesting depth */
#define FDT_MAX_STRINGS   512u   /* string table capacity in bytes */

/* ── DTB header (big-endian on-wire format) ───────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
} fdt_header_t;

/* ── Builder context ─────────────────────────────────────────────────────── */

typedef struct {
    uint8_t  *buf;          /* output buffer */
    size_t    cap;          /* buffer capacity in bytes */
    size_t    struct_off;   /* current write position in struct section */
    size_t    str_off;      /* current write position in string table */
    int       error;        /* non-zero if overflow or other error */
    /* Pre-computed offsets resolved at fdt_init time */
    size_t    hdr_size;     /* sizeof(fdt_header_t) + mem rsvmap (8 bytes) */
    size_t    str_start;    /* byte offset of string table in buf */
    size_t    struct_start; /* byte offset of struct section in buf */
} fdt_ctx_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

/*
 * fdt_init — initialise the builder.
 *
 * Reserves space for the header, memory reservation map (one empty entry),
 * and the struct + string sections in buf[0..cap).
 * String table is placed after the struct section at a fixed offset.
 */
void fdt_init(fdt_ctx_t *ctx, void *buf, size_t cap);

/* fdt_begin_node — emit FDT_TOKEN_BEGIN_NODE followed by a NUL-terminated name */
void fdt_begin_node(fdt_ctx_t *ctx, const char *name);

/* fdt_end_node — emit FDT_TOKEN_END_NODE */
void fdt_end_node(fdt_ctx_t *ctx);

/* fdt_prop_u32 — single 32-bit cell property */
void fdt_prop_u32(fdt_ctx_t *ctx, const char *name, uint32_t val);

/* fdt_prop_u64 — single 64-bit property (two 32-bit BE cells) */
void fdt_prop_u64(fdt_ctx_t *ctx, const char *name, uint64_t val);

/* fdt_prop_string — NUL-terminated string property */
void fdt_prop_string(fdt_ctx_t *ctx, const char *name, const char *val);

/*
 * fdt_prop_reg64 — emit a "reg" property with one (base, size) entry.
 *
 * Assumes #address-cells=2 and #size-cells=2 (64-bit addresses).
 * Use fdt_prop_reg64_n for multiple entries.
 */
void fdt_prop_reg64(fdt_ctx_t *ctx, uint64_t base, uint64_t size);

/*
 * fdt_prop_reg64_n — emit a "reg" property with n (base, size) pairs.
 *
 * bases[] and sizes[] are parallel arrays of length n.
 */
void fdt_prop_reg64_n(fdt_ctx_t *ctx,
                       const uint64_t *bases,
                       const uint64_t *sizes,
                       uint32_t n);

/*
 * fdt_prop_u32_array — emit a property as an array of 32-bit BE cells.
 *
 * Used for "interrupts", "clocks", "interrupt-parent", etc.
 */
void fdt_prop_u32_array(fdt_ctx_t *ctx, const char *name,
                         const uint32_t *vals, uint32_t n);

/*
 * fdt_finish — seal the DTB and return the total size in bytes.
 *
 * After this call ctx->buf[0..return_value) is a valid DTB blob.
 * Returns 0 on overflow or error (check ctx->error for the error code).
 */
size_t fdt_finish(fdt_ctx_t *ctx);
