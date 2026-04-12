/*
 * agentOS Boot Integrity Measurement
 *
 * Implements a lightweight TPM-inspired boot attestation chain for
 * agentOS Protection Domains.  At boot, each PD measures its own
 * program image hash (SHA-256 of the code region) and extends a
 * running Platform Configuration Register (PCR) maintained in the
 * controller PD.  The final PCR value is signed and stored in AgentFS
 * as the Boot Attestation Report.
 *
 * Design
 * ──────
 * PCR extend operation (like TPM PCR_Extend):
 *   PCR[new] = SHA-256(PCR[old] || new_measurement)
 *
 * Each PD calls OP_BOOT_MEASURE from its init() with its code hash.
 * The controller accumulates measurements in order into PCR[].
 * After all PDs have reported, controller runs OP_BOOT_SEAL to sign
 * the final PCR value and publish to AgentFS.
 *
 * The sign.c module (Ed25519) handles the signing.
 *
 * PCR bank:
 *   PCR[0]  — bootloader / microkit firmware hash
 *   PCR[1]  — controller image hash
 *   PCR[2]  — event_bus
 *   PCR[3]  — init_agent
 *   PCR[4]  — agentfs
 *   PCR[5]  — vibe_engine
 *   PCR[6]  — console_mux
 *   PCR[7]  — mem_profiler
 *   PCR[8..N-1] — worker_0..worker_7, swap_slots, perf_counters, time_partition
 *   PCR_AGGR — running aggregate (all measurements extended in)
 *
 * Messages (MR0 opcode):
 *   OP_BOOT_MEASURE (0xB0): MR1=pd_id, MR2..MR9=sha256[0..7] (8×u32) → ok
 *   OP_BOOT_SEAL    (0xB1): finalize and sign PCR_AGGR → quote_len in MR0
 *   OP_BOOT_QUOTE   (0xB2): return the latest boot quote (first 28 bytes in MRs)
 *   OP_BOOT_VERIFY  (0xB3): verify a presented PCR value (for remote attestation)
 *   OP_BOOT_RESET   (0xB4): clear (for test harnesses)
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "monocypher.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

/* ── Configuration ─────────────────────────────────────────────────────── */

#define BOOT_INTEGRITY_PCR_COUNT  32
#define BOOT_INTEGRITY_HASH_LEN   32    /* SHA-256 = 32 bytes */
#define BOOT_QUOTE_BUF_LEN        512   /* ATTEST\t<seq>\tPCR[i]=<hex>\n ... */

/* Opcodes */
#define OP_BOOT_MEASURE   0xB0
#define OP_BOOT_SEAL      0xB1
#define OP_BOOT_QUOTE     0xB2
#define OP_BOOT_VERIFY    0xB3
#define OP_BOOT_RESET     0xB4

/* ── Internal state ────────────────────────────────────────────────────── */

typedef struct {
    uint8_t  hash[BOOT_INTEGRITY_HASH_LEN];
    uint32_t pd_id;
    bool     measured;
} BootPCR;

static BootPCR   pcr_bank[BOOT_INTEGRITY_PCR_COUNT];
static uint8_t   pcr_aggr[BOOT_INTEGRITY_HASH_LEN];   /* aggregate PCR */
static bool      pcr_sealed    = false;
static uint32_t  measure_count = 0;
static uint8_t   boot_quote[BOOT_QUOTE_BUF_LEN];
static uint32_t  boot_quote_len = 0;

/* ── SHA-256 — minimal in-place implementation ──────────────────────────── */
/*
 * Minimal SHA-256 without external dependencies.
 * Input: message bytes.  Output: 32-byte digest.
 *
 * Based on FIPS 180-4 algorithm; no dynamic allocation.
 */

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static const uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,
    0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
    0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
    0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,
    0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,
    0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
};

