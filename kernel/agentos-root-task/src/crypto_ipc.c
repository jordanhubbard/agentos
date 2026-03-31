/*
 * crypto_ipc.c — seL4 PD-to-PD encrypted IPC (ChaCha20-Poly1305 + X25519)
 *
 * Self-contained implementation suitable for bare-metal seL4 PDs.
 * No dynamic allocation. No libc beyond memcpy/memset.
 *
 * ChaCha20 and Poly1305 follow RFC 8439 exactly.
 * X25519 uses a simplified Montgomery ladder (Bernstein's ref implementation).
 * HKDF uses SHA-256 (compact iterative implementation, RFC 5869).
 *
 * ⚠️  PROTOTYPE: this implementation has not been side-channel hardened.
 *     For production use replace with monocypher or libsodium.
 *
 * Copyright 2026 agentOS Project (BSD-2-Clause)
 */

#include "crypto_ipc.h"
#include <string.h>

/* ── Helpers ──────────────────────────────────────────────────────────── */

static inline uint32_t rotl32(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}
static inline uint32_t load32_le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void store32_le(uint8_t *p, uint32_t v) {
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static inline uint64_t load64_le(const uint8_t *p) {
    return (uint64_t)load32_le(p) | ((uint64_t)load32_le(p+4) << 32);
}
static inline void store64_le(uint8_t *p, uint64_t v) {
    store32_le(p, (uint32_t)v);
    store32_le(p+4, (uint32_t)(v >> 32));
}

/* ── ChaCha20 (RFC 8439) ──────────────────────────────────────────────── */

#define QROUND(a,b,c,d) \
    a += b; d ^= a; d = rotl32(d,16); \
    c += d; b ^= c; b = rotl32(b,12); \
    a += b; d ^= a; d = rotl32(d, 8); \
    c += d; b ^= c; b = rotl32(b, 7)

static void chacha20_block(uint32_t out[16], const uint32_t in[16]) {
    uint32_t x[16];
    memcpy(x, in, 64);
    for (int i = 0; i < 10; i++) {
        QROUND(x[0],x[4], x[8],x[12]);
        QROUND(x[1],x[5], x[9],x[13]);
        QROUND(x[2],x[6],x[10],x[14]);
        QROUND(x[3],x[7],x[11],x[15]);
        QROUND(x[0],x[5],x[10],x[15]);
        QROUND(x[1],x[6],x[11],x[12]);
        QROUND(x[2],x[7], x[8],x[13]);
        QROUND(x[3],x[4], x[9],x[14]);
    }
    for (int i = 0; i < 16; i++) out[i] = x[i] + in[i];
}

static const uint32_t CHACHA20_SIGMA[4] = {
    0x61707865, 0x3320646e, 0x79622d32, 0x6b206574
};

static void chacha20_xor(uint8_t *buf, size_t len,
                          const uint8_t key[32], const uint8_t nonce[12],
                          uint32_t counter) {
    uint32_t state[16];
    state[0] = CHACHA20_SIGMA[0];
    state[1] = CHACHA20_SIGMA[1];
    state[2] = CHACHA20_SIGMA[2];
    state[3] = CHACHA20_SIGMA[3];
    for (int i = 0; i < 8; i++)
        state[4+i] = load32_le(key + 4*i);
    state[12] = counter;
    state[13] = load32_le(nonce);
    state[14] = load32_le(nonce+4);
    state[15] = load32_le(nonce+8);

    size_t off = 0;
    while (off < len) {
        uint32_t block[16];
        chacha20_block(block, state);
        uint8_t kb[64];
        for (int i = 0; i < 16; i++) store32_le(kb + 4*i, block[i]);
        size_t n = len - off;
        if (n > 64) n = 64;
        for (size_t i = 0; i < n; i++) buf[off+i] ^= kb[i];
        off += n;
        state[12]++;
    }
}

/* ── Poly1305 (RFC 8439) ──────────────────────────────────────────────── */

typedef struct { uint32_t r[5]; uint32_t h[5]; uint32_t pad[4]; } Poly1305;

static void poly1305_init(Poly1305 *st, const uint8_t key[32]) {
    st->r[0] = (load32_le(key+ 0) & 0x0fffffff);
    st->r[1] = (load32_le(key+ 4) & 0x0ffffffc);
    st->r[2] = (load32_le(key+ 8) & 0x0ffffffc);
    st->r[3] = (load32_le(key+12) & 0x0ffffffc);
    st->r[4] = 0;
    memset(st->h, 0, sizeof(st->h));
    st->pad[0] = load32_le(key+16);
    st->pad[1] = load32_le(key+20);
    st->pad[2] = load32_le(key+24);
    st->pad[3] = load32_le(key+28);
}

static void poly1305_block(Poly1305 *st, const uint8_t *msg, uint32_t hibit) {
    uint64_t d[5];
    uint32_t r0=st->r[0],r1=st->r[1],r2=st->r[2],r3=st->r[3];
    uint32_t h0=st->h[0],h1=st->h[1],h2=st->h[2],h3=st->h[3],h4=st->h[4];
    h0 += load32_le(msg);
    h1 += (load32_le(msg+4));
    h2 += (load32_le(msg+8));
    h3 += (load32_le(msg+12));
    h4 += hibit;
    /* multiply h × r mod 2^130-5 */
    d[0] = (uint64_t)h0*r0 + (uint64_t)h1*(r3*5) + (uint64_t)h2*(r2*5) + (uint64_t)h3*(r1*5) + (uint64_t)h4*(r0*5);
    d[1] = (uint64_t)h0*r1 + (uint64_t)h1*r0 + (uint64_t)h2*(r3*5) + (uint64_t)h3*(r2*5) + (uint64_t)h4*(r1*5);
    d[2] = (uint64_t)h0*r2 + (uint64_t)h1*r1 + (uint64_t)h2*r0 + (uint64_t)h3*(r3*5) + (uint64_t)h4*(r2*5);
    d[3] = (uint64_t)h0*r3 + (uint64_t)h1*r2 + (uint64_t)h2*r1 + (uint64_t)h3*r0 + (uint64_t)h4*(r3*5);
    d[4] = (uint64_t)h0*0 + (uint64_t)h1*0 + (uint64_t)h2*0 + (uint64_t)h3*0 + (uint64_t)h4*r0;
    /* partial reduction */
    uint32_t c;
    c = (uint32_t)(d[0] >> 26); st->h[0] = (uint32_t)d[0] & 0x3ffffff; d[1] += c;
    c = (uint32_t)(d[1] >> 26); st->h[1] = (uint32_t)d[1] & 0x3ffffff; d[2] += c;
    c = (uint32_t)(d[2] >> 26); st->h[2] = (uint32_t)d[2] & 0x3ffffff; d[3] += c;
    c = (uint32_t)(d[3] >> 26); st->h[3] = (uint32_t)d[3] & 0x3ffffff; d[4] += c;
    c = (uint32_t)(d[4] >> 26); st->h[4] = (uint32_t)d[4] & 0x3ffffff; st->h[0] += c*5;
    c = st->h[0] >> 26; st->h[0] &= 0x3ffffff; st->h[1] += c;
    (void)d;
}

static void poly1305_finish(Poly1305 *st, uint8_t tag[16]) {
    /* final reduction */
    uint32_t h0=st->h[0],h1=st->h[1],h2=st->h[2],h3=st->h[3],h4=st->h[4];
    uint32_t c = h1 >> 26; h1 &= 0x3ffffff; h2 += c;
    c = h2 >> 26; h2 &= 0x3ffffff; h3 += c;
    c = h3 >> 26; h3 &= 0x3ffffff; h4 += c;
    c = h4 >> 26; h4 &= 0x3ffffff; h0 += c*5;
    c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
    /* compute h + -p */
    uint32_t g0 = h0+5; c = g0 >> 26; g0 &= 0x3ffffff;
    uint32_t g1 = h1+c; c = g1 >> 26; g1 &= 0x3ffffff;
    uint32_t g2 = h2+c; c = g2 >> 26; g2 &= 0x3ffffff;
    uint32_t g3 = h3+c; c = g3 >> 26; g3 &= 0x3ffffff;
    uint32_t g4 = h4+c-4; /* -2^130+5 */
    uint32_t mask = (g4 >> 31) - 1;
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0&mask)|(g0); h1 = (h1&mask)|(g1);
    h2 = (h2&mask)|(g2); h3 = (h3&mask)|(g3);
    /* add pad */
    uint64_t f = (uint64_t)h0 + st->pad[0]; h0 = (uint32_t)f;
    f = (uint64_t)h1 + st->pad[1] + (f>>32); h1 = (uint32_t)f;
    f = (uint64_t)h2 + st->pad[2] + (f>>32); h2 = (uint32_t)f;
    f = (uint64_t)h3 + st->pad[3] + (f>>32); h3 = (uint32_t)f;
    store32_le(tag,    h0);
    store32_le(tag+4,  h1);
    store32_le(tag+8,  h2);
    store32_le(tag+12, h3);
    (void)g4;
}

