/*
 * verify.c — Ed25519 + SHA-256 WASM module verifier for agentOS (seL4/Microkit)
 *
 * Completely self-contained: no libc, no system headers beyond freestanding
 * <stdint.h> and <stdbool.h>, plus the kernel's own string_bare.h.
 *
 * We do NOT include agentos.h here because it transitively pulls in
 * <microkit.h> → <sel4/arch/syscalls.h>, which uses GNU asm register syntax
 log_drain_write(15, 15, "");
 * we forward-declare it directly — it is a plain external C function.
 *
 * SHA-256 and SHA-512 implemented from scratch.
 * Ed25519 adapted from TweetNaCl (public domain, D.J. Bernstein et al.).
 *
 * Compile test:
 *   clang -target riscv64-unknown-elf -nostdlib -ffreestanding -std=c11 -Iinclude verify.c
 */

#include "agentos.h"
#include <stdint.h>
#include <stdbool.h>
#include "string_bare.h"
#include "verify.h"

/* console_log is declared in agentos.h (included above). */

/* =========================================================================
 * VIBE_VERIFY_MODE
 * 0 (default) = dev mode:  warn but allow load
 * 1           = prod mode: reject bad/missing sig
 * ========================================================================= */
#ifndef VIBE_VERIFY_MODE
#define VIBE_VERIFY_MODE 0
#endif

/* =========================================================================
 * Section 1: SHA-256
 * ========================================================================= */

#define ROTR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define SHR32(x, n)  ((x) >> (n))

#define CH(x, y, z)   (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z)  (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x)        (ROTR32(x,  2) ^ ROTR32(x, 13) ^ ROTR32(x, 22))
#define EP1(x)        (ROTR32(x,  6) ^ ROTR32(x, 11) ^ ROTR32(x, 25))
#define SIG0(x)       (ROTR32(x,  7) ^ ROTR32(x, 18) ^ SHR32(x,   3))
#define SIG1(x)       (ROTR32(x, 17) ^ ROTR32(x, 19) ^ SHR32(x,  10))

static const uint32_t SHA256_K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static const uint32_t SHA256_H0[8] = {
    0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
};

typedef struct {
    uint32_t state[8];
    uint32_t count[2];   /* bit count: count[0] = low 32 bits, count[1] = high */
    uint8_t  buf[64];
} sha256_ctx_t;