static void sha256(const uint8_t *msg, uint32_t len, uint8_t out[32]) {
    uint32_t h[8] = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19,
    };
    /* Pad message */
    uint64_t bit_len = (uint64_t)len * 8;
    uint32_t padded_len = ((len + 9 + 63) / 64) * 64;
    uint8_t padded[512];  /* max 512 bytes input for this impl */
    if (padded_len > sizeof(padded)) {
        /* Message too long for in-stack buffer — write zeros as fallback */
        memset(out, 0, 32);
        return;
    }
    memcpy(padded, msg, len);
    padded[len] = 0x80;
    memset(padded + len + 1, 0, padded_len - len - 1);
    for (int i = 0; i < 8; i++)
        padded[padded_len - 8 + i] = (uint8_t)((bit_len >> (56 - i * 8)) & 0xFF);

    for (uint32_t blk = 0; blk < padded_len; blk += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            w[i]  = ((uint32_t)padded[blk + i*4+0] << 24)
                  | ((uint32_t)padded[blk + i*4+1] << 16)
                  | ((uint32_t)padded[blk + i*4+2] <<  8)
                  | ((uint32_t)padded[blk + i*4+3]);
        }
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ROR32(w[i-15],7) ^ ROR32(w[i-15],18) ^ (w[i-15]>>3);
            uint32_t s1 = ROR32(w[i-2],17) ^ ROR32(w[i-2],19)  ^ (w[i-2]>>10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1  = ROR32(e,6)^ROR32(e,11)^ROR32(e,25);
            uint32_t ch  = (e&f)^(~e&g);
            uint32_t tmp1= hh + S1 + ch + K[i] + w[i];
            uint32_t S0  = ROR32(a,2)^ROR32(a,13)^ROR32(a,22);
            uint32_t maj = (a&b)^(a&c)^(b&c);
            uint32_t tmp2= S0 + maj;
            hh=g; g=f; f=e; e=d+tmp1; d=c; c=b; b=a; a=tmp1+tmp2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d;
        h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    for (int i = 0; i < 8; i++) {
        out[i*4+0] = (h[i]>>24)&0xFF; out[i*4+1] = (h[i]>>16)&0xFF;
        out[i*4+2] = (h[i]>>8)&0xFF;  out[i*4+3] = h[i]&0xFF;
    }
}
#undef ROR32

/* ── PCR extend ────────────────────────────────────────────────────────── */

static void pcr_extend(uint8_t pcr[BOOT_INTEGRITY_HASH_LEN],
                        const uint8_t measurement[BOOT_INTEGRITY_HASH_LEN]) {
    /* PCR[new] = SHA-256(PCR[old] || measurement) */
    uint8_t concat[BOOT_INTEGRITY_HASH_LEN * 2];
    memcpy(concat, pcr, BOOT_INTEGRITY_HASH_LEN);
    memcpy(concat + BOOT_INTEGRITY_HASH_LEN, measurement, BOOT_INTEGRITY_HASH_LEN);
    sha256(concat, sizeof(concat), pcr);
}

/* ── Helpers ───────────────────────────────────────────────────────────── */

static void fmt_hex(const uint8_t *b, uint32_t n, char *out) {
    static const char hx[] = "0123456789abcdef";
    for (uint32_t i = 0; i < n; i++) {
        out[i*2+0] = hx[(b[i]>>4)&0xF];
        out[i*2+1] = hx[b[i]&0xF];
    }
    out[n*2] = '\0';
}

static void build_quote(void) {
    char hex[65];
    uint32_t pos = 0;
    char *buf = (char *)boot_quote;
    uint32_t cap = BOOT_QUOTE_BUF_LEN;
#define Q(fmt, ...) do { \
    int n = snprintf(buf+pos, cap-pos, fmt, ##__VA_ARGS__); \
    if (n > 0 && (uint32_t)n < cap-pos) pos += (uint32_t)n; \
} while(0)
    Q("BOOT_QUOTE\t%u\n", measure_count);
    for (uint32_t i = 0; i < measure_count && i < BOOT_INTEGRITY_PCR_COUNT; i++) {
        if (pcr_bank[i].measured) {
            fmt_hex(pcr_bank[i].hash, BOOT_INTEGRITY_HASH_LEN, hex);
            Q("PCR[%u]\t%u\t%s\n", i, pcr_bank[i].pd_id, hex);
        }
    }
    fmt_hex(pcr_aggr, BOOT_INTEGRITY_HASH_LEN, hex);
    Q("AGGR\t%s\n", hex);
    Q("END\t%u\n", measure_count);
#undef Q
    boot_quote_len = pos;
}

/* ── Microkit entry points ─────────────────────────────────────────────── */

/* Called from controller init() — not a separate PD.  Controller includes
 * boot_integrity.h and calls boot_integrity_init() at startup. */
void boot_integrity_init(void) {
    memset(pcr_bank, 0, sizeof(pcr_bank));
    memset(pcr_aggr, 0, sizeof(pcr_aggr));
    pcr_sealed    = false;
    measure_count = 0;
    boot_quote_len = 0;
    microkit_dbg_puts("[boot_integrity] PCR bank initialized\n");
}

uint32_t boot_integrity_measure(uint32_t pd_id,
                                 const uint8_t measurement[BOOT_INTEGRITY_HASH_LEN]) {
    if (measure_count >= BOOT_INTEGRITY_PCR_COUNT) {
        microkit_dbg_puts("[boot_integrity] PCR bank full\n");
        return 0;
    }
    uint32_t slot = measure_count++;
    pcr_bank[slot].pd_id    = pd_id;
    pcr_bank[slot].measured = true;
    memcpy(pcr_bank[slot].hash, measurement, BOOT_INTEGRITY_HASH_LEN);
    pcr_extend(pcr_aggr, measurement);
    return slot;
}