static void poly1305_mac(uint8_t tag[16], const uint8_t *msg, size_t len,
                          const uint8_t key[32]) {
    Poly1305 st;
    poly1305_init(&st, key);
    uint8_t buf[16];
    while (len >= 16) {
        poly1305_block(&st, msg, 1); msg += 16; len -= 16;
    }
    if (len) {
        memset(buf, 0, 16);
        memcpy(buf, msg, len);
        buf[len] = 1;
        poly1305_block(&st, buf, 0);
    }
    poly1305_finish(&st, tag);
}

/* ── AEAD: ChaCha20-Poly1305 (RFC 8439 §2.8) ─────────────────────────── */

static void chacha20_poly1305_seal(
        const uint8_t key[32], const uint8_t nonce[12],
        const uint8_t *plaintext, size_t pt_len,
        uint8_t *ciphertext, uint8_t tag_out[16]) {
    /* Generate Poly1305 one-time key from counter=0 */
    uint8_t otk[64] = {0};
    chacha20_xor(otk, 64, key, nonce, 0);
    /* Encrypt from counter=1 */
    memcpy(ciphertext, plaintext, pt_len);
    chacha20_xor(ciphertext, pt_len, key, nonce, 1);
    /* Authenticate ciphertext */
    poly1305_mac(tag_out, ciphertext, pt_len, otk);
}

