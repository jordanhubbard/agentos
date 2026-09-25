/*
 * RemoteOS protocol v2 client. See include/remoteos_client.h.
 *
 * Hand-rolled JSON rather than a parser library: a PD has no allocator, the
 * message set here is small and fixed, and vendoring cJSON into the root task
 * to read four integers would cost more than it explains. The emitter builds
 * into the caller's buffer and the reader only ever looks for keys this file
 * sends, which is why the scanner below can be as simple as it is.
 */

#include "../include/remoteos_client.h"

/* ─── Local libc ─────────────────────────────────────────────────────────── */
/*
 * On-target consumers have no string.h, so everything this file needs lives
 * here and remains usable by both host tests and a future queue-based client.
 */

static uint32_t r_strlen(const char *s)
{
    uint32_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static void r_memcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

/* ─── Envelope writer ────────────────────────────────────────────────────── */

/*
 * Append-with-a-cursor. Every put_* checks the remaining space and sets
 * overflow rather than truncating, so a too-small buffer surfaces as an
 * error instead of a malformed envelope the service would reject obscurely.
 */
typedef struct {
    uint8_t *buf;
    uint32_t cap;
    uint32_t len;
    bool     overflow;
} r_writer_t;

static void w_init(r_writer_t *w, uint8_t *buf, uint32_t cap)
{
    w->buf = buf; w->cap = cap; w->len = 0; w->overflow = false;
}

static void w_raw(r_writer_t *w, const char *s, uint32_t n)
{
    if (w->overflow) return;
    if (w->len + n > w->cap) { w->overflow = true; return; }
    r_memcpy(w->buf + w->len, s, n);
    w->len += n;
}

static void w_str(r_writer_t *w, const char *s)
{
    w_raw(w, s, r_strlen(s));
}

static void w_u32(r_writer_t *w, uint32_t v)
{
    char tmp[10];
    int n = 0;
    if (v == 0) { w_raw(w, "0", 1); return; }
    while (v && n < 10) { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (n--) w_raw(w, &tmp[n], 1);
}

static void w_i32(r_writer_t *w, int32_t v)
{
    if (v < 0) {
        w_raw(w, "-", 1);
        /* Negate in unsigned space so INT32_MIN does not overflow. */
        w_u32(w, (uint32_t)0 - (uint32_t)v);
    } else {
        w_u32(w, (uint32_t)v);
    }
}

/*
 * JSON string body. The only strings this client sends are client names and
 * window titles; escape the characters that would break the envelope and drop
 * anything non-printable rather than emitting raw bytes the service would
 * reject.
 */
static void w_quoted(r_writer_t *w, const char *s)
{
    w_raw(w, "\"", 1);
    for (uint32_t i = 0; s && s[i]; i++) {
        char ch = s[i];
        if (ch == '"')        w_raw(w, "\\\"", 2);
        else if (ch == '\\')  w_raw(w, "\\\\", 2);
        else if ((uint8_t)ch >= 0x20 && (uint8_t)ch < 0x7f) w_raw(w, &ch, 1);
        else w_raw(w, "?", 1);
    }
    w_raw(w, "\"", 1);
}

/* ─── Reply scanner ──────────────────────────────────────────────────────── */

static bool r_key_at(const uint8_t *b, uint32_t len, uint32_t i, const char *key)
{
    uint32_t k = r_strlen(key);
    if (i + k + 2 > len) return false;
    if (b[i] != '"') return false;
    for (uint32_t j = 0; j < k; j++)
        if (b[i + 1 + j] != (uint8_t)key[j]) return false;
    return b[i + 1 + k] == '"';
}

/*
 * Find "key": and return the offset of the first byte of its value.
 *
 * Skips over string literals so a key name appearing inside a value -- an
 * error message quoting one, say -- cannot be mistaken for the key itself.
 */
static bool r_find_value(const uint8_t *b, uint32_t len, const char *key,
                          uint32_t *out)
{
    bool in_string = false;
    for (uint32_t i = 0; i < len; i++) {
        if (in_string) {
            if (b[i] == '\\') { i++; continue; }
            if (b[i] == '"') in_string = false;
            continue;
        }
        if (b[i] == '"') {
            if (r_key_at(b, len, i, key)) {
                uint32_t j = i + r_strlen(key) + 2;
                while (j < len && (b[j] == ':' || b[j] == ' ')) j++;
                if (j >= len) return false;
                *out = j;
                return true;
            }
            in_string = true;
        }
    }
    return false;
}

static bool r_get_u32(const uint8_t *b, uint32_t len, const char *key,
                       uint32_t *out)
{
    uint32_t i;
    if (!r_find_value(b, len, key, &i)) return false;
    if (i >= len || b[i] < '0' || b[i] > '9') return false;
    uint32_t v = 0;
    while (i < len && b[i] >= '0' && b[i] <= '9') {
        v = v * 10u + (uint32_t)(b[i] - '0');
        i++;
    }
    *out = v;
    return true;
}

static bool r_get_true(const uint8_t *b, uint32_t len, const char *key)
{
    uint32_t i;
    if (!r_find_value(b, len, key, &i)) return false;
    return (i + 4 <= len) && b[i] == 't' && b[i+1] == 'r'
        && b[i+2] == 'u' && b[i+3] == 'e';
}

/* ─── Framing ────────────────────────────────────────────────────────────── */

static remoteos_status_t r_send(remoteos_client_t *c, uint32_t len,
                                 const void *trailer, uint32_t trailer_len)
{
    uint8_t hdr[4];
    hdr[0] = (uint8_t)(len >> 24); hdr[1] = (uint8_t)(len >> 16);
    hdr[2] = (uint8_t)(len >> 8);  hdr[3] = (uint8_t)(len);
    if (!c->tp.write(c->tp.ctx, hdr, 4)) return REMOTEOS_ERR_TRANSPORT;
    if (!c->tp.write(c->tp.ctx, c->buf, len)) return REMOTEOS_ERR_TRANSPORT;
    if (trailer_len && !c->tp.write(c->tp.ctx, trailer, trailer_len))
        return REMOTEOS_ERR_TRANSPORT;
    return REMOTEOS_OK;
}

static remoteos_status_t r_recv(remoteos_client_t *c)
{
    uint8_t hdr[4];
    if (!c->tp.read(c->tp.ctx, hdr, 4)) return REMOTEOS_ERR_TRANSPORT;
    uint32_t len = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16)
                 | ((uint32_t)hdr[2] << 8)  | (uint32_t)hdr[3];
    if (len > c->buf_size) return REMOTEOS_ERR_OVERFLOW;
    if (len && !c->tp.read(c->tp.ctx, c->buf, len)) return REMOTEOS_ERR_TRANSPORT;
    c->reply_len = len;
    return REMOTEOS_OK;
}

/*
 * One synchronous round trip. The envelope is already in c->buf; on return it
 * holds the reply instead, so callers read their results out before issuing
 * anything else.
 */
static remoteos_status_t r_call(remoteos_client_t *c, const r_writer_t *w,
                                 const void *trailer, uint32_t trailer_len,
                                 uint32_t id)
{
    if (w->overflow) return REMOTEOS_ERR_OVERFLOW;

    remoteos_status_t st = r_send(c, w->len, trailer, trailer_len);
    if (st != REMOTEOS_OK) return st;

    st = r_recv(c);
    if (st != REMOTEOS_OK) return st;

    uint32_t reply_id = 0;
    if (!r_get_u32(c->buf, c->reply_len, "id", &reply_id))
        return REMOTEOS_ERR_PROTOCOL;
    /*
     * Replies arrive in request order on a single connection, so a mismatch
     * means the stream has desynchronised rather than that a reply is merely
     * out of order. Fail instead of hunting for the right one.
     */
    if (reply_id != id) return REMOTEOS_ERR_PROTOCOL;

    if (!r_get_true(c->buf, c->reply_len, "ok")) {
        uint32_t code = 0;
        c->remote_code = r_get_u32(c->buf, c->reply_len, "code", &code)
                       ? (int)code : -1;
        return REMOTEOS_ERR_REMOTE;
    }
    c->remote_code = 0;
    return REMOTEOS_OK;
}

/* Open an envelope: {"v":2,"id":N,"op":"...","params":{ */
static uint32_t r_begin(remoteos_client_t *c, r_writer_t *w, const char *op)
{
    uint32_t id = c->next_id++;
    if (c->next_id == 0) c->next_id = 1;   /* id 0 means "no reply expected" */
    w_init(w, c->buf, c->buf_size);
    w_str(w, "{\"v\":2,\"id\":");
    w_u32(w, id);
    w_str(w, ",\"op\":");
    w_quoted(w, op);
    w_str(w, ",\"params\":{");
    return id;
}

static void r_end(r_writer_t *w)
{
    w_str(w, "}}");
}

/* ─── Public API ─────────────────────────────────────────────────────────── */

remoteos_status_t remoteos_init(remoteos_client_t *c,
                                 const remoteos_transport_t *tp,
                                 uint8_t *buf, uint32_t buf_size)
{
    if (!c || !tp || !tp->read || !tp->write || !buf) return REMOTEOS_ERR_ARG;
    if (buf_size < REMOTEOS_MIN_BUFFER) return REMOTEOS_ERR_ARG;
    c->tp = *tp;
    c->buf = buf;
    c->buf_size = buf_size;
    c->next_id = 1;
    c->reply_len = 0;
    c->remote_code = 0;
    return REMOTEOS_OK;
}

remoteos_status_t remoteos_hello(remoteos_client_t *c, const char *client_name)
{
    if (!c) return REMOTEOS_ERR_ARG;
    r_writer_t w;
    uint32_t id = r_begin(c, &w, "hello");
    w_str(&w, "\"protocol\":");
    w_u32(&w, REMOTEOS_PROTOCOL_VERSION);
    w_str(&w, ",\"client\":");
    w_quoted(&w, client_name ? client_name : "agentos");
    r_end(&w);
    return r_call(c, &w, 0, 0, id);
}

remoteos_status_t remoteos_display_open(remoteos_client_t *c,
                                         uint32_t w_px, uint32_t h_px,
                                         const char *title,
                                         uint32_t *out_w, uint32_t *out_h,
                                         uint32_t *out_fb_handle)
{
    if (!c || !w_px || !h_px) return REMOTEOS_ERR_ARG;
    r_writer_t w;
    uint32_t id = r_begin(c, &w, "display.open");
    w_str(&w, "\"w\":");
    w_u32(&w, w_px);
    w_str(&w, ",\"h\":");
    w_u32(&w, h_px);
    w_str(&w, ",\"title\":");
    w_quoted(&w, title ? title : "agentOS");
    r_end(&w);

    remoteos_status_t st = r_call(c, &w, 0, 0, id);
    if (st != REMOTEOS_OK) return st;

    /*
     * Adopt what the service actually opened rather than what was asked for.
     * The host may have been told to use a different size, and on the
     * window-surface path the framebuffer is whatever SDL handed back.
     */
    uint32_t v;
    if (out_w) *out_w = r_get_u32(c->buf, c->reply_len, "w", &v) ? v : w_px;
    if (out_h) *out_h = r_get_u32(c->buf, c->reply_len, "h", &v) ? v : h_px;
    if (out_fb_handle) {
        if (!r_get_u32(c->buf, c->reply_len, "fb_handle", &v))
            return REMOTEOS_ERR_PROTOCOL;
        *out_fb_handle = v;
    }
    return REMOTEOS_OK;
}

remoteos_status_t remoteos_display_close(remoteos_client_t *c)
{
    if (!c) return REMOTEOS_ERR_ARG;
    r_writer_t w;
    uint32_t id = r_begin(c, &w, "display.close");
    r_end(&w);
    return r_call(c, &w, 0, 0, id);
}

remoteos_status_t remoteos_frame_commit(remoteos_client_t *c)
{
    if (!c) return REMOTEOS_ERR_ARG;
    r_writer_t w;
    uint32_t id = r_begin(c, &w, "frame.commit");
    r_end(&w);
    return r_call(c, &w, 0, 0, id);
}

remoteos_status_t remoteos_fill_rect(remoteos_client_t *c, uint32_t handle,
                                      int32_t x, int32_t y,
                                      uint32_t w_px, uint32_t h_px,
                                      uint32_t rgb)
{
    if (!c) return REMOTEOS_ERR_ARG;
    r_writer_t w;
    uint32_t id = r_begin(c, &w, "surface.fill_rect");
    w_str(&w, "\"handle\":");
    w_u32(&w, handle);
    w_str(&w, ",\"rgb\":");
    w_u32(&w, rgb);
    w_str(&w, ",\"rect\":{\"x\":");
    w_i32(&w, x);
    w_str(&w, ",\"y\":");
    w_i32(&w, y);
    w_str(&w, ",\"w\":");
    w_u32(&w, w_px);
    w_str(&w, ",\"h\":");
    w_u32(&w, h_px);
    w_str(&w, "}");
    r_end(&w);
    return r_call(c, &w, 0, 0, id);
}

remoteos_status_t remoteos_surface_upload(remoteos_client_t *c, uint32_t handle,
                                           const void *pixels, uint32_t len)
{
    if (!c || !pixels || !len) return REMOTEOS_ERR_ARG;
    r_writer_t w;
    uint32_t id = r_begin(c, &w, "surface.upload");
    w_str(&w, "\"handle\":");
    w_u32(&w, handle);
    w_str(&w, ",\"payload_len\":");
    w_u32(&w, len);
    r_end(&w);
    return r_call(c, &w, pixels, len, id);
}

remoteos_status_t remoteos_shutdown(remoteos_client_t *c)
{
    if (!c) return REMOTEOS_ERR_ARG;
    r_writer_t w;
    uint32_t id = r_begin(c, &w, "shutdown");
    r_end(&w);
    return r_call(c, &w, 0, 0, id);
}

int remoteos_remote_code(const remoteos_client_t *c)
{
    return c ? c->remote_code : 0;
}

const char *remoteos_status_str(remoteos_status_t s)
{
    switch (s) {
        case REMOTEOS_OK:            return "ok";
        case REMOTEOS_ERR_TRANSPORT: return "transport";
        case REMOTEOS_ERR_OVERFLOW:  return "overflow";
        case REMOTEOS_ERR_PROTOCOL:  return "protocol";
        case REMOTEOS_ERR_REMOTE:    return "remote";
        case REMOTEOS_ERR_ARG:       return "bad-argument";
    }
    return "unknown";
}