static void sha256_transform(sha256_ctx_t *ctx, const uint8_t data[64])
{
    uint32_t a, b, c, d, e, f, g, h, t1, t2, w[64];
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)data[i*4+0] << 24)
             | ((uint32_t)data[i*4+1] << 16)
             | ((uint32_t)data[i*4+2] <<  8)
             | ((uint32_t)data[i*4+3]      );
    }
    for (i = 16; i < 64; i++) {
        w[i] = SIG1(w[i-2]) + w[i-7] + SIG0(w[i-15]) + w[i-16];
    }

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (i = 0; i < 64; i++) {
        t1 = h + EP1(e) + CH(e,f,g) + SHA256_K[i] + w[i];
        t2 = EP0(a) + MAJ(a,b,c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(sha256_ctx_t *ctx)
{
    memcpy(ctx->state, SHA256_H0, sizeof(SHA256_H0));
    ctx->count[0] = 0;
    ctx->count[1] = 0;
}

static void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len)
{
    size_t i;
    uint32_t used = (ctx->count[0] >> 3) & 0x3f; /* bytes used in buf */

    /* Update bit count */
    uint32_t bits_lo = (uint32_t)(len << 3);
    uint32_t bits_hi = (uint32_t)(len >> 29);
    ctx->count[0] += bits_lo;
    if (ctx->count[0] < bits_lo) ctx->count[1]++;
    ctx->count[1] += bits_hi;

    for (i = 0; i < len; i++) {
        ctx->buf[used++] = data[i];
        if (used == 64) {
            sha256_transform(ctx, ctx->buf);
            used = 0;
        }
    }
}

static void sha256_final(sha256_ctx_t *ctx, uint8_t out[32])
{
    uint32_t used = (ctx->count[0] >> 3) & 0x3f;
    int i;

    ctx->buf[used++] = 0x80;
    if (used > 56) {
        while (used < 64) ctx->buf[used++] = 0x00;
        sha256_transform(ctx, ctx->buf);
        used = 0;
    }
    while (used < 56) ctx->buf[used++] = 0x00;

    /* Append bit count (big-endian 64-bit) */
    ctx->buf[56] = (uint8_t)(ctx->count[1] >> 24);
    ctx->buf[57] = (uint8_t)(ctx->count[1] >> 16);
    ctx->buf[58] = (uint8_t)(ctx->count[1] >>  8);
    ctx->buf[59] = (uint8_t)(ctx->count[1]      );
    ctx->buf[60] = (uint8_t)(ctx->count[0] >> 24);
    ctx->buf[61] = (uint8_t)(ctx->count[0] >> 16);
    ctx->buf[62] = (uint8_t)(ctx->count[0] >>  8);
    ctx->buf[63] = (uint8_t)(ctx->count[0]      );
    sha256_transform(ctx, ctx->buf);

    for (i = 0; i < 8; i++) {
        out[i*4+0] = (uint8_t)(ctx->state[i] >> 24);
        out[i*4+1] = (uint8_t)(ctx->state[i] >> 16);
        out[i*4+2] = (uint8_t)(ctx->state[i] >>  8);
        out[i*4+3] = (uint8_t)(ctx->state[i]      );
    }
}

static void sha256_hash(uint8_t out[32], const uint8_t *data, size_t len)
{
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}

/* =========================================================================
 * Section 2: SHA-512
 * ========================================================================= */

#define ROTR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))
#define SHR64(x, n)  ((x) >> (n))

#define CH64(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ64(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0_64(x)      (ROTR64(x, 28) ^ ROTR64(x, 34) ^ ROTR64(x, 39))
#define EP1_64(x)      (ROTR64(x, 14) ^ ROTR64(x, 18) ^ ROTR64(x, 41))
#define SIG0_64(x)     (ROTR64(x,  1) ^ ROTR64(x,  8) ^ SHR64(x,   7))
#define SIG1_64(x)     (ROTR64(x, 19) ^ ROTR64(x, 61) ^ SHR64(x,   6))

static const uint64_t SHA512_K[80] = {
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

static const uint64_t SHA512_H0[8] = {
    0x6a09e667f3bcc908ull, 0xbb67ae8584caa73bull,
    0x3c6ef372fe94f82bull, 0xa54ff53a5f1d36f1ull,
    0x510e527fade682d1ull, 0x9b05688c2b3e6c1full,
    0x1f83d9abfb41bd6bull, 0x5be0cd19137e2179ull,
};

typedef struct {
    uint64_t state[8];
    uint64_t count[2];   /* bit count: count[0]=low 64 bits, count[1]=high */
    uint8_t  buf[128];
} sha512_ctx_t;

static void sha512_transform(sha512_ctx_t *ctx, const uint8_t data[128])
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
    for (i = 16; i < 80; i++) {
        w[i] = SIG1_64(w[i-2]) + w[i-7] + SIG0_64(w[i-15]) + w[i-16];
    }

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (i = 0; i < 80; i++) {
        t1 = h + EP1_64(e) + CH64(e,f,g) + SHA512_K[i] + w[i];
        t2 = EP0_64(a) + MAJ64(a,b,c);
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha512_init(sha512_ctx_t *ctx)
{
    memcpy(ctx->state, SHA512_H0, sizeof(SHA512_H0));
    ctx->count[0] = 0;
    ctx->count[1] = 0;
}

static void sha512_update(sha512_ctx_t *ctx, const uint8_t *data, size_t len)
{
    size_t i;
    uint64_t used = (ctx->count[0] >> 3) & 0x7f; /* bytes used in buf */

    /* Update bit count (128-bit) */
    uint64_t bits = (uint64_t)len << 3;
    ctx->count[0] += bits;
    if (ctx->count[0] < bits) ctx->count[1]++;
    ctx->count[1] += (uint64_t)len >> 61;

    for (i = 0; i < len; i++) {
        ctx->buf[used++] = data[i];
        if (used == 128) {
            sha512_transform(ctx, ctx->buf);
            used = 0;
        }
    }
}

static void sha512_final(sha512_ctx_t *ctx, uint8_t out[64])
{
    uint64_t used = (ctx->count[0] >> 3) & 0x7f;
    int i;

    ctx->buf[used++] = 0x80;
    if (used > 112) {
        while (used < 128) ctx->buf[used++] = 0x00;
        sha512_transform(ctx, ctx->buf);
        used = 0;
    }
    while (used < 112) ctx->buf[used++] = 0x00;

    /* Append bit count (big-endian 128-bit: count[1] then count[0]) */
    for (i = 7; i >= 0; i--) ctx->buf[112 + (7-i)] = (uint8_t)(ctx->count[1] >> (i*8));
    for (i = 7; i >= 0; i--) ctx->buf[120 + (7-i)] = (uint8_t)(ctx->count[0] >> (i*8));
    sha512_transform(ctx, ctx->buf);

    for (i = 0; i < 8; i++) {
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
    sha512_ctx_t ctx;
    sha512_init(&ctx);
    sha512_update(&ctx, data, len);
    sha512_final(&ctx, out);
}

/* =========================================================================
 * Section 3: Ed25519 verifier
 * Adapted from TweetNaCl (public domain, D.J. Bernstein et al.)
 * ========================================================================= */

/* GF(2^255-19): 16 × 16-bit limbs, signed 64-bit integers */
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

/* Carry propagation (TweetNaCl car25519) */
static void gf_car(gf o)
{
    int i;
    long long c;
    for (i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        c = o[i] >> 16;
        o[(i+1) * (i<15)] += c - 1 + 37 * (c-1) * (i==15);
        o[i] -= c << 16;
    }
}

static void gf_add(gf o, const gf a, const gf b)
{
    int i;
    for (i = 0; i < 16; i++) o[i] = a[i] + b[i];
}

static void gf_sub(gf o, const gf a, const gf b)
{
    int i;
    for (i = 0; i < 16; i++) o[i] = a[i] - b[i];
}

static void gf_cpy(gf o, const gf a)
{
    int i;
    for (i = 0; i < 16; i++) o[i] = a[i];
}

static void gf_mul(gf o, const gf a, const gf b)
{
    long long t[31];
    int i, j;
    for (i = 0; i < 31; i++) t[i] = 0;
    for (i = 0; i < 16; i++)
        for (j = 0; j < 16; j++)
            t[i+j] += a[i] * b[j];
    for (i = 0; i < 15; i++) t[i] += 38 * t[i+16];
    for (i = 0; i < 16; i++) o[i] = t[i];
    gf_car(o);
    gf_car(o);
}

static void gf_sq(gf o, const gf a)
{
    gf_mul(o, a, a);
}

/* Constant-time conditional swap (TweetNaCl sel25519) */
static void gf_sel(gf p, gf q, int b)
{
    long long t, c = ~(b - 1);
    int i;
    for (i = 0; i < 16; i++) {
        t    = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

/* Pack field element to little-endian 32 bytes */
static void gf_pack(uint8_t o[32], const gf n)
{
    int i, j, b;
    gf m, t;
    gf_cpy(t, n);
    gf_car(t);
    gf_car(t);
    gf_car(t);
    for (j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i-1] >> 16) & 1);
            m[i-1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        gf_sel(t, m, 1-b);
    }
    for (i = 0; i < 16; i++) {
        o[2*i]   = (uint8_t)(t[i]      );
        o[2*i+1] = (uint8_t)(t[i] >>  8);
    }
}

/* Unpack little-endian 32 bytes to field element */
static void gf_unpack(gf o, const uint8_t n[32])
{
    int i;
    for (i = 0; i < 16; i++)
        o[i] = (long long)n[2*i] | ((long long)n[2*i+1] << 8);
    o[15] &= 0x7fff;
}

/* Returns 1 if a != b, 0 if equal */
static int gf_neq(const gf a, const gf b)
{
    uint8_t pa[32], pb[32];
    gf_pack(pa, a);
    gf_pack(pb, b);
    return memcmp(pa, pb, 32) != 0 ? 1 : 0;
}

/* a^(2^252 - 3) mod p, used for square root (TweetNaCl pow2523) */
static void gf_pow2523(gf o, const gf a)
{
    gf c, t0;
    int i;

    gf_sq(c, a);
    gf_mul(t0, c, a);      /* a^3 */
    gf_sq(c, t0);
    gf_mul(t0, c, a);      /* a^7 */
    gf_sq(c, t0);
    gf_sq(c, c);
    gf_mul(t0, c, t0);     /* a^(2^4 - 1) */
    gf_sq(c, t0);
    gf_sq(c, c);
    gf_sq(c, c);
    gf_sq(c, c);
    gf_mul(t0, c, t0);     /* a^(2^8 - 1) */
    gf_sq(c, t0);
    for (i = 1; i < 8; i++) gf_sq(c, c);
    gf_mul(t0, c, t0);     /* a^(2^16 - 1) */
    gf_sq(c, t0);
    for (i = 1; i < 16; i++) gf_sq(c, c);
    gf_mul(t0, c, t0);     /* a^(2^32 - 1) */
    gf_sq(c, t0);
    for (i = 1; i < 32; i++) gf_sq(c, c);
    gf_mul(c, c, t0);      /* a^(2^64 - 1) */
    gf_sq(c, c);
    for (i = 1; i < 32; i++) gf_sq(c, c);
    gf_mul(t0, c, t0);     /* a^(2^128 - 1) */
    gf_sq(c, t0);
    for (i = 1; i < 64; i++) gf_sq(c, c);
    gf_mul(c, c, t0);      /* a^(2^192 - 1) */
    gf_sq(c, c);
    gf_sq(c, c);
    gf_mul(o, c, a);       /* a^(2^252 - 3) */
}

/* Extended Twisted Edwards point addition (TweetNaCl add) */
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

/* Conditional swap of two points */
static void pt_cswap(gf p[4], gf q[4], uint8_t b)
{
    int i;
    for (i = 0; i < 4; i++) gf_sel(p[i], q[i], (int)b);
}

/* Scalar multiplication on Ed25519 (double-and-add, TweetNaCl scalarmult) */
static void pt_scalarmult(gf p[4], gf q[4], const uint8_t s[32])
{
    int i, b;
    gf_cpy(p[0], gf0);
    gf_cpy(p[1], gf1);
    gf_cpy(p[2], gf1);
    gf_cpy(p[3], gf0);

    for (i = 255; i >= 0; i--) {
        b = (int)((s[i/8] >> (i&7)) & 1);
        pt_cswap(p, q, (uint8_t)b);
        pt_add(q, p);
        pt_add(p, p);
        pt_cswap(p, q, (uint8_t)b);
    }
}

/* Scalar multiply with base point */
static void pt_scalarbase(gf p[4], const uint8_t s[32])
{
    gf q[4];
    gf_cpy(q[0], gfX);
    gf_cpy(q[1], gfY);
    gf_cpy(q[2], gf1);
    gf_mul(q[3], gfX, gfY);
    pt_scalarmult(p, q, s);
}

/* Pack point to compressed 32-byte encoding */
static void pt_pack(uint8_t r[32], gf p[4])
{
    gf tx, ty, zi;
    uint8_t xb[32];

    /* Compute zi = 1/p[2] via Fermat: zi = p[2]^(p-2) */
    /* For p = 2^255-19, p-2 = 2^255-21. We reuse pow2523 which gives
     * a^(2^252-3). Then (p-2) = (2^252-3)*4 + ... use standard approach:
     * invert via p[2]^(p-2) = p[2]^(2^255-21).
     * TweetNaCl uses: pow2523(zi,p[2]); sq(zi,zi); sq(zi,zi); mul(zi,zi,p[2]);
     * That gives p[2]^(4*(2^252-3)+1) = p[2]^(2^254-11) which is NOT p-2.
     * Correct TweetNaCl inversion: use modular exponentiation.
     *
     * Actually TweetNaCl's pack25519 / pt_pack equivalent computes
     *   recip(zi, p[2]) which is z^(2^255-21) via a specific ladder.
     * Use the standard inversion ladder: z^(p-2) = z^(2^255-21).
     */
    /* Standard field inversion for p=2^255-19: z^(p-2) = z^(2^255-21) */
    {
        gf z2, z9, z11, z2_5_0, z2_10_0, z2_20_0, z2_50_0, z2_100_0, t0, t1;
        int i;

        gf_sq(z2, p[2]);                         /* z^2 */
        gf_sq(t0, z2);                           /* z^4 */
        gf_sq(t0, t0);                           /* z^8 */
        gf_mul(z9, t0, p[2]);                    /* z^9 */
        gf_mul(z11, z9, z2);                     /* z^11 */
        gf_sq(t0, z11);                          /* z^22 */
        gf_mul(z2_5_0, t0, z9);                  /* z^(2^5-1) = z^31 */

        gf_sq(t0, z2_5_0);
        for (i = 1; i < 5; i++) gf_sq(t0, t0);  /* z^(2^10-2^5) */
        gf_mul(z2_10_0, t0, z2_5_0);             /* z^(2^10-1) */

        gf_sq(t0, z2_10_0);
        for (i = 1; i < 10; i++) gf_sq(t0, t0); /* z^(2^20-2^10) */
        gf_mul(z2_20_0, t0, z2_10_0);            /* z^(2^20-1) */

        gf_sq(t0, z2_20_0);
        for (i = 1; i < 20; i++) gf_sq(t0, t0); /* z^(2^40-2^20) */
        gf_mul(t0, t0, z2_20_0);                 /* z^(2^40-1) */

        gf_sq(t0, t0);
        for (i = 1; i < 10; i++) gf_sq(t0, t0); /* z^(2^50-2^10) */
        gf_mul(z2_50_0, t0, z2_10_0);            /* z^(2^50-1) */

        gf_sq(t0, z2_50_0);
        for (i = 1; i < 50; i++) gf_sq(t0, t0); /* z^(2^100-2^50) */
        gf_mul(z2_100_0, t0, z2_50_0);           /* z^(2^100-1) */

        gf_sq(t0, z2_100_0);
        for (i = 1; i < 100; i++) gf_sq(t0, t0);/* z^(2^200-2^100) */
        gf_mul(t0, t0, z2_100_0);                /* z^(2^200-1) */

        gf_sq(t0, t0);
        for (i = 1; i < 50; i++) gf_sq(t0, t0); /* z^(2^250-2^50) */
        gf_mul(t0, t0, z2_50_0);                 /* z^(2^250-1) */

        gf_sq(t0, t0);
        gf_sq(t0, t0);
        gf_sq(t0, t0);
        gf_sq(t0, t0);
        gf_sq(t0, t0);                           /* z^(2^255-2^5) */
        gf_mul(zi, t0, z11);                     /* z^(2^255-21) = z^(p-2) */
        (void)t1;
    }

    gf_mul(tx, p[0], zi);
    gf_mul(ty, p[1], zi);
    gf_pack(r, ty);
    gf_pack(xb, tx);
    r[31] ^= (xb[0] & 1) << 7;
}

/* Decode compressed point and negate x (for verification).
 * Returns 0 on success, -1 if not on curve. */
static int pt_unpackneg(gf r[4], const uint8_t p[32])
{
    gf t, chk, num, den, den2, den4, den6;
    uint8_t sign, pb[32];
    int i;

    sign = p[31] >> 7;
    gf_unpack(r[1], p);          /* y */
    gf_cpy(r[2], gf1);           /* z = 1 */

    /* num = y^2 - 1, den = d*y^2 + 1 */
    gf_sq(num, r[1]);
    gf_mul(den, num, D);
    gf_sub(num, num, gf1);
    gf_add(den, den, gf1);

    gf_sq(den2, den);
    gf_sq(den4, den2);
    gf_mul(den6, den4, den2);
    gf_mul(t, den6, num);
    gf_mul(t, t, den);           /* t = num * den^7 */

    gf_pow2523(t, t);            /* t = (num * den^7)^((p-5)/8) */
    gf_mul(t, t, num);
    gf_mul(t, t, den);
    gf_mul(t, t, den);
    gf_mul(r[0], t, den);        /* x = num * den^3 * (num*den^7)^((p-5)/8) */

    gf_sq(chk, r[0]);
    gf_mul(chk, chk, den);
    if (gf_neq(chk, num)) {
        gf_mul(r[0], r[0], gfI);
    }

    gf_sq(chk, r[0]);
    gf_mul(chk, chk, den);
    if (gf_neq(chk, num)) return -1;

    /* If parity of x doesn't match sign bit, negate x */
    gf_pack(pb, r[0]);
    if ((int)(pb[0] & 1) != (int)sign) {
        gf_sub(r[0], gf0, r[0]);
    }

    gf_mul(r[3], r[0], r[1]);   /* T = x*y */
    (void)i;
    return 0;
}

/* Scalar reduction mod l = 2^252 + 27742317777372353535851937790883648493
 * (TweetNaCl modl, operates on 64-byte buffer) */
static void sc_reduce(uint8_t r[64])
{
    long long x[64];
    long long carry;
    int i;

    for (i = 0; i < 64; i++) x[i] = (long long)(uint8_t)r[i];
    for (i = 0; i < 64; i++) r[i] = 0;

    /* Reduce the high 32 bytes using l */
    x[23] += x[63] * 666643;
    x[24] += x[63] * 470296;
    x[25] += x[63] * 654183;
    x[26] -= x[63] * 997805;
    x[27] += x[63] * 136657;
    x[28] -= x[63] * 683901;
    x[63]  = 0;

    x[22] += x[62] * 666643;
    x[23] += x[62] * 470296;
    x[24] += x[62] * 654183;
    x[25] -= x[62] * 997805;
    x[26] += x[62] * 136657;
    x[27] -= x[62] * 683901;
    x[62]  = 0;

    x[21] += x[61] * 666643;
    x[22] += x[61] * 470296;
    x[23] += x[61] * 654183;
    x[24] -= x[61] * 997805;
    x[25] += x[61] * 136657;
    x[26] -= x[61] * 683901;
    x[61]  = 0;

    x[20] += x[60] * 666643;
    x[21] += x[60] * 470296;
    x[22] += x[60] * 654183;
    x[23] -= x[60] * 997805;
    x[24] += x[60] * 136657;
    x[25] -= x[60] * 683901;
    x[60]  = 0;

    x[19] += x[59] * 666643;
    x[20] += x[59] * 470296;
    x[21] += x[59] * 654183;
    x[22] -= x[59] * 997805;
    x[23] += x[59] * 136657;
    x[24] -= x[59] * 683901;
    x[59]  = 0;

    x[18] += x[58] * 666643;
    x[19] += x[58] * 470296;
    x[20] += x[58] * 654183;
    x[21] -= x[58] * 997805;
    x[22] += x[58] * 136657;
    x[23] -= x[58] * 683901;
    x[58]  = 0;

    x[17] += x[57] * 666643;
    x[18] += x[57] * 470296;
    x[19] += x[57] * 654183;
    x[20] -= x[57] * 997805;
    x[21] += x[57] * 136657;
    x[22] -= x[57] * 683901;
    x[57]  = 0;

    x[16] += x[56] * 666643;
    x[17] += x[56] * 470296;
    x[18] += x[56] * 654183;
    x[19] -= x[56] * 997805;
    x[20] += x[56] * 136657;
    x[21] -= x[56] * 683901;
    x[56]  = 0;

    x[15] += x[55] * 666643;
    x[16] += x[55] * 470296;
    x[17] += x[55] * 654183;
    x[18] -= x[55] * 997805;
    x[19] += x[55] * 136657;
    x[20] -= x[55] * 683901;
    x[55]  = 0;

    x[14] += x[54] * 666643;
    x[15] += x[54] * 470296;
    x[16] += x[54] * 654183;
    x[17] -= x[54] * 997805;
    x[18] += x[54] * 136657;
    x[19] -= x[54] * 683901;
    x[54]  = 0;

    x[13] += x[53] * 666643;
    x[14] += x[53] * 470296;
    x[15] += x[53] * 654183;
    x[16] -= x[53] * 997805;
    x[17] += x[53] * 136657;
    x[18] -= x[53] * 683901;
    x[53]  = 0;

    x[12] += x[52] * 666643;
    x[13] += x[52] * 470296;
    x[14] += x[52] * 654183;
    x[15] -= x[52] * 997805;
    x[16] += x[52] * 136657;
    x[17] -= x[52] * 683901;
    x[52]  = 0;

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

    x[11] += x[23] * 666643;
    x[12] += x[23] * 470296;
    x[13] += x[23] * 654183;
    x[14] -= x[23] * 997805;
    x[15] += x[23] * 136657;
    x[16] -= x[23] * 683901;
    x[23]  = 0;

    x[10] += x[22] * 666643;
    x[11] += x[22] * 470296;
    x[12] += x[22] * 654183;
    x[13] -= x[22] * 997805;
    x[14] += x[22] * 136657;
    x[15] -= x[22] * 683901;
    x[22]  = 0;

    x[ 9] += x[21] * 666643;
    x[10] += x[21] * 470296;
    x[11] += x[21] * 654183;
    x[12] -= x[21] * 997805;
    x[13] += x[21] * 136657;
    x[14] -= x[21] * 683901;
    x[21]  = 0;

    x[ 8] += x[20] * 666643;
    x[ 9] += x[20] * 470296;
    x[10] += x[20] * 654183;
    x[11] -= x[20] * 997805;
    x[12] += x[20] * 136657;
    x[13] -= x[20] * 683901;
    x[20]  = 0;

    x[ 7] += x[19] * 666643;
    x[ 8] += x[19] * 470296;
    x[ 9] += x[19] * 654183;
    x[10] -= x[19] * 997805;
    x[11] += x[19] * 136657;
    x[12] -= x[19] * 683901;
    x[19]  = 0;

    x[ 6] += x[18] * 666643;
    x[ 7] += x[18] * 470296;
    x[ 8] += x[18] * 654183;
    x[ 9] -= x[18] * 997805;
    x[10] += x[18] * 136657;
    x[11] -= x[18] * 683901;
    x[18]  = 0;

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
    (void)carry;

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

/* Ed25519 verification.
 * Returns 0 if signature is valid, -1 if invalid.
 * sig[64] = R_bytes[32] || S_bytes[32]
 * msg[32] = SHA-256 hash of the WASM content (the "message" being signed)
 * pk[32]  = Ed25519 public key
 */
/* ed25519_verify_hash: internal helper — verifies sig over a fixed 32-byte hash */
static int ed25519_verify_hash(const uint8_t sig[64], const uint8_t msg[32],
                               const uint8_t pk[32])
{
    uint8_t h[64];          /* SHA-512 output */
    uint8_t hreduced[64];   /* h mod l */
    uint8_t t[32];          /* packed result point */
    gf p[4], q[4];
    sha512_ctx_t ctx;
    int i;

    /* Decode public key (-A) */
    if (pt_unpackneg(q, pk) != 0) return -1;

    /* H = SHA-512(R[32] || pk[32] || msg[32]) */
    sha512_init(&ctx);
    sha512_update(&ctx, sig,  32);   /* R_bytes */
    sha512_update(&ctx, pk,   32);
    sha512_update(&ctx, msg,  32);
    sha512_final(&ctx, h);

    /* Copy h to hreduced and reduce mod l */
    for (i = 0; i < 64; i++) hreduced[i] = h[i];
    sc_reduce(hreduced);

    /* p = hreduced * (-A)  →  -h*A */
    pt_scalarmult(p, q, hreduced);

    /* q = S * B  (base-point scalar mult with S = sig[32..63]) */
    pt_scalarbase(q, sig + 32);

    /* p = p + q  →  S*B - h*A  (same as S*B + (-h*A)) */
    pt_add(p, q);

    /* Pack result */
    pt_pack(t, p);

    /* Check t == R_bytes = sig[0..31] */
    return (memcmp(t, sig, 32) == 0) ? 0 : -1;
}

/* ed25519_verify — the exported symbol is provided by ed25519_verify.c.
 * Internal code uses ed25519_verify_hash (static, above) directly.
 * Declaration removed to avoid duplicate-symbol conflict with ed25519_verify.c. */

/* =========================================================================
 * Section 4: WASM custom section scanner
 * ========================================================================= */

/* Read a LEB128-encoded unsigned integer from buf[0..max).
 * Returns number of bytes consumed, or 0 on error.
 * *out receives the decoded value.
 */
static size_t leb128_read(const uint8_t *buf, size_t max, uint32_t *out)
{
    uint32_t val = 0;
    unsigned shift = 0;
    size_t i;

    for (i = 0; i < max && i < 5; i++) {
        uint8_t b = buf[i];
        val |= (uint32_t)(b & 0x7f) << shift;
        shift += 7;
        if ((b & 0x80) == 0) {
            *out = val;
            return i + 1;
        }
    }
    return 0; /* error or overflow */
}

/* Name of the custom section containing our signature payload */
#define AGENTOS_SIG_SECTION_NAME   "agentos.signature"
#define AGENTOS_SIG_SECTION_NAMELEN 17u

/* Layout of the 128-byte payload inside the custom section body:
 *   [0  .. 31]  pubkey     (Ed25519 public key, 32 bytes)
 *   [32 .. 95]  sig        (Ed25519 signature,  64 bytes)
 *   [96 ..127]  sha256     (SHA-256 of WASM without this section, 32 bytes)
 */
#define SIG_PAYLOAD_SIZE    128u
#define SIG_OFF_PUBKEY       0u
#define SIG_OFF_SIG         32u
#define SIG_OFF_SHA256      96u

/* Find the "agentos.signature" custom section (id == 0x00).
 *
 * out_off:       byte offset of the section-id (0x00) byte within wasm
 * out_sec_total: total byte size of the section (id + LEB128 size field + body)
 * out_payload:   pointer to the 128-byte payload inside wasm
 *
 * Returns true if found, false otherwise.
 */
static bool wasm_find_sig_section(const uint8_t *wasm, size_t len,
                                   size_t *out_off, size_t *out_sec_total,
                                   const uint8_t **out_payload)
{
    size_t pos;
    uint32_t sec_size;
    size_t leb_bytes;

    /* WASM magic: \0asm + version 0x01000000 */
    if (len < 8) return false;
    if (wasm[0] != 0x00 || wasm[1] != 0x61 ||
        wasm[2] != 0x73 || wasm[3] != 0x6d) return false;

    pos = 8; /* skip magic + version */

    while (pos < len) {
        uint8_t sec_id;
        size_t  body_start;
        uint32_t name_len;
        size_t   name_leb_bytes;

        if (pos + 1 > len) break;
        sec_id = wasm[pos];

        leb_bytes = leb128_read(wasm + pos + 1, len - pos - 1, &sec_size);
        if (leb_bytes == 0) break;

        body_start = pos + 1 + leb_bytes;
        if (body_start + sec_size > len) break;

        if (sec_id == 0x00) {
            /* Custom section — read name */
            if (sec_size < 1) goto next;

            name_leb_bytes = leb128_read(wasm + body_start,
                                          len - body_start, &name_len);
            if (name_leb_bytes == 0) goto next;

            if (name_len == AGENTOS_SIG_SECTION_NAMELEN &&
                body_start + name_leb_bytes + name_len + SIG_PAYLOAD_SIZE <= len &&
                body_start + name_leb_bytes + name_len + SIG_PAYLOAD_SIZE
                    <= body_start + sec_size)
            {
                /* Compare name bytes */
                const uint8_t *name_start = wasm + body_start + name_leb_bytes;
                if (memcmp(name_start, AGENTOS_SIG_SECTION_NAME,
                           AGENTOS_SIG_SECTION_NAMELEN) == 0)
                {
                    *out_off       = pos;
                    *out_sec_total = 1 + leb_bytes + (size_t)sec_size;
                    *out_payload   = name_start + AGENTOS_SIG_SECTION_NAMELEN;
                    return true;
                }
            }
        }

    next:
        pos = body_start + (size_t)sec_size;
    }

    return false;
}

/* =========================================================================
 * Section 5: WASM capabilities manifest verification
 * ========================================================================= */

/*
 * find_wasm_custom_section - locate a named WASM custom section (type 0x00).
 *
 * Walks the WASM binary's section sequence starting after the 8-byte header.
 * For each custom section, decodes the LEB128-prefixed name and compares it
 * against `name` (length `name_len`).  On success, returns a pointer to the
 * first byte of the section *body* (after the name field) and writes the
 * body length to *out_len.  Returns NULL if not found or on parse error.
 *
 * This helper is used by verify_capabilities_manifest() and is intentionally
 * separate from wasm_find_sig_section() which has a specialised layout for
 * the 128-byte agentos.signature payload.
 */
static const uint8_t *find_wasm_custom_section(
    const uint8_t *wasm, size_t wasm_len,
    const char *name, size_t *out_len)
{
    size_t name_len = strlen(name);

    if (wasm_len < 8) return NULL;
    const uint8_t *p   = wasm + 8;   /* skip WASM magic + version */
    const uint8_t *end = wasm + wasm_len;

    while (p < end) {
        if (p + 1 > end) break;
        uint8_t section_type = *p++;

        uint32_t section_size = 0;
        size_t leb = leb128_read(p, (size_t)(end - p), &section_size);
        if (leb == 0) break;
        p += leb;

        const uint8_t *section_start = p;

        if (section_type == 0x00 && section_size > name_len + 1) {
            uint32_t n_len = 0;
            size_t nleb = leb128_read(p, (size_t)(end - p), &n_len);
            if (nleb == 0) goto next_section;
            const uint8_t *np = p + nleb;

            if (n_len == (uint32_t)name_len && np + n_len <= end &&
                memcmp(np, name, name_len) == 0) {
                const uint8_t *body = np + n_len;
                size_t body_len = (size_t)section_size - (size_t)(np + n_len - section_start);
                *out_len = body_len;
                return body;
            }
        }

    next_section:
        p = section_start + (size_t)section_size;
    }

    return NULL;
}

/*
 * verify_capabilities_manifest - verify that the agentos.capabilities
 * custom section hash matches the agentos.cap_signature section.
 *
 * The signing model:
 *   agentos.cap_signature = SHA-256(agentos.capabilities section bytes)
 *   signed with the agent issuer's ED25519 key (stored in agentos.signature)
 *
 * For the sim layer (no hardware key), we verify the SHA-256 hash only.
 * Returns  0 on success,
 *         -1 if required sections are missing,
 *         -2 if hash mismatch.
 */
int verify_capabilities_manifest(const uint8_t *wasm, size_t wasm_len)
{
    size_t caps_len   = 0;
    size_t sig_len    = 0;
    uint8_t computed[32];

    /* 1. Find agentos.capabilities section */
    const uint8_t *caps_body = find_wasm_custom_section(
        wasm, wasm_len, "agentos.capabilities", &caps_len);
    if (!caps_body) return -1;

    /* 2. Find agentos.cap_signature section (must be exactly 32 bytes) */
    const uint8_t *cap_sig = find_wasm_custom_section(
        wasm, wasm_len, "agentos.cap_signature", &sig_len);
    if (!cap_sig || sig_len < 32) return -1;

    /* 3. Compute SHA-256 of the capabilities section body bytes */
    sha256_hash(computed, caps_body, caps_len);

    /* 4. Compare against the stored cap_signature digest */
    if (memcmp(computed, cap_sig, 32) != 0) return -2;

    return 0;
}

/* =========================================================================
 * Section 6: Public API
 * ========================================================================= */

bool vibe_verify_module(const uint8_t *wasm, size_t len,
                        const uint8_t *trusted_pubkey)
{
    size_t sig_off, sig_sec_total;
    const uint8_t *payload;
    const uint8_t *sec_pubkey;
    const uint8_t *sec_sig;
    const uint8_t *sec_sha256;
    uint8_t computed_sha256[32];
    sha256_ctx_t sha_ctx;
    int ed_result;

    /* 1. Locate signature section */
    if (!wasm_find_sig_section(wasm, len, &sig_off, &sig_sec_total, &payload)) {
        log_drain_write(15, 15, "[vibe_verify] WARNING: no agentos.signature section found\n");
#if VIBE_VERIFY_MODE
        return false;
#else
        return true;
#endif
    }

    /* 3. Extract fields from 128-byte payload */
    sec_pubkey = payload + SIG_OFF_PUBKEY;   /* [0..31]  */
    sec_sig    = payload + SIG_OFF_SIG;      /* [32..95] */
    sec_sha256 = payload + SIG_OFF_SHA256;   /* [96..127] */

    /* 4. Check trusted_pubkey matches section pubkey (if caller provides one) */
    if (trusted_pubkey != NULL) {
        if (memcmp(trusted_pubkey, sec_pubkey, 32) != 0) {
            log_drain_write(15, 15, "[vibe_verify] ERROR: pubkey mismatch (untrusted module)\n");
#if VIBE_VERIFY_MODE
            return false;
#else
            log_drain_write(15, 15, "[vibe_verify] WARNING: pubkey mismatch — dev mode, allowing\n");
            return true;
#endif
        }
    }

    /* 5. Compute SHA-256 of wasm bytes, skipping the signature section.
     *    Hash wasm[0 .. sig_off-1] then wasm[sig_off+sig_sec_total .. len-1].
     */
    sha256_init(&sha_ctx);
    if (sig_off > 0) {
        sha256_update(&sha_ctx, wasm, sig_off);
    }
    {
        size_t tail_start = sig_off + sig_sec_total;
        if (tail_start < len) {
            sha256_update(&sha_ctx, wasm + tail_start, len - tail_start);
        }
    }
    sha256_final(&sha_ctx, computed_sha256);

    /* 6. Compare stored SHA-256 vs computed */
    if (memcmp(sec_sha256, computed_sha256, 32) != 0) {
        log_drain_write(15, 15, "[vibe_verify] ERROR: WASM content hash mismatch\n");
#if VIBE_VERIFY_MODE
        return false;
#else
        log_drain_write(15, 15, "[vibe_verify] WARNING: hash mismatch — dev mode, allowing\n");
        return true;
#endif
    }

    /* 7. Ed25519 signature check: sign(sha256_of_wasm, signing_key)
     *    ed25519_verify(sig[64], msg[32], pk[32])
     *    msg = computed_sha256 (same as sec_sha256 after step 6 passes)
     */
    ed_result = ed25519_verify_hash(sec_sig, computed_sha256, sec_pubkey);
    if (ed_result != 0) {
        log_drain_write(15, 15, "[vibe_verify] ERROR: Ed25519 signature invalid\n");
#if VIBE_VERIFY_MODE
        return false;
#else
        log_drain_write(15, 15, "[vibe_verify] WARNING: bad Ed25519 sig — dev mode, allowing\n");
        return true;
#endif
    }

    /* 8. All checks passed */
    log_drain_write(15, 15, "[vibe_verify] OK: module signature verified\n");
    return true;
}