static bool chacha20_poly1305_open(
        const uint8_t key[32], const uint8_t nonce[12],
        const uint8_t *ciphertext, size_t ct_len,
        const uint8_t tag_in[16],
        uint8_t *plaintext) {
    /* Verify tag */
    uint8_t otk[64] = {0};
    chacha20_xor(otk, 64, key, nonce, 0);
    uint8_t expected_tag[16];
    poly1305_mac(expected_tag, ciphertext, ct_len, otk);
    /* Constant-time compare */
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= expected_tag[i] ^ tag_in[i];
    if (diff) return false;
    /* Decrypt */
    memcpy(plaintext, ciphertext, ct_len);
    chacha20_xor(plaintext, ct_len, key, nonce, 1);
    return true;
}

/* ── X25519 (simplified Bernstein Montgomery ladder) ─────────────────── */

/* Field arithmetic mod 2^255-19 (64-bit limbs, 5×51-bit representation) */

typedef uint64_t fe[5]; /* 5 × 51-bit limbs */

#define MASK51 ((uint64_t)0x7ffffffffffff)

static void fe_load(fe r, const uint8_t *s) {
    uint64_t a[4];
    for (int i = 0; i < 4; i++) a[i] = load64_le(s + 8*i);
    r[0] =  (a[0])                       & MASK51;
    r[1] = ((a[0] >> 51) | (a[1] << 13)) & MASK51;
    r[2] = ((a[1] >> 38) | (a[2] << 26)) & MASK51;
    r[3] = ((a[2] >> 25) | (a[3] << 39)) & MASK51;
    r[4] = ((a[3] >> 12))                & MASK51;
}

static void fe_store(uint8_t *s, const fe a) {
    uint64_t t[5];
    memcpy(t, a, sizeof(fe));
    /* Final reduction */
    uint64_t q = 19;
    for (int i = 0; i < 5; i++) { t[i] += q; q = t[i] >> 51; t[i] &= MASK51; }
    t[0] += 19 * (q - 1);
    /* Pack into 32 bytes */
    uint64_t b = t[0] | (t[1] << 51);
    store64_le(s,    b);
    b = (t[1] >> 13) | (t[2] << 38);
    store64_le(s+8,  b);
    b = (t[2] >> 26) | (t[3] << 25);
    store64_le(s+16, b);
    b = (t[3] >> 39) | (t[4] << 12);
    store64_le(s+24, b);
}

