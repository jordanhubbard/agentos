/*
 * agentOS HTTP Service — Public Interface
 *
 * http_svc is an active PD (priority 140) that maintains a table of up to
 * HTTP_MAX_HANDLERS URL-prefix → app_id route mappings.
 *
 * AppManager calls OP_HTTP_REGISTER when launching a new app and
 * OP_HTTP_UNREGISTER on teardown.  The Rust http-gateway
 * (userspace/servers/http-gateway/) dispatches incoming HTTP requests
 * via OP_HTTP_DISPATCH.
 *
 * URL prefixes are passed inline in message registers (8 bytes per MR,
 * MR4..MR11) to avoid requiring a shared memory region between the caller
 * and this PD.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── http_svc version ────────────────────────────────────────────────────── */
#define HTTP_SVC_VERSION    1u

/* ── Console slot / pd_id ────────────────────────────────────────────────── */
#define HTTP_SVC_CONSOLE_SLOT   19u
#define HTTP_SVC_PD_ID          19u

/* ── Channel IDs (from http_svc's perspective) ───────────────────────────── */
#define HTTP_CH_CONTROLLER   0   /* controller PPCs in (pp=true) */
#define HTTP_CH_APP_MANAGER  1   /* app_manager PPCs in (pp=true) */

/* ── IPC Opcodes ─────────────────────────────────────────────────────────── */

/*
 * OP_HTTP_REGISTER (0x90) — register a URL-prefix route handler
 *
 * Prefix is packed into MRs 4..11: each MR holds 8 ASCII bytes, little-endian.
 * Up to 64 bytes total (HTTP_PREFIX_MAX).  Unused bytes must be zero.
 *
 *   MR1 = app_id       (owning application's ID from AppManager)
 *   MR2 = vnic_id      (app's vNIC slot; 0xFFFFFFFF if networking not used)
 *   MR3 = prefix_len   (byte count of URL prefix, 1..HTTP_PREFIX_MAX)
 *   MR4..MR11 = prefix bytes packed 8-per-word, little-endian
 *   Reply:
 *   MR0 = result (HTTP_OK or HTTP_ERR_*)
 *   MR1 = handler_id   (0..HTTP_MAX_HANDLERS-1)
 */
#define OP_HTTP_REGISTER    0x90u

/*
 * OP_HTTP_UNREGISTER (0x91) — remove a registered route
 *   MR1 = handler_id
 *   Reply:
 *   MR0 = result
 */
#define OP_HTTP_UNREGISTER  0x91u

/*
 * OP_HTTP_DISPATCH (0x92) — look up which app handles an incoming URL
 *
 * Prefix of incoming URL is packed into MRs 1..8 (8 bytes per MR).
 * http_svc does a longest-prefix match against the handler table.
 *
 *   MR1..MR8 = URL path bytes packed 8-per-word, little-endian
 *   Reply:
 *   MR0 = result
 *   MR1 = matched app_id  (HTTP_APP_ID_NONE = 0xFFFFFFFF if no match)
 *   MR2 = matched vnic_id
 *   MR3 = matched handler_id
 *
 *   MVP: implemented — returns HTTP_APP_ID_NONE when table is empty.
 */
#define OP_HTTP_DISPATCH    0x92u

/*
 * OP_HTTP_LIST (0x93) — enumerate registered handlers into http_req_shmem
 *   Writes an array of http_handler_entry_t at http_req_shmem offset 0.
 *   Reply:
 *   MR0 = result
 *   MR1 = handler_count
 */
#define OP_HTTP_LIST        0x93u

/*
 * OP_HTTP_HEALTH (0x94) — liveness check
 *   Reply:
 *   MR0 = HTTP_OK
 *   MR1 = active_handler_count
 *   MR2 = HTTP_SVC_VERSION
 */
#define OP_HTTP_HEALTH      0x94u

/* ── Result codes ────────────────────────────────────────────────────────── */
#define HTTP_OK             0u
#define HTTP_ERR_NO_SLOTS   1u  /* handler table full */
#define HTTP_ERR_NOT_FOUND  2u  /* handler_id not registered */
#define HTTP_ERR_INVAL      3u  /* invalid argument (e.g. prefix_len == 0) */
#define HTTP_ERR_STUB       4u  /* feature not yet implemented */

/* ── Handler table limits ────────────────────────────────────────────────── */
#define HTTP_MAX_HANDLERS   8u
#define HTTP_PREFIX_MAX     64u   /* must be <= 8 × sizeof(seL4_Word) */

/* Sentinel returned when no handler matches */
#define HTTP_APP_ID_NONE    0xFFFFFFFFu

/* ── Shared memory layout (http_req_shmem, 64KB) ────────────────────────── */
#define HTTP_REQ_SHMEM_SIZE     0x10000u   /* 64KB */
#define HTTP_REQ_HDR_OFF        0u         /* http_req_hdr_t (184 bytes) */
#define HTTP_REQ_BODY_OFF       192u       /* request body (aligned to 64 bytes) */
#define HTTP_REQ_BODY_MAX       (HTTP_REQ_SHMEM_SIZE - HTTP_REQ_BODY_OFF)

/* Magic written at offset 0 of a valid request header */
#define HTTP_REQ_MAGIC          0x48545450u  /* "HTTP" */

/*
 * http_req_hdr_t — request header written at HTTP_REQ_HDR_OFF
 *
 * Used by the Rust http-gateway to pass a dispatched request's metadata.
 * The gateway writes this before calling OP_HTTP_DISPATCH (future use).
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;            /* HTTP_REQ_MAGIC */
    uint8_t  method[8];        /* "GET\0", "POST\0", etc. */
    uint8_t  path[128];        /* URL path (null-terminated) */
    uint8_t  content_type[32]; /* e.g. "application/json\0" */
    uint32_t body_len;         /* bytes at HTTP_REQ_BODY_OFF */
    uint32_t flags;            /* reserved */
    uint8_t  _pad[4];
} http_req_hdr_t;              /* 184 bytes */

/* ── Handler entry written by OP_HTTP_LIST ───────────────────────────────── */
typedef struct __attribute__((packed)) {
    bool     active;
    uint8_t  _pad[3];
    uint32_t handler_id;
    uint32_t app_id;
    uint32_t vnic_id;
    char     prefix[HTTP_PREFIX_MAX];
} http_handler_entry_t;        /* 80 bytes */

/*
 * ── MR packing layout for URL prefixes ───────────────────────────────────
 *
 * OP_HTTP_REGISTER and OP_HTTP_DISPATCH pass a URL prefix inline in message
 * registers.  The prefix (up to HTTP_PREFIX_MAX = 64 bytes) is split into
 * 8 × 64-bit message registers (MR4..MR11), 8 bytes per register,
 * little-endian byte order.  Unused bytes are zero.
 *
 * Callers pack the prefix using this layout before calling microkit_ppcall().
 * http_svc.c provides http_pack_prefix() / http_unpack_prefix() helpers.
 */
#define HTTP_PREFIX_MR_BASE   4u   /* first MR index for prefix data */
#define HTTP_PREFIX_MR_COUNT  8u   /* 8 MRs × 8 bytes = 64 bytes */
