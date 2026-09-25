/*
 * RemoteOS protocol v2 client — transport-independent protocol foundation.
 *
 * Speaks the wire protocol published by RemoteOS-SDL
 * (https://github.com/jordanhubbard/RemoteOS-SDL, PROTOCOL.md) so a
 * FB_BACKEND_REMOTE_API framebuffer can be presented by a host-side display
 * service instead of local hardware. The same service already backs the
 * RubyOS and PythonOS desktops; nothing in it is specific to those guests.
 *
 * Framing: a four-byte unsigned big-endian length precedes every JSON
 * envelope. When params carry "payload_len", exactly that many binary bytes
 * follow the envelope.
 *
 *   {"v":2,"id":17,"op":"display.open","params":{"w":640,"h":480}}
 *   {"v":2,"id":17,"ok":true,"result":{"handle":1,"fb_handle":1,"w":640,"h":480}}
 *
 * Deliberately free of seL4, Microkit and libc: the caller supplies both the
 * transport and the scratch buffer. This file does not grant a PD device
 * ownership; any future on-target caller must use the existing serial and
 * framebuffer virtualizer contracts.
 *
 * Not thread safe, and synchronous by construction: every request that
 * carries a positive id blocks until its matching reply arrives. Handles
 * belong to one connection and expire with it.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/* ─── Transport ──────────────────────────────────────────────────────────── */

/*
 * Both callbacks must transfer exactly n bytes or fail; short transfers are
 * an error rather than something the client retries, because a half-written
 * envelope desynchronises the stream for good.
 */
typedef struct {
    void *ctx;
    bool (*write)(void *ctx, const void *buf, uint32_t n);
    bool (*read)(void *ctx, void *buf, uint32_t n);
} remoteos_transport_t;

/* ─── Errors ─────────────────────────────────────────────────────────────── */

typedef enum {
    REMOTEOS_OK             = 0,
    REMOTEOS_ERR_TRANSPORT  = 1,  /* read or write did not move every byte */
    REMOTEOS_ERR_OVERFLOW   = 2,  /* envelope longer than the scratch buffer */
    REMOTEOS_ERR_PROTOCOL   = 3,  /* reply was not parseable as v2 */
    REMOTEOS_ERR_REMOTE     = 4,  /* service answered ok:false */
    REMOTEOS_ERR_ARG        = 5,  /* caller passed something unusable */
} remoteos_status_t;

/* ─── Client ─────────────────────────────────────────────────────────────── */

#define REMOTEOS_PROTOCOL_VERSION 2

/*
 * A scratch buffer holds one envelope in each direction. 4 KiB is ample for
 * every control message here; bulk pixels travel as a binary trailer and are
 * never copied through it.
 */
#define REMOTEOS_MIN_BUFFER 512

typedef struct {
    remoteos_transport_t tp;
    uint8_t  *buf;
    uint32_t  buf_size;
    uint32_t  next_id;
    uint32_t  reply_len;        /* length of the reply currently in buf */
    int       remote_code;      /* service error code when ERR_REMOTE */
} remoteos_client_t;

/* ─── Lifecycle ──────────────────────────────────────────────────────────── */

remoteos_status_t remoteos_init(remoteos_client_t *c,
                                 const remoteos_transport_t *tp,
                                 uint8_t *buf, uint32_t buf_size);

/*
 * Negotiate. Must be the first request on a connection; the service rejects
 * anything else until it has seen one.
 */
remoteos_status_t remoteos_hello(remoteos_client_t *c, const char *client_name);

/* ─── Display ────────────────────────────────────────────────────────────── */

/*
 * Open the host window. The size asked for is a request: the service may open
 * something else and reports what it actually created, which the caller is
 * expected to adopt. out_w/out_h/out_handle may each be NULL.
 */
remoteos_status_t remoteos_display_open(remoteos_client_t *c,
                                         uint32_t w, uint32_t h,
                                         const char *title,
                                         uint32_t *out_w, uint32_t *out_h,
                                         uint32_t *out_fb_handle);

remoteos_status_t remoteos_display_close(remoteos_client_t *c);

/* Present the framebuffer. */
remoteos_status_t remoteos_frame_commit(remoteos_client_t *c);

/* ─── Surfaces ───────────────────────────────────────────────────────────── */

remoteos_status_t remoteos_fill_rect(remoteos_client_t *c, uint32_t handle,
                                      int32_t x, int32_t y,
                                      uint32_t w, uint32_t h, uint32_t rgb);

/*
 * Push width*height*4 bytes of BGRX into a surface. This is the path a
 * framebuffer flip takes: pixels go out as a binary trailer rather than
 * through the envelope buffer.
 */
remoteos_status_t remoteos_surface_upload(remoteos_client_t *c, uint32_t handle,
                                           const void *pixels, uint32_t len);

/* Ask the service to exit. */
remoteos_status_t remoteos_shutdown(remoteos_client_t *c);

/* ─── Introspection ──────────────────────────────────────────────────────── */

/* Service error code from the last REMOTEOS_ERR_REMOTE, or 0. */
int remoteos_remote_code(const remoteos_client_t *c);

const char *remoteos_status_str(remoteos_status_t s);
