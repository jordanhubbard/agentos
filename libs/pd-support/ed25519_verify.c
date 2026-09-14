/*
 * ed25519_verify.c — Ed25519 / SHA-512 signature verification for agentOS
 *
 * Phase 1 hash-only verifier replaced by real Ed25519 verification via
 * monocypher.c (Track C).
 *
 * The public API (ed25519_verify, sha512) is unchanged so callers in
 * monitor.c / cap_broker.c do not need to be updated.
 *
 * SHA-512 is still exported directly because boot_integrity.c and the
 * WASM capability hash path depend on it directly.
 */

#include "ed25519_verify.h"
#include "monocypher.h"
#include <stdint.h>

/* ── SHA-512 ─────────────────────────────────────────────────────────────── */
/*
 * Full SHA-512 retained here for the exported sha512() symbol that other
 * translation units reference via ed25519_verify.h.  The monocypher.c
 * implementation uses its own internal copy to remain self-contained.
 */

static const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL,
    0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL,
    0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL, 0x2de92c6f592b0275ULL,
    0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL,
    0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL, 0x27b70a8546d22ffcULL,
    0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL,
    0x92722c851482353bULL, 0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL, 0xd192e819d6ef5218ULL,
    0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL,
    0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL, 0x748f82ee5defb2fcULL,
    0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL,
    0xc67178f2e372532bULL, 0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL, 0x06f067aa72176fbaULL,
    0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL,
    0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

static inline uint64_t rotr64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }
static inline uint64_t Ch(uint64_t x, uint64_t y, uint64_t z)  { return (x & y) ^ (~x & z); }
static inline uint64_t Maj(uint64_t x, uint64_t y, uint64_t z) { return (x & y) ^ (x & z) ^ (y & z); }
static inline uint64_t Sigma0(uint64_t x) { return rotr64(x,28) ^ rotr64(x,34) ^ rotr64(x,39); }
static inline uint64_t Sigma1(uint64_t x) { return rotr64(x,14) ^ rotr64(x,18) ^ rotr64(x,41); }
static inline uint64_t sigma0(uint64_t x) { return rotr64(x, 1) ^ rotr64(x, 8) ^ (x >> 7); }
static inline uint64_t sigma1(uint64_t x) { return rotr64(x,19) ^ rotr64(x,61) ^ (x >> 6); }

static inline uint64_t load_be64(const uint8_t *p) {
    return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
           ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
           ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
           ((uint64_t)p[6] <<  8) | ((uint64_t)p[7]);
}

static inline void store_be64(uint8_t *p, uint64_t v) {
    p[0] = (uint8_t)(v >> 56); p[1] = (uint8_t)(v >> 48);
    p[2] = (uint8_t)(v >> 40); p[3] = (uint8_t)(v >> 32);
    p[4] = (uint8_t)(v >> 24); p[5] = (uint8_t)(v >> 16);
    p[6] = (uint8_t)(v >>  8); p[7] = (uint8_t)(v);
}

void sha512(const uint8_t *msg, uint32_t len, uint8_t hash[64]) {
    uint64_t H[8] = {
        0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
        0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
        0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
        0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL
    };

    uint32_t total = len;
    uint32_t full_blocks = len / 128;
    uint32_t pos = 0;

    for (uint32_t blk = 0; blk < full_blocks; blk++) {
        uint64_t W[80];
        for (int t = 0; t < 16; t++)
            W[t] = load_be64(&msg[pos + t * 8]);
        for (int t = 16; t < 80; t++)
            W[t] = sigma1(W[t-2]) + W[t-7] + sigma0(W[t-15]) + W[t-16];

        uint64_t a=H[0], b=H[1], c=H[2], d=H[3], e=H[4], f=H[5], g=H[6], h=H[7];
        for (int t = 0; t < 80; t++) {
            uint64_t T1 = h + Sigma1(e) + Ch(e,f,g) + K512[t] + W[t];
            uint64_t T2 = Sigma0(a) + Maj(a,b,c);
            h=g; g=f; f=e; e=d+T1; d=c; c=b; b=a; a=T1+T2;
        }
        H[0]+=a; H[1]+=b; H[2]+=c; H[3]+=d; H[4]+=e; H[5]+=f; H[6]+=g; H[7]+=h;
        pos += 128;
    }

    uint8_t final_block[256];
    uint32_t remaining = len - pos;
    uint32_t fb_len = 0;

    for (uint32_t i = 0; i < remaining; i++)
        final_block[fb_len++] = msg[pos + i];
    final_block[fb_len++] = 0x80;
    while ((fb_len % 128) != 112)
        final_block[fb_len++] = 0x00;
    for (int i = 0; i < 8; i++)
        final_block[fb_len++] = 0;
    uint64_t bit_len = (uint64_t)total * 8;
    store_be64(&final_block[fb_len], bit_len);
    fb_len += 8;

    for (uint32_t off = 0; off < fb_len; off += 128) {
        uint64_t W[80];
        for (int t = 0; t < 16; t++)
            W[t] = load_be64(&final_block[off + t * 8]);
        for (int t = 16; t < 80; t++)
            W[t] = sigma1(W[t-2]) + W[t-7] + sigma0(W[t-15]) + W[t-16];

        uint64_t a=H[0], b=H[1], c=H[2], d=H[3], e=H[4], f=H[5], g=H[6], h=H[7];
        for (int t = 0; t < 80; t++) {
            uint64_t T1 = h + Sigma1(e) + Ch(e,f,g) + K512[t] + W[t];
            uint64_t T2 = Sigma0(a) + Maj(a,b,c);
            h=g; g=f; f=e; e=d+T1; d=c; c=b; b=a; a=T1+T2;
        }
        H[0]+=a; H[1]+=b; H[2]+=c; H[3]+=d; H[4]+=e; H[5]+=f; H[6]+=g; H[7]+=h;
    }

    for (int i = 0; i < 8; i++)
        store_be64(&hash[i * 8], H[i]);
}

/* ── Ed25519 verify — delegates to monocypher.c ─────────────────────────── */

int ed25519_verify(const uint8_t sig[64],
                   const uint8_t *msg, uint32_t msg_len,
                   const uint8_t pubkey[32])
{
    return crypto_ed25519_check(sig, msg, (size_t)msg_len, pubkey) == 0 ? 0 : 1;
}
