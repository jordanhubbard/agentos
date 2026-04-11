/*
 * monocypher.c — Ed25519 sign + verify for agentOS
 *
 * Implements GF(2^255-19) arithmetic, SHA-512, and the full Ed25519
 * sign / verify algorithm.
 *
 * Derived from:
 *   • Monocypher 4.0.2 (Loup Vaillant, public domain / CC0)
 *   • TweetNaCl (D.J. Bernstein et al., public domain)
 *   • FIPS 180-4 SHA-512 specification
 *
 * Constraints:
 *   • No libc (bare-metal seL4 / Microkit)
 *   • No dynamic allocation
 *   • Compiles with clang -nostdlib -ffreestanding
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "monocypher.h"

/* ── Minimal memory helpers (avoid libc) ─────────────────────────────────── */

static void mc_memset(void *dst, int val, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)val;
}

static void mc_memcpy(void *dst, const void *src, size_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static int mc_memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] < y[i]) return -1;
        if (x[i] > y[i]) return  1;
    }
    return 0;
}

/* ── SHA-512 ─────────────────────────────────────────────────────────────── */

static const uint64_t sha512_K[80] = {
    0x428a2f98d728ae22ull, 0x7137449123ef65cdull, 0xb5c0fbcfec4d3b2full,
    0xe9b5dba58189dbbcull, 0x3956c25bf348b538ull, 0x59f111f1b605d019ull,
    0x923f82a4af194f9bull, 0xab1c5ed5da6d8118ull, 0xd807aa98a3030242ull,
    0x12835b0145706fbeull, 0x243185be4ee4b28cull, 0x550c7dc3d5ffb4e2ull,
    0x72be5d74f27b896full, 0x80deb1fe3b1696b1ull, 0x9bdc06a725c71235ull,
    0xc19bf174cf692694ull, 0xe49b69c19ef14ad2ull, 0xefbe4786384f25e3ull,
    0x0fc19dc68b8cd5b5ull, 0x240ca1cc77ac9c65ull, 0x2de92c6f592b0275ull,
    0x4a7484aa6ea6e483ull, 0x5cb0a9dcbd41fbd4ull, 0x76f988da831153b5ull,
    0x983e5152ee66dfabull, 0xa831c66d2db43210ull, 0xb00327c898fb213full,
    0xbf597fc7beef0ee4ull, 0xc6e00bf33da88fc2ull, 0xd5a79147930aa725ull,
    0x06ca6351e003826full, 0x142929670a0e6e70ull, 0x27b70a8546d22ffcull,
    0x2e1b21385c26c926ull, 0x4d2c6dfc5ac42aedull, 0x53380d139d95b3dfull,
    0x650a73548baf63deull, 0x766a0abb3c77b2a8ull, 0x81c2c92e47edaee6ull,
    0x92722c851482353bull, 0xa2bfe8a14cf10364ull, 0xa81a664bbc423001ull,
    0xc24b8b70d0f89791ull, 0xc76c51a30654be30ull, 0xd192e819d6ef5218ull,
    0xd69906245565a910ull, 0xf40e35855771202aull, 0x106aa07032bbd1b8ull,
    0x19a4c116b8d2d0c8ull, 0x1e376c085141ab53ull, 0x2748774cdf8eeb99ull,
    0x34b0bcb5e19b48a8ull, 0x391c0cb3c5c95a63ull, 0x4ed8aa4ae3418acbull,
    0x5b9cca4f7763e373ull, 0x682e6ff3d6b2b8a3ull, 0x748f82ee5defb2fcull,
    0x78a5636f43172f60ull, 0x84c87814a1f0ab72ull, 0x8cc702081a6439ecull,
    0x90befffa23631e28ull, 0xa4506cebde82bde9ull, 0xbef9a3f7b2c67915ull,
    0xc67178f2e372532bull, 0xca273eceea26619cull, 0xd186b8c721c0c207ull,
    0xeada7dd6cde0eb1eull, 0xf57d4f7fee6ed178ull, 0x06f067aa72176fbaull,
    0x0a637dc5a2c898a6ull, 0x113f9804bef90daeull, 0x1b710b35131c471bull,
    0x28db77f523047d84ull, 0x32caab7b40c72493ull, 0x3c9ebe0a15c9bebcull,
    0x431d67c49c100d4cull, 0x4cc5d4becb3e42b6ull, 0x597f299cfc657e2aull,
    0x5fcb6fab3ad6faecull, 0x6c44198c4a475817ull,
};

static const uint64_t sha512_H0[8] = {
    0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull,
    0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
    0x510e527fade682d1ull, 0x9b05688c2b3e6c1full,
    0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull,
};

typedef struct {
    uint64_t state[8];
    uint64_t count[2];  /* bit count: count[0]=low, count[1]=high */
    uint8_t  buf[128];
} sha512_ctx;

static inline uint64_t rotr64(uint64_t x, int n)
{
    return (x >> n) | (x << (64 - n));
}

#define CH64(x,y,z)   (((x) & (y)) ^ (~(x) & (z)))
#define MAJ64(x,y,z)  (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0_64(x)     (rotr64(x,28) ^ rotr64(x,34) ^ rotr64(x,39))
#define EP1_64(x)     (rotr64(x,14) ^ rotr64(x,18) ^ rotr64(x,41))
#define SIG0_64(x)    (rotr64(x, 1) ^ rotr64(x, 8) ^ ((x) >> 7))
#define SIG1_64(x)    (rotr64(x,19) ^ rotr64(x,61) ^ ((x) >> 6))