/*
 * boot_integrity_handle_ppc — called from controller's protected() for
 * OP_BOOT_* opcodes forwarded from init_agent / workers.
 */
microkit_msginfo boot_integrity_handle_ppc(uint32_t op,
                                            microkit_msginfo msginfo __attribute__((unused))) {
    switch (op) {
        case OP_BOOT_MEASURE: {
            /* MR1=pd_id, MR2..MR9 = sha256 as 8×u32 (big-endian order) */
            uint32_t pd_id = (uint32_t)microkit_mr_get(1);
            uint8_t  m[BOOT_INTEGRITY_HASH_LEN];
            for (int i = 0; i < 8; i++) {
                uint32_t w = (uint32_t)microkit_mr_get(2 + i);
                m[i*4+0] = (w>>24)&0xFF; m[i*4+1] = (w>>16)&0xFF;
                m[i*4+2] = (w>>8)&0xFF;  m[i*4+3] = w&0xFF;
            }
            uint32_t slot = boot_integrity_measure(pd_id, m);
            if (slot == 0 && measure_count == 0) {
                microkit_mr_set(0, 0);
                return microkit_msginfo_new(1, 1);
            }
            microkit_mr_set(0, slot);
            return microkit_msginfo_new(0, 1);
        }
        case OP_BOOT_SEAL: {
            build_quote();
            pcr_sealed = true;
            microkit_dbg_puts("[boot_integrity] Boot measurements sealed\n");

            /*
             * Sign the PCR aggregate with an ephemeral Ed25519 key derived
             * deterministically from pcr_aggr.  This is NOT cryptographically
             * secure for production (the private key is exposed in memory), but
             * it provides a correct, verifiable signature for Phase-1 demo and
             * attaches the signing path end-to-end.
             *
             * Production systems should replace sk[] with a key loaded from a
             * secure storage (TPM, eFuse, sealed AgentFS blob, etc.).
             */
            {
                uint8_t sk[32], pk[32], sig[64];

                /* Derive ephemeral sk from PCR aggregate (deterministic) */
                for (int i = 0; i < 32; i++)
                    sk[i] = pcr_aggr[i % BOOT_INTEGRITY_HASH_LEN];

                crypto_ed25519_public_key(pk, sk);
                crypto_ed25519_sign(sig, sk, pk,
                                    boot_quote, boot_quote_len);

                microkit_dbg_puts("[boot_integrity] PCR log signed with ephemeral Ed25519 key\n");
                (void)sig; /* stored to AgentFS in a future phase */
            }

            microkit_mr_set(0, boot_quote_len);
            /* Return first 28 bytes of quote as preview */
            for (int i = 0; i < 7; i++) {
                uint32_t word = 0;
                for (int j = 0; j < 4 && (uint32_t)(i*4+j) < boot_quote_len; j++)
                    word |= (uint32_t)boot_quote[i*4+j] << (j * 8);
                microkit_mr_set(1 + i, word);
            }
            return microkit_msginfo_new(0, 8);
        }
        case OP_BOOT_QUOTE: {
            microkit_mr_set(0, boot_quote_len);
            microkit_mr_set(1, pcr_sealed ? 1 : 0);
            /* Return first 24 bytes (6 u32) of quote */
            for (int i = 0; i < 6; i++) {
                uint32_t word = 0;
                for (int j = 0; j < 4 && (uint32_t)(i*4+j) < boot_quote_len; j++)
                    word |= (uint32_t)boot_quote[i*4+j] << (j * 8);
                microkit_mr_set(2 + i, word);
            }
            return microkit_msginfo_new(0, 8);
        }
        case OP_BOOT_VERIFY: {
            /* MR1..MR8 = expected PCR_AGGR value (8×u32) */
            uint8_t expected[BOOT_INTEGRITY_HASH_LEN];
            for (int i = 0; i < 8; i++) {
                uint32_t w = (uint32_t)microkit_mr_get(1 + i);
                expected[i*4+0]=(w>>24)&0xFF; expected[i*4+1]=(w>>16)&0xFF;
                expected[i*4+2]=(w>>8)&0xFF;  expected[i*4+3]=w&0xFF;
            }
            bool match = (memcmp(pcr_aggr, expected, BOOT_INTEGRITY_HASH_LEN) == 0);
            microkit_mr_set(0, match ? 1 : 0);
            return microkit_msginfo_new(0, 1);
        }
        case OP_BOOT_RESET: {
            boot_integrity_init();
            microkit_mr_set(0, 1);
            return microkit_msginfo_new(0, 1);
        }
        default:
            microkit_mr_set(0, 0xDEAD);
            return microkit_msginfo_new(1, 1);
    }
}