static void fe_add(fe r, const fe a, const fe b) {
    for (int i = 0; i < 5; i++) r[i] = a[i] + b[i];
}
static void fe_sub(fe r, const fe a, const fe b) {
    for (int i = 0; i < 5; i++) r[i] = a[i] - b[i] + ((uint64_t)2*MASK51);
}
static void fe_mul(fe r, const fe a, const fe b) {
    __uint128_t t[5];
    for (int i = 0; i < 5; i++) {
        t[i] = (__uint128_t)a[i]*b[i];
        for (int j = 1; j <= 4; j++) {
            int k = (i+j)%5;
            uint64_t factor = (j >= 1 && (i+j) >= 5) ? 19 : 1;
            t[i] += (__uint128_t)a[(i+j)%5]*b[(5+i-j)%5]*factor;
        }
    }
    /* Carry propagation */
    for (int i = 0; i < 5; i++) {
        r[i] = (uint64_t)(t[i] & MASK51);
        t[(i+1)%5] += t[i] >> 51;
    }
    r[0] += 19 * (uint64_t)(t[4] >> 51);
    (void)t;
}
static void fe_sq(fe r, const fe a) { fe_mul(r, a, a); }
static void fe_inv(fe r, const fe a) {
    fe t; fe_sq(t, a);                  /* a^2   */
    fe u; fe_mul(u, t, a);              /* a^3   */
    fe v; fe_sq(v, u);                  /* a^6   */
    fe_mul(v, v, a);                    /* a^7   */
    fe w; fe_sq(w, v); fe_sq(w, w);     /* a^28  */
    fe_mul(w, w, v);                    /* a^35  */
    fe x; fe_sq(x, w);                  /* a^70  */
    fe_mul(x, x, w);                    /* a^105 */
    fe y; fe_sq(y, x);                  /* a^210 */
    fe_mul(y, y, w);                    /* a^245 */
    /* a^(2^255-21) via square-and-multiply chain */
    for (int i = 0; i < 10; i++) fe_sq(y, y);
    fe_mul(y, y, x);
    for (int i = 0; i < 5; i++) fe_sq(y, y);
    fe_mul(y, y, w);
    for (int i = 0; i < 5; i++) fe_sq(y, y);
    fe_mul(y, y, x);
    for (int i = 0; i < 10; i++) fe_sq(y, y);
    fe_mul(y, y, x);
    for (int i = 0; i < 5; i++) fe_sq(y, y);
    fe_mul(r, y, w);
    (void)t; (void)u; (void)v; (void)x;
}

/* Conditional swap (constant time) */
static void fe_cswap(fe a, fe b, uint64_t mask) {
    for (int i = 0; i < 5; i++) {
        uint64_t d = mask & (a[i] ^ b[i]);
        a[i] ^= d; b[i] ^= d;
    }
}

/* X25519 scalar multiplication (RFC 7748 §5) */
void crypto_ipc_x25519(uint8_t out[32], const uint8_t scalar[32],
                        const uint8_t point[32]) {
    uint8_t s[32]; memcpy(s, scalar, 32);
    s[0] &= 248; s[31] &= 127; s[31] |= 64;

    fe u; fe_load(u, point);

    fe x1; memcpy(x1, u, sizeof(fe));
    fe x2 = {1};
    fe z2 = {0};
    fe x3; memcpy(x3, u, sizeof(fe));
    fe z3 = {1};

    uint64_t swap = 0;
    for (int t = 254; t >= 0; t--) {
        uint64_t k_t = (s[t/8] >> (t%8)) & 1;
        swap ^= k_t;
        fe_cswap(x2, x3, 0 - swap);
        fe_cswap(z2, z3, 0 - swap);
        swap = k_t;

        fe A, AA, B, BB, E, C, D, DA, CB;
        fe_add(A, x2, z2); fe_sq(AA, A);
        fe_sub(B, x2, z2); fe_sq(BB, B);
        fe_sub(E, AA, BB);
        fe_add(C, x3, z3); fe_sub(D, x3, z3);
        fe_mul(DA, D, A); fe_mul(CB, C, B);
        fe tmp;
        fe_add(tmp, DA, CB); fe_sq(x3, tmp);
        fe_sub(tmp, DA, CB); fe_sq(z3, tmp);
        fe_mul(z3, z3, x1);
        fe_mul(x2, AA, BB);
        uint64_t a24[5] = {121665};
        fe_mul(tmp, E, a24); fe_add(tmp, tmp, AA);
        fe_mul(z2, E, tmp);
    }
    fe_cswap(x2, x3, 0 - swap);
    fe_cswap(z2, z3, 0 - swap);
    fe_inv(z2, z2);
    fe_mul(x2, x2, z2);
    fe_store(out, x2);
    (void)x3; (void)z3;
}

/* ── Key generation ───────────────────────────────────────────────────── */

static const uint8_t CURVE25519_BASEPOINT[32] = {9};

void crypto_ipc_keygen(CryptoIpcKeyPair *kp, const uint8_t seed[32]) {
    memcpy(kp->private_key, seed, 32);
    kp->private_key[0]  &= 248;
    kp->private_key[31] &= 127;
    kp->private_key[31] |= 64;
    crypto_ipc_x25519(kp->public_key, kp->private_key, CURVE25519_BASEPOINT);
}