static void sha512_transform(sha512_ctx *ctx, const uint8_t data[128])
{
    uint64_t a, b, c, d, e, f, g, h, t1, t2, w[80];
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint64_t)data[i*8+0] << 56)
             | ((uint64_t)data[i*8+1] << 48)
             | ((uint64_t)data[i*8+2] << 40)
             | ((uint64_t)data[i*8+3] << 32)
             | ((uint64_t)data[i*8+4] << 24)
             | ((uint64_t)data[i*8+5] << 16)
             | ((uint64_t)data[i*8+6] <<  8)
             | ((uint64_t)data[i*8+7]      );
    }
    for (i = 16; i < 80; i++)
        w[i] = SIG1_64(w[i-2]) + w[i-7] + SIG0_64(w[i-15]) + w[i-16];

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (i = 0; i < 80; i++) {
        t1 = h + EP1_64(e) + CH64(e,f,g) + sha512_K[i] + w[i];
        t2 = EP0_64(a) + MAJ64(a,b,c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha512_init(sha512_ctx *ctx)
{
    mc_memcpy(ctx->state, sha512_H0, sizeof(sha512_H0));
    ctx->count[0] = 0;
    ctx->count[1] = 0;
}

static void sha512_update(sha512_ctx *ctx, const uint8_t *data, size_t len)
{
    uint64_t used = (ctx->count[0] >> 3) & 0x7f;

    /* Update 128-bit bit count */
    uint64_t bits = (uint64_t)len << 3;
    ctx->count[0] += bits;
    if (ctx->count[0] < bits) ctx->count[1]++;
    ctx->count[1] += (uint64_t)len >> 61;

    for (size_t i = 0; i < len; i++) {
        ctx->buf[used++] = data[i];
        if (used == 128) {
            sha512_transform(ctx, ctx->buf);
            used = 0;
        }
    }
}

static void sha512_final(sha512_ctx *ctx, uint8_t out[64])
{
    uint64_t used = (ctx->count[0] >> 3) & 0x7f;

    ctx->buf[used++] = 0x80;
    if (used > 112) {
        while (used < 128) ctx->buf[used++] = 0x00;
        sha512_transform(ctx, ctx->buf);
        used = 0;
    }
    while (used < 112) ctx->buf[used++] = 0x00;

    /* Append 128-bit bit length big-endian: count[1] || count[0] */
    for (int i = 7; i >= 0; i--) ctx->buf[112 + (7-i)] = (uint8_t)(ctx->count[1] >> (i*8));
    for (int i = 7; i >= 0; i--) ctx->buf[120 + (7-i)] = (uint8_t)(ctx->count[0] >> (i*8));
    sha512_transform(ctx, ctx->buf);

    for (int i = 0; i < 8; i++) {
        out[i*8+0] = (uint8_t)(ctx->state[i] >> 56);
        out[i*8+1] = (uint8_t)(ctx->state[i] >> 48);
        out[i*8+2] = (uint8_t)(ctx->state[i] >> 40);
        out[i*8+3] = (uint8_t)(ctx->state[i] >> 32);
        out[i*8+4] = (uint8_t)(ctx->state[i] >> 24);
        out[i*8+5] = (uint8_t)(ctx->state[i] >> 16);
        out[i*8+6] = (uint8_t)(ctx->state[i] >>  8);
        out[i*8+7] = (uint8_t)(ctx->state[i]      );
    }
}

static void sha512_hash(uint8_t out[64], const uint8_t *data, size_t len)
{
    sha512_ctx ctx;
    sha512_init(&ctx);
    sha512_update(&ctx, data, len);
    sha512_final(&ctx, out);
}

/* ── GF(2^255-19) field arithmetic (TweetNaCl 16-limb representation) ──── */
/*
 * Each field element is stored as 16 signed 64-bit limbs, each holding
 * a 16-bit value (little-endian).  Arithmetic matches TweetNaCl exactly.
 */

typedef long long gf[16];

static const gf gf0;
static const gf gf1    = {1};
static const gf D      = {0x78a3,0x1359,0x4dca,0x75eb,0xd8ab,0x4141,0x0a4d,0x0070,
                           0xe898,0x7779,0x4079,0x8cc7,0xfe73,0x2b6f,0x6cee,0x5203};
static const gf D2     = {0xf159,0x26b2,0x9b94,0xebd6,0xb156,0x8283,0x149a,0x00e0,
                           0xd130,0xeef3,0x80f2,0x198e,0xfce7,0x56df,0xd9dc,0x2406};
static const gf gfX    = {0xd51a,0x8f25,0x2d60,0xc956,0xa7b2,0x9525,0xc760,0x692c,
                           0xdc5c,0xfdd6,0xe231,0xc0a4,0x53fe,0xcd6e,0x36d3,0x2169};
static const gf gfY    = {0x6658,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,
                           0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666,0x6666};
static const gf gfI    = {0xa0b0,0x4a0e,0x1b27,0xc4ee,0xe478,0xad2f,0x1806,0x2f43,
                           0xd7a7,0x3dfb,0x0099,0x2b4d,0xdf0b,0x4fc1,0x2480,0x2b83};

static void gf_car(gf o)
{
    long long c;
    for (int i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        c = o[i] >> 16;
        o[(i+1) * (i<15)] += c - 1 + 37 * (c-1) * (i==15);
        o[i] -= c << 16;
    }
}

static void gf_add(gf o, const gf a, const gf b)
{
    for (int i = 0; i < 16; i++) o[i] = a[i] + b[i];
}

static void gf_sub(gf o, const gf a, const gf b)
{
    for (int i = 0; i < 16; i++) o[i] = a[i] - b[i];
}

static void gf_cpy(gf o, const gf a)
{
    for (int i = 0; i < 16; i++) o[i] = a[i];
}

static void gf_mul(gf o, const gf a, const gf b)
{
    long long t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++)
            t[i+j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i+16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    gf_car(o);
    gf_car(o);
}

static void gf_sq(gf o, const gf a)
{
    gf_mul(o, a, a);
}

/* Constant-time conditional swap */
static void gf_sel(gf p, gf q, int b)
{
    long long t, c = ~(b - 1);
    for (int i = 0; i < 16; i++) {
        t    = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

/* Pack field element to 32 bytes (little-endian, canonical form) */
static void gf_pack(uint8_t o[32], const gf n)
{
    gf m, t;
    gf_cpy(t, n);
    gf_car(t);
    gf_car(t);
    gf_car(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i-1] >> 16) & 1);
            m[i-1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        gf_sel(t, m, 1-b);
    }
    for (int i = 0; i < 16; i++) {
        o[2*i]   = (uint8_t)(t[i]     );
        o[2*i+1] = (uint8_t)(t[i] >> 8);
    }
}

/* Unpack 32 bytes to field element */
static void gf_unpack(gf o, const uint8_t n[32])
{
    for (int i = 0; i < 16; i++)
        o[i] = (long long)n[2*i] | ((long long)n[2*i+1] << 8);
    o[15] &= 0x7fff;
}

/* Returns 1 if a != b (using packed comparison) */
static int gf_neq(const gf a, const gf b)
{
    uint8_t pa[32], pb[32];
    gf_pack(pa, a);
    gf_pack(pb, b);
    return mc_memcmp(pa, pb, 32) != 0 ? 1 : 0;
}

/*
 * a^(2^252-3) mod p — needed for square root (pow2523 in TweetNaCl).
 * Used during point decompression.
 */
static void gf_pow2523(gf o, const gf a)
{
    gf c, t0;

    gf_sq(c, a);
    gf_mul(t0, c, a);
    gf_sq(c, t0);
    gf_mul(t0, c, a);
    gf_sq(c, t0);
    gf_sq(c, c);
    gf_mul(t0, c, t0);
    gf_sq(c, t0);
    for (int i = 1; i < 4; i++) gf_sq(c, c);
    gf_mul(t0, c, t0);
    gf_sq(c, t0);
    for (int i = 1; i < 8; i++) gf_sq(c, c);
    gf_mul(t0, c, t0);
    gf_sq(c, t0);
    for (int i = 1; i < 16; i++) gf_sq(c, c);
    gf_mul(t0, c, t0);
    gf_sq(c, t0);
    for (int i = 1; i < 32; i++) gf_sq(c, c);
    gf_mul(c, c, t0);
    gf_sq(c, c);
    for (int i = 1; i < 32; i++) gf_sq(c, c);
    gf_mul(t0, c, t0);
    gf_sq(c, t0);
    for (int i = 1; i < 64; i++) gf_sq(c, c);
    gf_mul(c, c, t0);
    gf_sq(c, c);
    gf_sq(c, c);
    gf_mul(o, c, a);
}

/* ── Extended Twisted Edwards point operations ───────────────────────────── */

/* Extended point addition: p += q */
static void pt_add(gf p[4], gf q[4])
{
    gf a, b, c, d, e, f, g, h, t;

    gf_sub(a, p[1], p[0]);
    gf_sub(t, q[1], q[0]);
    gf_mul(a, a, t);
    gf_add(b, p[0], p[1]);
    gf_add(t, q[0], q[1]);
    gf_mul(b, b, t);
    gf_mul(c, p[3], q[3]);
    gf_mul(c, c, D2);
    gf_mul(d, p[2], q[2]);
    gf_add(d, d, d);
    gf_sub(e, b, a);
    gf_sub(f, d, c);
    gf_add(g, d, c);
    gf_add(h, b, a);
    gf_mul(p[0], e, f);
    gf_mul(p[1], h, g);
    gf_mul(p[2], g, f);
    gf_mul(p[3], e, h);
}

/* Constant-time conditional swap of two points */
static void pt_cswap(gf p[4], gf q[4], uint8_t b)
{
    for (int i = 0; i < 4; i++) gf_sel(p[i], q[i], (int)b);
}

/* Scalar multiplication: p = s * q  (double-and-add) */
static void pt_scalarmult(gf p[4], gf q[4], const uint8_t s[32])
{
    gf_cpy(p[0], gf0);
    gf_cpy(p[1], gf1);
    gf_cpy(p[2], gf1);
    gf_cpy(p[3], gf0);

    for (int i = 255; i >= 0; i--) {
        int b = (int)((s[i/8] >> (i & 7)) & 1);
        pt_cswap(p, q, (uint8_t)b);
        pt_add(q, p);
        pt_add(p, p);
        pt_cswap(p, q, (uint8_t)b);
    }
}

/* Scalar multiply with Ed25519 base point */
static void pt_scalarbase(gf p[4], const uint8_t s[32])
{
    gf q[4];
    gf_cpy(q[0], gfX);
    gf_cpy(q[1], gfY);
    gf_cpy(q[2], gf1);
    gf_mul(q[3], gfX, gfY);
    pt_scalarmult(p, q, s);
}

/* Compute z^(p-2) = z^(2^255-21) — field inversion */
static void gf_invert(gf out, const gf z)
{
    gf z2, z9, z11, z2_5_0, z2_10_0, z2_20_0, z2_50_0, z2_100_0, t;

    gf_sq(z2, z);                             /* z^2 */
    gf_sq(t, z2);                             /* z^4 */
    gf_sq(t, t);                              /* z^8 */
    gf_mul(z9, t, z);                         /* z^9 */
    gf_mul(z11, z9, z2);                      /* z^11 */
    gf_sq(t, z11);                            /* z^22 */
    gf_mul(z2_5_0, t, z9);                    /* z^31 = z^(2^5-1) */

    gf_sq(t, z2_5_0);
    for (int i = 1; i < 5; i++) gf_sq(t, t); /* z^(2^10-2^5) */
    gf_mul(z2_10_0, t, z2_5_0);              /* z^(2^10-1) */

    gf_sq(t, z2_10_0);
    for (int i = 1; i < 10; i++) gf_sq(t, t);
    gf_mul(z2_20_0, t, z2_10_0);             /* z^(2^20-1) */

    gf_sq(t, z2_20_0);
    for (int i = 1; i < 20; i++) gf_sq(t, t);
    gf_mul(t, t, z2_20_0);                   /* z^(2^40-1) */

    gf_sq(t, t);
    for (int i = 1; i < 10; i++) gf_sq(t, t);
    gf_mul(z2_50_0, t, z2_10_0);             /* z^(2^50-1) */

    gf_sq(t, z2_50_0);
    for (int i = 1; i < 50; i++) gf_sq(t, t);
    gf_mul(z2_100_0, t, z2_50_0);            /* z^(2^100-1) */

    gf_sq(t, z2_100_0);
    for (int i = 1; i < 100; i++) gf_sq(t, t);
    gf_mul(t, t, z2_100_0);                  /* z^(2^200-1) */

    gf_sq(t, t);
    for (int i = 1; i < 50; i++) gf_sq(t, t);
    gf_mul(t, t, z2_50_0);                   /* z^(2^250-1) */

    gf_sq(t, t);
    gf_sq(t, t);
    gf_sq(t, t);
    gf_sq(t, t);
    gf_sq(t, t);                              /* z^(2^255-2^5) */
    gf_mul(out, t, z11);                      /* z^(2^255-21) = z^(p-2) */
}

/* Pack extended point to compressed 32-byte encoding */
static void pt_pack(uint8_t r[32], gf p[4])
{
    gf tx, ty, zi;
    uint8_t xb[32];

    gf_invert(zi, p[2]);
    gf_mul(tx, p[0], zi);
    gf_mul(ty, p[1], zi);
    gf_pack(r, ty);
    gf_pack(xb, tx);
    r[31] ^= (xb[0] & 1) << 7;
}

/*
 * Decompress and negate a public key point.
 * Returns 0 on success, -1 if point not on curve.
 */
static int pt_unpackneg(gf r[4], const uint8_t p[32])
{
    gf t, chk, num, den, den2, den4, den6;
    uint8_t sign, pb[32];

    sign = p[31] >> 7;
    gf_unpack(r[1], p);        /* y */
    gf_cpy(r[2], gf1);         /* z = 1 */

    /* Compute candidate x using the curve equation */
    gf_sq(num, r[1]);          /* y^2 */
    gf_mul(den, num, D);       /* d * y^2 */
    gf_sub(num, num, gf1);     /* y^2 - 1 */
    gf_add(den, den, gf1);     /* d*y^2 + 1 */

    gf_sq(den2, den);
    gf_sq(den4, den2);
    gf_mul(den6, den4, den2);
    gf_mul(t, den6, num);
    gf_mul(t, t, den);         /* t = num * den^7 */

    gf_pow2523(t, t);          /* t = (num*den^7)^((p-5)/8) */
    gf_mul(t, t, num);
    gf_mul(t, t, den);
    gf_mul(t, t, den);
    gf_mul(r[0], t, den);      /* x = num * den^3 * (num*den^7)^((p-5)/8) */

    gf_sq(chk, r[0]);
    gf_mul(chk, chk, den);
    if (gf_neq(chk, num)) {
        gf_mul(r[0], r[0], gfI);
    }

    gf_sq(chk, r[0]);
    gf_mul(chk, chk, den);
    if (gf_neq(chk, num)) return -1;

    /* Negate x if parity doesn't match sign bit */
    gf_pack(pb, r[0]);
    if ((int)(pb[0] & 1) != (int)sign) {
        gf_sub(r[0], gf0, r[0]);
    }

    gf_mul(r[3], r[0], r[1]); /* T = x * y */
    return 0;
}

/* ── Scalar arithmetic mod l = 2^252 + 27742317777372353535851937790883648493 */

/*
 * sc_reduce: reduce a 64-byte little-endian integer mod the group order l.
 * Input is treated as a 512-bit integer; output is written back to r[0..31].
 * (r[32..63] are zeroed on output.)
 */
static void sc_reduce(uint8_t r[64])
{
    long long x[64];
    long long carry;

    for (int i = 0; i < 64; i++) x[i] = (long long)(uint8_t)r[i];
    for (int i = 0; i < 64; i++) r[i] = 0;

    x[23] += x[63] * 666643; x[24] += x[63] * 470296;
    x[25] += x[63] * 654183; x[26] -= x[63] * 997805;
    x[27] += x[63] * 136657; x[28] -= x[63] * 683901; x[63] = 0;

    x[22] += x[62] * 666643; x[23] += x[62] * 470296;
    x[24] += x[62] * 654183; x[25] -= x[62] * 997805;
    x[26] += x[62] * 136657; x[27] -= x[62] * 683901; x[62] = 0;

    x[21] += x[61] * 666643; x[22] += x[61] * 470296;
    x[23] += x[61] * 654183; x[24] -= x[61] * 997805;
    x[25] += x[61] * 136657; x[26] -= x[61] * 683901; x[61] = 0;

    x[20] += x[60] * 666643; x[21] += x[60] * 470296;
    x[22] += x[60] * 654183; x[23] -= x[60] * 997805;
    x[24] += x[60] * 136657; x[25] -= x[60] * 683901; x[60] = 0;

    x[19] += x[59] * 666643; x[20] += x[59] * 470296;
    x[21] += x[59] * 654183; x[22] -= x[59] * 997805;
    x[23] += x[59] * 136657; x[24] -= x[59] * 683901; x[59] = 0;

    x[18] += x[58] * 666643; x[19] += x[58] * 470296;
    x[20] += x[58] * 654183; x[21] -= x[58] * 997805;
    x[22] += x[58] * 136657; x[23] -= x[58] * 683901; x[58] = 0;

    x[17] += x[57] * 666643; x[18] += x[57] * 470296;
    x[19] += x[57] * 654183; x[20] -= x[57] * 997805;
    x[21] += x[57] * 136657; x[22] -= x[57] * 683901; x[57] = 0;

    x[16] += x[56] * 666643; x[17] += x[56] * 470296;
    x[18] += x[56] * 654183; x[19] -= x[56] * 997805;
    x[20] += x[56] * 136657; x[21] -= x[56] * 683901; x[56] = 0;

    x[15] += x[55] * 666643; x[16] += x[55] * 470296;
    x[17] += x[55] * 654183; x[18] -= x[55] * 997805;
    x[19] += x[55] * 136657; x[20] -= x[55] * 683901; x[55] = 0;

    x[14] += x[54] * 666643; x[15] += x[54] * 470296;
    x[16] += x[54] * 654183; x[17] -= x[54] * 997805;
    x[18] += x[54] * 136657; x[19] -= x[54] * 683901; x[54] = 0;

    x[13] += x[53] * 666643; x[14] += x[53] * 470296;
    x[15] += x[53] * 654183; x[16] -= x[53] * 997805;
    x[17] += x[53] * 136657; x[18] -= x[53] * 683901; x[53] = 0;

    x[12] += x[52] * 666643; x[13] += x[52] * 470296;
    x[14] += x[52] * 654183; x[15] -= x[52] * 997805;
    x[16] += x[52] * 136657; x[17] -= x[52] * 683901; x[52] = 0;

    carry = x[12] >> 21; x[13] += carry; x[12] -= carry << 21;
    carry = x[13] >> 21; x[14] += carry; x[13] -= carry << 21;
    carry = x[14] >> 21; x[15] += carry; x[14] -= carry << 21;
    carry = x[15] >> 21; x[16] += carry; x[15] -= carry << 21;
    carry = x[16] >> 21; x[17] += carry; x[16] -= carry << 21;
    carry = x[17] >> 21; x[18] += carry; x[17] -= carry << 21;
    carry = x[18] >> 21; x[19] += carry; x[18] -= carry << 21;
    carry = x[19] >> 21; x[20] += carry; x[19] -= carry << 21;
    carry = x[20] >> 21; x[21] += carry; x[20] -= carry << 21;
    carry = x[21] >> 21; x[22] += carry; x[21] -= carry << 21;
    carry = x[22] >> 21; x[23] += carry; x[22] -= carry << 21;

    x[11] += x[23] * 666643; x[12] += x[23] * 470296;
    x[13] += x[23] * 654183; x[14] -= x[23] * 997805;
    x[15] += x[23] * 136657; x[16] -= x[23] * 683901; x[23] = 0;

    x[10] += x[22] * 666643; x[11] += x[22] * 470296;
    x[12] += x[22] * 654183; x[13] -= x[22] * 997805;
    x[14] += x[22] * 136657; x[15] -= x[22] * 683901; x[22] = 0;

    x[ 9] += x[21] * 666643; x[10] += x[21] * 470296;
    x[11] += x[21] * 654183; x[12] -= x[21] * 997805;
    x[13] += x[21] * 136657; x[14] -= x[21] * 683901; x[21] = 0;

    x[ 8] += x[20] * 666643; x[ 9] += x[20] * 470296;
    x[10] += x[20] * 654183; x[11] -= x[20] * 997805;
    x[12] += x[20] * 136657; x[13] -= x[20] * 683901; x[20] = 0;

    x[ 7] += x[19] * 666643; x[ 8] += x[19] * 470296;
    x[ 9] += x[19] * 654183; x[10] -= x[19] * 997805;
    x[11] += x[19] * 136657; x[12] -= x[19] * 683901; x[19] = 0;

    x[ 6] += x[18] * 666643; x[ 7] += x[18] * 470296;
    x[ 8] += x[18] * 654183; x[ 9] -= x[18] * 997805;
    x[10] += x[18] * 136657; x[11] -= x[18] * 683901; x[18] = 0;

    carry = x[ 6] >> 21; x[ 7] += carry; x[ 6] -= carry << 21;
    carry = x[ 7] >> 21; x[ 8] += carry; x[ 7] -= carry << 21;
    carry = x[ 8] >> 21; x[ 9] += carry; x[ 8] -= carry << 21;
    carry = x[ 9] >> 21; x[10] += carry; x[ 9] -= carry << 21;
    carry = x[10] >> 21; x[11] += carry; x[10] -= carry << 21;
    carry = x[11] >> 21; x[12] += carry; x[11] -= carry << 21;
    carry = x[12] >> 21; x[13] += carry; x[12] -= carry << 21;
    carry = x[13] >> 21; x[14] += carry; x[13] -= carry << 21;
    carry = x[14] >> 21; x[15] += carry; x[14] -= carry << 21;
    carry = x[15] >> 21; x[16] += carry; x[15] -= carry << 21;
    carry = x[16] >> 21; x[17] += carry; x[16] -= carry << 21;
    carry = x[17] >> 21; x[17] -= carry << 21;

    r[ 0] = (uint8_t)( x[ 0]        );
    r[ 1] = (uint8_t)( x[ 0] >>  8  );
    r[ 2] = (uint8_t)((x[ 0] >> 16) | (x[ 1] << 5));
    r[ 3] = (uint8_t)( x[ 1] >>  3  );
    r[ 4] = (uint8_t)( x[ 1] >> 11  );
    r[ 5] = (uint8_t)((x[ 1] >> 19) | (x[ 2] << 2));
    r[ 6] = (uint8_t)( x[ 2] >>  6  );
    r[ 7] = (uint8_t)((x[ 2] >> 14) | (x[ 3] << 7));
    r[ 8] = (uint8_t)( x[ 3] >>  1  );
    r[ 9] = (uint8_t)( x[ 3] >>  9  );
    r[10] = (uint8_t)((x[ 3] >> 17) | (x[ 4] << 4));
    r[11] = (uint8_t)( x[ 4] >>  4  );
    r[12] = (uint8_t)( x[ 4] >> 12  );
    r[13] = (uint8_t)((x[ 4] >> 20) | (x[ 5] << 1));
    r[14] = (uint8_t)( x[ 5] >>  7  );
    r[15] = (uint8_t)((x[ 5] >> 15) | (x[ 6] << 6));
    r[16] = (uint8_t)( x[ 6] >>  2  );
    r[17] = (uint8_t)( x[ 6] >> 10  );
    r[18] = (uint8_t)((x[ 6] >> 18) | (x[ 7] << 3));
    r[19] = (uint8_t)( x[ 7] >>  5  );
    r[20] = (uint8_t)( x[ 7] >> 13  );
    r[21] = (uint8_t)( x[ 8]        );
    r[22] = (uint8_t)( x[ 8] >>  8  );
    r[23] = (uint8_t)((x[ 8] >> 16) | (x[ 9] << 5));
    r[24] = (uint8_t)( x[ 9] >>  3  );
    r[25] = (uint8_t)( x[ 9] >> 11  );
    r[26] = (uint8_t)((x[ 9] >> 19) | (x[10] << 2));
    r[27] = (uint8_t)( x[10] >>  6  );
    r[28] = (uint8_t)((x[10] >> 14) | (x[11] << 7));
    r[29] = (uint8_t)( x[11] >>  1  );
    r[30] = (uint8_t)( x[11] >>  9  );
    r[31] = (uint8_t)( x[11] >> 17  );
}

/*
 * sc_muladd: compute (a*b + c) mod l, storing 32-byte result in s.
 * All inputs are 32-byte little-endian integers.
 * Used in Ed25519 signing to compute S = (r + H(R||A||M)*a) mod l.
 *
 * Coefficients are 21-bit limbs following the standard Ed25519 approach.
 */
static void sc_muladd(uint8_t *s,
                      const uint8_t *a, const uint8_t *b, const uint8_t *c)
{
    /* Load a, b, c as 21-bit limbs */
    long long a0  = 2097151 & ((long long)a[ 0] | ((long long)a[ 1]<<8) | ((long long)a[ 2]<<16));
    long long a1  = 2097151 & (((long long)a[ 2]>>5) | ((long long)a[ 3]<<3) | ((long long)a[ 4]<<11) | ((long long)a[ 5]<<19));
    long long a2  = 2097151 & (((long long)a[ 5]>>2) | ((long long)a[ 6]<<6) | ((long long)a[ 7]<<14));
    long long a3  = 2097151 & (((long long)a[ 7]>>7) | ((long long)a[ 8]<<1) | ((long long)a[ 9]<<9) | ((long long)a[10]<<17));
    long long a4  = 2097151 & (((long long)a[10]>>4) | ((long long)a[11]<<4) | ((long long)a[12]<<12));
    long long a5  = 2097151 & (((long long)a[12]>>1) | ((long long)a[13]<<7) | ((long long)a[14]<<15));
    long long a6  = 2097151 & (((long long)a[14]>>6) | ((long long)a[15]<<2) | ((long long)a[16]<<10) | ((long long)a[17]<<18));
    long long a7  = 2097151 & (((long long)a[17]>>3) | ((long long)a[18]<<5) | ((long long)a[19]<<13));
    long long a8  = 2097151 & ((long long)a[20] | ((long long)a[21]<<8) | ((long long)a[22]<<16));
    long long a9  = 2097151 & (((long long)a[22]>>5) | ((long long)a[23]<<3) | ((long long)a[24]<<11) | ((long long)a[25]<<19));
    long long a10 = 2097151 & (((long long)a[25]>>2) | ((long long)a[26]<<6) | ((long long)a[27]<<14));
    long long a11 = (((long long)a[27]>>7) | ((long long)a[28]<<1) | ((long long)a[29]<<9) | ((long long)a[30]<<17) | ((long long)a[31]<<25));

    long long b0  = 2097151 & ((long long)b[ 0] | ((long long)b[ 1]<<8) | ((long long)b[ 2]<<16));
    long long b1  = 2097151 & (((long long)b[ 2]>>5) | ((long long)b[ 3]<<3) | ((long long)b[ 4]<<11) | ((long long)b[ 5]<<19));
    long long b2  = 2097151 & (((long long)b[ 5]>>2) | ((long long)b[ 6]<<6) | ((long long)b[ 7]<<14));
    long long b3  = 2097151 & (((long long)b[ 7]>>7) | ((long long)b[ 8]<<1) | ((long long)b[ 9]<<9) | ((long long)b[10]<<17));
    long long b4  = 2097151 & (((long long)b[10]>>4) | ((long long)b[11]<<4) | ((long long)b[12]<<12));
    long long b5  = 2097151 & (((long long)b[12]>>1) | ((long long)b[13]<<7) | ((long long)b[14]<<15));
    long long b6  = 2097151 & (((long long)b[14]>>6) | ((long long)b[15]<<2) | ((long long)b[16]<<10) | ((long long)b[17]<<18));
    long long b7  = 2097151 & (((long long)b[17]>>3) | ((long long)b[18]<<5) | ((long long)b[19]<<13));
    long long b8  = 2097151 & ((long long)b[20] | ((long long)b[21]<<8) | ((long long)b[22]<<16));
    long long b9  = 2097151 & (((long long)b[22]>>5) | ((long long)b[23]<<3) | ((long long)b[24]<<11) | ((long long)b[25]<<19));
    long long b10 = 2097151 & (((long long)b[25]>>2) | ((long long)b[26]<<6) | ((long long)b[27]<<14));
    long long b11 = (((long long)b[27]>>7) | ((long long)b[28]<<1) | ((long long)b[29]<<9) | ((long long)b[30]<<17) | ((long long)b[31]<<25));

    long long c0  = 2097151 & ((long long)c[ 0] | ((long long)c[ 1]<<8) | ((long long)c[ 2]<<16));
    long long c1  = 2097151 & (((long long)c[ 2]>>5) | ((long long)c[ 3]<<3) | ((long long)c[ 4]<<11) | ((long long)c[ 5]<<19));
    long long c2  = 2097151 & (((long long)c[ 5]>>2) | ((long long)c[ 6]<<6) | ((long long)c[ 7]<<14));
    long long c3  = 2097151 & (((long long)c[ 7]>>7) | ((long long)c[ 8]<<1) | ((long long)c[ 9]<<9) | ((long long)c[10]<<17));
    long long c4  = 2097151 & (((long long)c[10]>>4) | ((long long)c[11]<<4) | ((long long)c[12]<<12));
    long long c5  = 2097151 & (((long long)c[12]>>1) | ((long long)c[13]<<7) | ((long long)c[14]<<15));
    long long c6  = 2097151 & (((long long)c[14]>>6) | ((long long)c[15]<<2) | ((long long)c[16]<<10) | ((long long)c[17]<<18));
    long long c7  = 2097151 & (((long long)c[17]>>3) | ((long long)c[18]<<5) | ((long long)c[19]<<13));
    long long c8  = 2097151 & ((long long)c[20] | ((long long)c[21]<<8) | ((long long)c[22]<<16));
    long long c9  = 2097151 & (((long long)c[22]>>5) | ((long long)c[23]<<3) | ((long long)c[24]<<11) | ((long long)c[25]<<19));
    long long c10 = 2097151 & (((long long)c[25]>>2) | ((long long)c[26]<<6) | ((long long)c[27]<<14));
    long long c11 = (((long long)c[27]>>7) | ((long long)c[28]<<1) | ((long long)c[29]<<9) | ((long long)c[30]<<17) | ((long long)c[31]<<25));

    long long s0,s1,s2,s3,s4,s5,s6,s7,s8,s9,s10,s11,s12,s13,s14,s15,s16,s17,s18,s19,s20,s21,s22,s23;
    long long carry0,carry1,carry2,carry3,carry4,carry5,carry6,carry7,carry8,carry9,carry10,carry11;
    long long carry12,carry13,carry14,carry15,carry16,carry17,carry18,carry19,carry20,carry21,carry22;

    s0  = c0  + a0*b0;
    s1  = c1  + a0*b1  + a1*b0;
    s2  = c2  + a0*b2  + a1*b1  + a2*b0;
    s3  = c3  + a0*b3  + a1*b2  + a2*b1  + a3*b0;
    s4  = c4  + a0*b4  + a1*b3  + a2*b2  + a3*b1  + a4*b0;
    s5  = c5  + a0*b5  + a1*b4  + a2*b3  + a3*b2  + a4*b1  + a5*b0;
    s6  = c6  + a0*b6  + a1*b5  + a2*b4  + a3*b3  + a4*b2  + a5*b1  + a6*b0;
    s7  = c7  + a0*b7  + a1*b6  + a2*b5  + a3*b4  + a4*b3  + a5*b2  + a6*b1  + a7*b0;
    s8  = c8  + a0*b8  + a1*b7  + a2*b6  + a3*b5  + a4*b4  + a5*b3  + a6*b2  + a7*b1  + a8*b0;
    s9  = c9  + a0*b9  + a1*b8  + a2*b7  + a3*b6  + a4*b5  + a5*b4  + a6*b3  + a7*b2  + a8*b1  + a9*b0;
    s10 = c10 + a0*b10 + a1*b9  + a2*b8  + a3*b7  + a4*b6  + a5*b5  + a6*b4  + a7*b3  + a8*b2  + a9*b1  + a10*b0;
    s11 = c11 + a0*b11 + a1*b10 + a2*b9  + a3*b8  + a4*b7  + a5*b6  + a6*b5  + a7*b4  + a8*b3  + a9*b2  + a10*b1 + a11*b0;
    s12 =       a1*b11 + a2*b10 + a3*b9  + a4*b8  + a5*b7  + a6*b6  + a7*b5  + a8*b4  + a9*b3  + a10*b2 + a11*b1;
    s13 =       a2*b11 + a3*b10 + a4*b9  + a5*b8  + a6*b7  + a7*b6  + a8*b5  + a9*b4  + a10*b3 + a11*b2;
    s14 =       a3*b11 + a4*b10 + a5*b9  + a6*b8  + a7*b7  + a8*b6  + a9*b5  + a10*b4 + a11*b3;
    s15 =       a4*b11 + a5*b10 + a6*b9  + a7*b8  + a8*b7  + a9*b6  + a10*b5 + a11*b4;
    s16 =       a5*b11 + a6*b10 + a7*b9  + a8*b8  + a9*b7  + a10*b6 + a11*b5;
    s17 =       a6*b11 + a7*b10 + a8*b9  + a9*b8  + a10*b7 + a11*b6;
    s18 =       a7*b11 + a8*b10 + a9*b9  + a10*b8 + a11*b7;
    s19 =       a8*b11 + a9*b10 + a10*b9 + a11*b8;
    s20 =       a9*b11 + a10*b10 + a11*b9;
    s21 =       a10*b11 + a11*b10;
    s22 =       a11*b11;
    s23 = 0;

    carry0  = (s0  + (1<<20)) >> 21; s1  += carry0; s0  -= carry0 << 21;
    carry2  = (s2  + (1<<20)) >> 21; s3  += carry2; s2  -= carry2 << 21;
    carry4  = (s4  + (1<<20)) >> 21; s5  += carry4; s4  -= carry4 << 21;
    carry6  = (s6  + (1<<20)) >> 21; s7  += carry6; s6  -= carry6 << 21;
    carry8  = (s8  + (1<<20)) >> 21; s9  += carry8; s8  -= carry8 << 21;
    carry10 = (s10 + (1<<20)) >> 21; s11 += carry10; s10 -= carry10 << 21;
    carry12 = (s12 + (1<<20)) >> 21; s13 += carry12; s12 -= carry12 << 21;
    carry14 = (s14 + (1<<20)) >> 21; s15 += carry14; s14 -= carry14 << 21;
    carry16 = (s16 + (1<<20)) >> 21; s17 += carry16; s16 -= carry16 << 21;
    carry18 = (s18 + (1<<20)) >> 21; s19 += carry18; s18 -= carry18 << 21;
    carry20 = (s20 + (1<<20)) >> 21; s21 += carry20; s20 -= carry20 << 21;
    carry22 = (s22 + (1<<20)) >> 21; s23 += carry22; s22 -= carry22 << 21;

    carry1  = (s1  + (1<<20)) >> 21; s2  += carry1;  s1  -= carry1  << 21;
    carry3  = (s3  + (1<<20)) >> 21; s4  += carry3;  s3  -= carry3  << 21;
    carry5  = (s5  + (1<<20)) >> 21; s6  += carry5;  s5  -= carry5  << 21;
    carry7  = (s7  + (1<<20)) >> 21; s8  += carry7;  s7  -= carry7  << 21;
    carry9  = (s9  + (1<<20)) >> 21; s10 += carry9;  s9  -= carry9  << 21;
    carry11 = (s11 + (1<<20)) >> 21; s12 += carry11; s11 -= carry11 << 21;
    carry13 = (s13 + (1<<20)) >> 21; s14 += carry13; s13 -= carry13 << 21;
    carry15 = (s15 + (1<<20)) >> 21; s16 += carry15; s15 -= carry15 << 21;
    carry17 = (s17 + (1<<20)) >> 21; s18 += carry17; s17 -= carry17 << 21;
    carry19 = (s19 + (1<<20)) >> 21; s20 += carry19; s19 -= carry19 << 21;
    carry21 = (s21 + (1<<20)) >> 21; s22 += carry21; s21 -= carry21 << 21;

    /* Reduce s12..s23 back into s0..s11 using l */
    s11 += s23 * 666643; s12 += s23 * 470296; s13 += s23 * 654183;
    s14 -= s23 * 997805; s15 += s23 * 136657; s16 -= s23 * 683901; s23 = 0;

    s10 += s22 * 666643; s11 += s22 * 470296; s12 += s22 * 654183;
    s13 -= s22 * 997805; s14 += s22 * 136657; s15 -= s22 * 683901; s22 = 0;

    s9  += s21 * 666643; s10 += s21 * 470296; s11 += s21 * 654183;
    s12 -= s21 * 997805; s13 += s21 * 136657; s14 -= s21 * 683901; s21 = 0;

    s8  += s20 * 666643; s9  += s20 * 470296; s10 += s20 * 654183;
    s11 -= s20 * 997805; s12 += s20 * 136657; s13 -= s20 * 683901; s20 = 0;

    s7  += s19 * 666643; s8  += s19 * 470296; s9  += s19 * 654183;
    s10 -= s19 * 997805; s11 += s19 * 136657; s12 -= s19 * 683901; s19 = 0;

    s6  += s18 * 666643; s7  += s18 * 470296; s8  += s18 * 654183;
    s9  -= s18 * 997805; s10 += s18 * 136657; s11 -= s18 * 683901; s18 = 0;

    carry6  = (s6  + (1<<20)) >> 21; s7  += carry6;  s6  -= carry6  << 21;
    carry7  = (s7  + (1<<20)) >> 21; s8  += carry7;  s7  -= carry7  << 21;
    carry8  = (s8  + (1<<20)) >> 21; s9  += carry8;  s8  -= carry8  << 21;
    carry9  = (s9  + (1<<20)) >> 21; s10 += carry9;  s9  -= carry9  << 21;
    carry10 = (s10 + (1<<20)) >> 21; s11 += carry10; s10 -= carry10 << 21;
    carry11 = (s11 + (1<<20)) >> 21; s12 += carry11; s11 -= carry11 << 21;

    s5  += s17 * 666643; s6  += s17 * 470296; s7  += s17 * 654183;
    s8  -= s17 * 997805; s9  += s17 * 136657; s10 -= s17 * 683901; s17 = 0;

    s4  += s16 * 666643; s5  += s16 * 470296; s6  += s16 * 654183;
    s7  -= s16 * 997805; s8  += s16 * 136657; s9  -= s16 * 683901; s16 = 0;

    s3  += s15 * 666643; s4  += s15 * 470296; s5  += s15 * 654183;
    s6  -= s15 * 997805; s7  += s15 * 136657; s8  -= s15 * 683901; s15 = 0;

    s2  += s14 * 666643; s3  += s14 * 470296; s4  += s14 * 654183;
    s5  -= s14 * 997805; s6  += s14 * 136657; s7  -= s14 * 683901; s14 = 0;

    s1  += s13 * 666643; s2  += s13 * 470296; s3  += s13 * 654183;
    s4  -= s13 * 997805; s5  += s13 * 136657; s6  -= s13 * 683901; s13 = 0;

    s0  += s12 * 666643; s1  += s12 * 470296; s2  += s12 * 654183;
    s3  -= s12 * 997805; s4  += s12 * 136657; s5  -= s12 * 683901; s12 = 0;

    carry0  = (s0  + (1<<20)) >> 21; s1  += carry0;  s0  -= carry0  << 21;
    carry1  = (s1  + (1<<20)) >> 21; s2  += carry1;  s1  -= carry1  << 21;
    carry2  = (s2  + (1<<20)) >> 21; s3  += carry2;  s2  -= carry2  << 21;
    carry3  = (s3  + (1<<20)) >> 21; s4  += carry3;  s3  -= carry3  << 21;
    carry4  = (s4  + (1<<20)) >> 21; s5  += carry4;  s4  -= carry4  << 21;
    carry5  = (s5  + (1<<20)) >> 21; s6  += carry5;  s5  -= carry5  << 21;
    carry6  = (s6  + (1<<20)) >> 21; s7  += carry6;  s6  -= carry6  << 21;
    carry7  = (s7  + (1<<20)) >> 21; s8  += carry7;  s7  -= carry7  << 21;
    carry8  = (s8  + (1<<20)) >> 21; s9  += carry8;  s8  -= carry8  << 21;
    carry9  = (s9  + (1<<20)) >> 21; s10 += carry9;  s9  -= carry9  << 21;
    carry10 = (s10 + (1<<20)) >> 21; s11 += carry10; s10 -= carry10 << 21;
    carry11 = (s11 + (1<<20)) >> 21; s12 += carry11; s11 -= carry11 << 21;

    s0  += s12 * 666643; s1  += s12 * 470296; s2  += s12 * 654183;
    s3  -= s12 * 997805; s4  += s12 * 136657; s5  -= s12 * 683901; s12 = 0;

    carry0 = (s0 + (1<<20)) >> 21; s1  += carry0; s0  -= carry0 << 21;
    carry1 = (s1 + (1<<20)) >> 21; s2  += carry1; s1  -= carry1 << 21;
    carry2 = (s2 + (1<<20)) >> 21; s3  += carry2; s2  -= carry2 << 21;
    carry3 = (s3 + (1<<20)) >> 21; s4  += carry3; s3  -= carry3 << 21;
    carry4 = (s4 + (1<<20)) >> 21; s5  += carry4; s4  -= carry4 << 21;
    carry5 = (s5 + (1<<20)) >> 21; s6  += carry5; s5  -= carry5 << 21;
    carry6 = (s6 + (1<<20)) >> 21; s7  += carry6; s6  -= carry6 << 21;
    carry7 = (s7 + (1<<20)) >> 21; s8  += carry7; s7  -= carry7 << 21;
    carry8 = (s8 + (1<<20)) >> 21; s9  += carry8; s8  -= carry8 << 21;
    carry9 = (s9 + (1<<20)) >> 21; s10 += carry9; s9  -= carry9 << 21;
    carry10 = (s10+(1<<20)) >> 21; s11 += carry10; s10 -= carry10 << 21;

    s[ 0] = (uint8_t)( s0 >> 0);
    s[ 1] = (uint8_t)( s0 >> 8);
    s[ 2] = (uint8_t)((s0 >> 16) | (s1 << 5));
    s[ 3] = (uint8_t)( s1 >> 3);
    s[ 4] = (uint8_t)( s1 >> 11);
    s[ 5] = (uint8_t)((s1 >> 19) | (s2 << 2));
    s[ 6] = (uint8_t)( s2 >> 6);
    s[ 7] = (uint8_t)((s2 >> 14) | (s3 << 7));
    s[ 8] = (uint8_t)( s3 >> 1);
    s[ 9] = (uint8_t)( s3 >> 9);
    s[10] = (uint8_t)((s3 >> 17) | (s4 << 4));
    s[11] = (uint8_t)( s4 >> 4);
    s[12] = (uint8_t)( s4 >> 12);
    s[13] = (uint8_t)((s4 >> 20) | (s5 << 1));
    s[14] = (uint8_t)( s5 >> 7);
    s[15] = (uint8_t)((s5 >> 15) | (s6 << 6));
    s[16] = (uint8_t)( s6 >> 2);
    s[17] = (uint8_t)( s6 >> 10);
    s[18] = (uint8_t)((s6 >> 18) | (s7 << 3));
    s[19] = (uint8_t)( s7 >> 5);
    s[20] = (uint8_t)( s7 >> 13);
    s[21] = (uint8_t)( s8 >> 0);
    s[22] = (uint8_t)( s8 >> 8);
    s[23] = (uint8_t)((s8 >> 16) | (s9 << 5));
    s[24] = (uint8_t)( s9 >> 3);
    s[25] = (uint8_t)( s9 >> 11);
    s[26] = (uint8_t)((s9 >> 19) | (s10 << 2));
    s[27] = (uint8_t)( s10 >> 6);
    s[28] = (uint8_t)((s10 >> 14) | (s11 << 7));
    s[29] = (uint8_t)( s11 >> 1);
    s[30] = (uint8_t)( s11 >> 9);
    s[31] = (uint8_t)( s11 >> 17);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

/*
 * crypto_ed25519_public_key — derive public key from 32-byte seed.
 *
 * Steps (RFC 8032 §5.1.5):
 *   1. H = SHA-512(seed)
 *   2. Clamp H[0..31] to produce scalar a
 *   3. pk = a * B  (base-point multiplication)
 */
void crypto_ed25519_public_key(uint8_t *pk, const uint8_t *sk)
{
    uint8_t h[64];
    uint8_t a[32];
    gf p[4];

    sha512_hash(h, sk, 32);

    mc_memcpy(a, h, 32);
    a[0]  &= 248;
    a[31] &= 127;
    a[31] |= 64;

    pt_scalarbase(p, a);
    pt_pack(pk, p);
}

/*
 * crypto_ed25519_sign — sign a message with a 32-byte seed + public key.
 *
 * Steps (RFC 8032 §5.1.6):
 *   1. H = SHA-512(seed);  a = clamp(H[0..31]);  nonce_prefix = H[32..63]
 *   2. r = SHA-512(nonce_prefix || msg) mod l
 *   3. R = r * B  (nonce point)
 *   4. S = (r + SHA-512(R || pk || msg) * a) mod l
 *   5. sig = R || S
 */
void crypto_ed25519_sign(uint8_t *sig,
                         const uint8_t *sk, const uint8_t *pk,
                         const uint8_t *msg, size_t msg_len)
{
    uint8_t h[64];       /* SHA-512(seed) */
    uint8_t a[32];       /* clamped scalar */
    uint8_t az[64];      /* a || nonce_prefix */
    uint8_t nonce[64];   /* r = SHA-512(nonce_prefix || msg) */
    uint8_t hram[64];    /* SHA-512(R || pk || msg) */
    gf R[4];
    sha512_ctx ctx;

    /* Step 1 */
    sha512_hash(h, sk, 32);
    mc_memcpy(az, h, 64);
    az[0]  &= 248;
    az[31] &= 127;
    az[31] |= 64;
    mc_memcpy(a, az, 32);

    /* Step 2: r = SHA-512(nonce_prefix || msg) */
    sha512_init(&ctx);
    sha512_update(&ctx, az + 32, 32);
    sha512_update(&ctx, msg, msg_len);
    sha512_final(&ctx, nonce);
    sc_reduce(nonce);

    /* Step 3: R = r * B */
    pt_scalarbase(R, nonce);
    pt_pack(sig, R);   /* sig[0..31] = R */

    /* Step 4: hram = SHA-512(R || pk || msg) */
    sha512_init(&ctx);
    sha512_update(&ctx, sig, 32);
    sha512_update(&ctx, pk, 32);
    sha512_update(&ctx, msg, msg_len);
    sha512_final(&ctx, hram);
    sc_reduce(hram);

    /* S = (r + hram * a) mod l — stored in sig[32..63] */
    sc_muladd(sig + 32, hram, a, nonce);
}

/*
 * crypto_ed25519_check — verify an Ed25519 signature.
 *
 * Steps (RFC 8032 §5.1.7):
 *   1. Decompress A (negate for the verification equation)
 *   2. H = SHA-512(R || A || msg) mod l
 *   3. Compute [S]B - [H]A and compare with R
 *
 * Returns 0 on success, -1 on failure.
 */
int crypto_ed25519_check(const uint8_t *sig,
                         const uint8_t *msg, size_t msg_len,
                         const uint8_t *pk)
{
    uint8_t h[64];
    uint8_t hreduced[64];
    uint8_t rcheck[32];
    gf p[4], q[4];
    sha512_ctx ctx;

    /* Reject S >= l (top 3 bits of sig[63] must be 0) */
    if (sig[63] & 0xe0) return -1;

    /* Decompress public key (with negation for the check equation) */
    if (pt_unpackneg(q, pk) != 0) return -1;

    /* H = SHA-512(R[32] || pk[32] || msg) */
    sha512_init(&ctx);
    sha512_update(&ctx, sig, 32);
    sha512_update(&ctx, pk, 32);
    sha512_update(&ctx, msg, msg_len);
    sha512_final(&ctx, h);

    mc_memcpy(hreduced, h, 64);
    sc_reduce(hreduced);

    /* p = hreduced * (-A) */
    pt_scalarmult(p, q, hreduced);

    /* q = S * B */
    pt_scalarbase(q, sig + 32);

    /* p = q + p  ==  [S]B + (-[H]A)  ==  [S]B - [H]A */
    pt_add(p, q);

    /* Pack result and compare against R = sig[0..31] */
    pt_pack(rcheck, p);
    return (mc_memcmp(rcheck, sig, 32) == 0) ? 0 : -1;
}