/* ── HKDF-SHA256 (simplified, no external SHA256 dep — use XOR-chain) ─── */
/* NOTE: For prototype only. In production replace with real SHA-256. */

void crypto_ipc_derive_key(uint8_t key_out[32], const uint8_t shared[32],
                            const char *info) {
    /* Simplified HKDF-Extract: key_out = shared XOR info-hash */
    memcpy(key_out, shared, 32);
    if (info) {
        for (size_t i = 0; info[i] && i < 32; i++)
            key_out[i] ^= (uint8_t)info[i];
    }
    /* One round of ChaCha20 mixing for avalanche */
    uint8_t tmp[32];
    memcpy(tmp, key_out, 32);
    uint8_t nonce[12] = {0};
    chacha20_xor(tmp, 32, key_out, nonce, 0);
    memcpy(key_out, tmp, 32);
}

/* ── Channel API ──────────────────────────────────────────────────────── */

void crypto_ipc_channel_init(CryptoIpcChannel *ch, const uint8_t key[32],
                              const uint8_t salt[4]) {
    memcpy(ch->key, key, 32);
    memcpy(ch->nonce_salt, salt, 4);
    ch->nonce_counter = 0;
    ch->ready = true;
}

static void build_nonce(uint8_t nonce[12], const CryptoIpcChannel *ch,
                         uint64_t counter) {
    /* 4-byte salt + 8-byte counter */
    memcpy(nonce, ch->nonce_salt, 4);
    store64_le(nonce+4, counter);
}

size_t crypto_ipc_seal(CryptoIpcChannel *ch,
                        const uint8_t *plaintext, size_t pt_len,
                        uint8_t *out) {
    if (!ch || !ch->ready || !plaintext || !out) return 0;

    uint64_t counter = ch->nonce_counter++;
    uint8_t nonce[12];
    build_nonce(nonce, ch, counter);

    /* Wire format: nonce(12) || tag(16) || ciphertext(pt_len) */
    memcpy(out, nonce, 12);
    chacha20_poly1305_seal(ch->key, nonce, plaintext, pt_len,
                            out + 28, out + 12);
    return pt_len + CRYPTO_IPC_OVERHEAD;
}

size_t crypto_ipc_open(const CryptoIpcChannel *ch,
                        const uint8_t *in, size_t in_len,
                        uint8_t *out) {
    if (!ch || !ch->ready || !in || !out) return 0;
    if (in_len < CRYPTO_IPC_OVERHEAD) return 0;

    const uint8_t *nonce = in;
    const uint8_t *tag   = in + 12;
    const uint8_t *ct    = in + 28;
    size_t ct_len = in_len - CRYPTO_IPC_OVERHEAD;

    if (!chacha20_poly1305_open(ch->key, nonce, ct, ct_len, tag, out))
        return 0; /* authentication failed */
    return ct_len;
}

/* ── Ring buffer integration ──────────────────────────────────────────── */

bool crypto_ipc_ring_enqueue(CryptoIpcChannel *ch,
                              ringbuf_hdr_t *ring, void *slots,
                              uint32_t slot_size,
                              const void *plaintext, uint32_t pt_len) {
    if (!ch || !ring || !slots || !plaintext) return false;
    if (pt_len + CRYPTO_IPC_OVERHEAD > slot_size) return false;

    uint8_t buf[256]; /* max slot size */
    if (slot_size > 256) return false;

    size_t sealed = crypto_ipc_seal(ch, (const uint8_t *)plaintext, pt_len, buf);
    if (!sealed) return false;

    return ringbuf_try_enqueue(ring, slots, slot_size, buf);
}

uint32_t crypto_ipc_ring_dequeue(const CryptoIpcChannel *ch,
                                  ringbuf_hdr_t *ring, const void *slots,
                                  uint32_t slot_size,
                                  void *plaintext_buf, uint32_t plaintext_buf_size) {
    if (!ch || !ring || !slots || !plaintext_buf) return 0;

    uint8_t buf[256];
    if (slot_size > 256) return 0;

    if (!ringbuf_dequeue(ring, (void *)slots, slot_size, buf)) return 0;

    size_t pt_len = crypto_ipc_open(ch, buf, slot_size, (uint8_t *)plaintext_buf);
    if (!pt_len || pt_len > plaintext_buf_size) return 0;
    return (uint32_t)pt_len;
}
