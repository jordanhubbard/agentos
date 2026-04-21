/*
 * agentOS WireGuard Overlay Network Protection Domain
 *
 * Implements an encrypted agent-to-agent overlay network using the
 * WireGuard protocol (Noise_IKpsk2 with Curve25519, ChaCha20-Poly1305).
 *
 * Priority: 140  (passive="true" — only executes on PPC / timer notify)
 * Shmem:    wg_staging, 128KB, mapped at 0x6000000 (rw)
 *           Layout:
 *             [0x000000..0x00001F]  local private key  (32 bytes, write-once)
 *             [0x000020..0x00003F]  local public key   (32 bytes, derived)
 *             [0x001000..0x001FFF]  peer pubkey staging (OP_WG_ADD_PEER)
 *             [0x002000..0x00FFFF]  TX plaintext staging (OP_WG_SEND)
 *             [0x010000..0x01FFFF]  RX plaintext staging (OP_WG_RECV)
 *
 * Crypto:
 *   Curve25519 key derivation and ChaCha20-Poly1305 AEAD stubs are tagged:
 *     CRYPTO_INTEGRATION_POINT
 *   Wire up by linking Monocypher and calling:
 *     crypto_x25519()             for ECDH key agreement
 *     crypto_aead_lock()          for encryption (ChaCha20-Poly1305)
 *     crypto_aead_unlock()        for decryption
 *
 * Keepalive:
 *   Microkit timer channel (CH_TIMER) fires every ~1s.  wg_net counts
 *   ticks and sends a WireGuard keepalive (empty encrypted packet) to
 *   each active peer every WG_KEEPALIVE_SECS (25) seconds.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include "contracts/wg_net_contract.h"
#include "wg_net.h"
#include "net_server.h"
#include "monocypher.h"

/* ── virtio-MMIO helpers (reuse net_server.h MMIO constants) ─────────────── */
#define WG_MMIO_MAGIC   VIRTIO_MMIO_MAGIC

/* ── Channel IDs (from wg_net's perspective in agentos.system) ──────────── */
#define CH_CONTROLLER     0   /* controller <-> wg_net (PPC + notify) */
#define WG_CH_NET_SERVER  1   /* wg_net -> net_server (PPC for OP_NET_VNIC_SEND) */
#define CH_TIMER        2   /* periodic 1-second tick from timer driver */

/* ── Staging region virtual address (set by Microkit via setvar_vaddr) ───── */
uintptr_t wg_staging_vaddr;
#define WG_STAGING ((volatile uint8_t *)wg_staging_vaddr)

/* Staging sub-region offsets */
#define WG_STAGING_PRIVKEY_OFF   0x000000UL   /* local private key (32 bytes) */
#define WG_STAGING_PUBKEY_OFF    0x000020UL   /* local public key  (32 bytes) */
#define WG_STAGING_PEER_KEY_OFF  0x001000UL   /* peer pubkey for ADD_PEER (32 bytes) */
#define WG_STAGING_TX_OFF        0x002000UL   /* TX plaintext region */
#define WG_STAGING_RX_OFF        0x010000UL   /* RX plaintext region */
#define WG_STAGING_TX_MAX        0x00E000UL   /* max TX payload: 56KB */
#define WG_STAGING_RX_MAX        0x00E000UL   /* max RX payload: 56KB */

/* Overhead added by ChaCha20-Poly1305: 16-byte Poly1305 tag */
#define WG_AEAD_TAG_LEN  16u

/* WireGuard transport header size (type[4] + receiver_index[4] + counter[8]) */
#define WG_TRANSPORT_HDR_LEN  16u

/* ── Module state ─────────────────────────────────────────────────────────── */
static wg_peer_t   peers[WG_MAX_PEERS];
static uint32_t    active_peer_count = 0;

/* Local Curve25519 key pair */
static uint8_t     wg_privkey[WG_KEY_LEN];   /* local private key */
static uint8_t     wg_pubkey[WG_KEY_LEN];    /* local public key (derived) */
static bool        wg_privkey_set = false;

/* Keepalive timer state */
static uint32_t    timer_tick    = 0;         /* ticks since boot */
static uint32_t    keepalive_due = 0;         /* tick at which next keepalive fires */

/* RX ring: single-slot staging per peer for received packets */
static uint32_t    rx_peer_id    = 0;         /* peer_id of pending RX packet */
static uint32_t    rx_data_len   = 0;         /* length of pending RX plaintext */
static bool        rx_pending    = false;     /* true if decrypted packet awaits read */

/* ── Minimal string / numeric helpers (no libc) ──────────────────────────── */

static void wg_log_dec(uint32_t v) {
    char buf[12];
    int  i = 11;
    buf[i] = '\0';
    if (v == 0) { buf[--i] = '0'; }
    else { while (v > 0 && i > 0) { buf[--i] = '0' + (char)(v % 10); v /= 10; } }
    log_drain_write(16, 16, &buf[i]);
}

static void wg_log_hex(uint32_t v) {
    static const char hex[] = "0123456789abcdef";
    char buf[11];
    buf[0]  = '0'; buf[1]  = 'x';
    buf[2]  = hex[(v >> 28) & 0xf]; buf[3]  = hex[(v >> 24) & 0xf];
    buf[4]  = hex[(v >> 20) & 0xf]; buf[5]  = hex[(v >> 16) & 0xf];
    buf[6]  = hex[(v >> 12) & 0xf]; buf[7]  = hex[(v >>  8) & 0xf];
    buf[8]  = hex[(v >>  4) & 0xf]; buf[9]  = hex[ v        & 0xf];
    buf[10] = '\0';
    log_drain_write(16, 16, buf);
}

/* Copy n bytes from volatile src to non-volatile dst */
static void wg_copy_from_staging(uint8_t *dst, volatile const uint8_t *src,
                                   uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        dst[i] = src[i];
}

/* Copy n bytes from non-volatile src to volatile dst (staging) */
static void wg_copy_to_staging(volatile uint8_t *dst, const uint8_t *src,
                                 uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        dst[i] = src[i];
}

/* Zero n bytes at dst */
static void wg_zero(uint8_t *dst, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        dst[i] = 0;
}

/* ── Peer table helpers ───────────────────────────────────────────────────── */

static wg_peer_t *find_peer(uint8_t peer_id) {
    for (int i = 0; i < (int)WG_MAX_PEERS; i++) {
        if (peers[i].active && peers[i].peer_id == peer_id)
            return &peers[i];
    }
    return NULL;
}

static int alloc_peer_slot(void) {
    for (int i = 0; i < (int)WG_MAX_PEERS; i++) {
        if (!peers[i].active)
            return i;
    }
    return -1;
}

/* ── ChaCha20-Poly1305 AEAD stub ─────────────────────────────────────────── */

/*
 * wg_encrypt — ChaCha20-Poly1305 encryption stub.
 *
 * CRYPTO_INTEGRATION_POINT: Replace body with:
 *   crypto_aead_lock(cipher, cipher + plain_len,
 *                    key, nonce, NULL, 0, plain, plain_len);
 *   *cipher_len = plain_len + WG_AEAD_TAG_LEN;
 * where crypto_aead_lock() is from Monocypher (monocypher.h).
 *
 * Currently performs an identity transform (no-op) for stub/test mode.
 * Returns 0 on success, -1 on failure.
 */
static int wg_encrypt(const uint8_t *key, const uint8_t *nonce,
                       const uint8_t *plain, uint32_t plain_len,
                       uint8_t *cipher, uint32_t *cipher_len) {
    if (plain_len + WG_AEAD_TAG_LEN < plain_len)   /* overflow check */
        return -1;

    /*
     * crypto_aead_lock() expects a 24-byte XChaCha20 nonce.
     * The caller supplies a 12-byte WireGuard nonce (counter); zero-pad to 24.
     */
    uint8_t nonce24[24];
    for (int i = 0; i < 24; i++) nonce24[i] = 0;
    for (int i = 0; i < 12; i++) nonce24[i] = nonce[i];

    /*
     * crypto_aead_lock writes cipher_text then appends mac[16] to the output.
     * Layout: cipher[0..plain_len-1] = encrypted payload,
     *         cipher[plain_len..plain_len+15] = Poly1305 tag.
     */
    uint8_t mac[WG_AEAD_TAG_LEN];
    crypto_aead_lock(cipher, mac, key, nonce24,
                     NULL, 0u,
                     plain, (size_t)plain_len);

    /* Append the Poly1305 tag immediately after the ciphertext */
    for (uint32_t i = 0; i < WG_AEAD_TAG_LEN; i++)
        cipher[plain_len + i] = mac[i];

    *cipher_len = plain_len + WG_AEAD_TAG_LEN;
    return 0;
}

/*
 * wg_decrypt — ChaCha20-Poly1305 decryption stub.
 *
 * CRYPTO_INTEGRATION_POINT: Replace body with:
 *   int rc = crypto_aead_unlock(plain, key, nonce, cipher + cipher_len - 16,
 *                               NULL, 0, cipher, cipher_len - 16);
 *   if (rc != 0) return -1;  // authentication failed
 *   *plain_len = cipher_len - WG_AEAD_TAG_LEN;
 *
 * Returns 0 on success, -1 if authentication fails.
 */
static int wg_decrypt(const uint8_t *key, const uint8_t *nonce,
                       const uint8_t *cipher, uint32_t cipher_len,
                       uint8_t *plain, uint32_t *plain_len) {
    if (cipher_len < WG_AEAD_TAG_LEN)
        return -1;

    uint32_t ct_len = cipher_len - WG_AEAD_TAG_LEN;
    const uint8_t *mac = cipher + ct_len;   /* tag is appended after ciphertext */

    /* Expand 12-byte nonce to 24-byte XChaCha20 nonce */
    uint8_t nonce24[24];
    for (int i = 0; i < 24; i++) nonce24[i] = 0;
    for (int i = 0; i < 12; i++) nonce24[i] = nonce[i];

    int rc = crypto_aead_unlock(plain, mac, key, nonce24,
                                NULL, 0u,
                                cipher, (size_t)ct_len);
    if (rc != 0)
        return -1;   /* MAC verification failed */

    *plain_len = ct_len;
    return 0;
}

/*
 * wg_derive_pubkey — Curve25519 scalar multiplication: pubkey = privkey * G
 *
 * CRYPTO_INTEGRATION_POINT: Replace body with:
 *   static const uint8_t basepoint[32] = { 9 };
 *   crypto_x25519(pubkey, privkey, basepoint);
 * where crypto_x25519() is from Monocypher.
 *
 * Stub: copies privkey XOR 0x42 as a placeholder public key.
 */
static void wg_derive_pubkey(const uint8_t *privkey, uint8_t *pubkey) {
    /* Curve25519 generator: u-coordinate = 9 */
    static const uint8_t basepoint[32] = { 9u };
    crypto_x25519(pubkey, privkey, basepoint);
}

/* ── Keepalive sender ─────────────────────────────────────────────────────── */

/*
 * Send a WireGuard keepalive (empty encrypted packet) to all active peers.
 * A keepalive is an encrypted transport packet with zero-length plaintext;
 * it refreshes the receiver's session timer without delivering data.
 */
static void send_keepalives(void) {
    if (!wg_privkey_set)
        return;

    for (int i = 0; i < (int)WG_MAX_PEERS; i++) {
        wg_peer_t *p = &peers[i];
        if (!p->active)
            continue;

        /*
         * Build a WireGuard transport header + empty encrypted payload
         * into the wg_staging TX region.
         *
         * WireGuard transport message layout:
         *   [0..3]   type = 4 (transport data)
         *   [4..7]   receiver index
         *   [8..15]  nonce (counter)
         *   [16..]   encrypted payload (0 bytes plaintext + 16-byte tag)
         */
        if (!wg_staging_vaddr) {
            log_drain_write(16, 16, "[wg_net] keepalive: staging not mapped\n");
            return;
        }

        volatile uint8_t *tx = WG_STAGING + WG_STAGING_TX_OFF;

        /* Transport header: type=4, receiver_index=peer_id, nonce=0 */
        tx[0]  = 4;   tx[1]  = 0; tx[2]  = 0; tx[3]  = 0;
        tx[4]  = p->peer_id; tx[5] = 0; tx[6] = 0; tx[7] = 0;
        for (int b = 8; b < 16; b++) tx[b] = 0;

        /* Encrypt empty payload (0-byte plaintext) */
        uint8_t  nonce[12]   = {0};
        uint8_t  cipher[WG_AEAD_TAG_LEN];
        uint32_t cipher_len  = 0;

        /* Use a per-peer session key placeholder (all-zero here) */
        uint8_t session_key[WG_KEY_LEN];
        wg_zero(session_key, WG_KEY_LEN);

        int rc = wg_encrypt(session_key, nonce, NULL, 0, cipher, &cipher_len);
        if (rc != 0)
            continue;

        /* Append ciphertext after transport header */
        for (uint32_t b = 0; b < cipher_len; b++)
            tx[WG_TRANSPORT_HDR_LEN + b] = cipher[b];

        uint32_t pkt_len = WG_TRANSPORT_HDR_LEN + cipher_len;

        /* Forward to net_server: OP_NET_VNIC_SEND */
        microkit_mr_set(0, (uint64_t)OP_NET_VNIC_SEND);
        microkit_mr_set(1, 0u);                       /* vnic_id = 0 (wg_net's own vNIC) */
        microkit_mr_set(2, (uint32_t)WG_STAGING_TX_OFF);
        microkit_mr_set(3, pkt_len);
        microkit_ppcall(WG_CH_NET_SERVER, microkit_msginfo_new(OP_NET_VNIC_SEND, 4));

        p->tx_bytes += pkt_len;

        log_drain_write(16, 16, "[wg_net] keepalive -> peer=");
        wg_log_dec(p->peer_id);
        log_drain_write(16, 16, " ep=");
        wg_log_hex(p->endpoint_ip);
        log_drain_write(16, 16, ":");
        wg_log_dec(p->endpoint_port);
        log_drain_write(16, 16, "\n");
    }
}

/* ── OP_WG_ADD_PEER ───────────────────────────────────────────────────────── */

static microkit_msginfo handle_add_peer(void) {
    uint32_t peer_id      = (uint32_t)microkit_mr_get(1);
    uint32_t pubkey_off   = (uint32_t)microkit_mr_get(2);
    uint32_t endpoint_ip  = (uint32_t)microkit_mr_get(3);
    uint32_t endpoint_port= (uint32_t)microkit_mr_get(4);
    uint32_t allowed_ip   = (uint32_t)microkit_mr_get(5);
    uint32_t allowed_mask = (uint32_t)microkit_mr_get(6);

    if (peer_id >= WG_MAX_PEERS) {
        log_drain_write(16, 16, "[wg_net] ADD_PEER: invalid peer_id=");
        wg_log_dec(peer_id);
        log_drain_write(16, 16, "\n");
        microkit_mr_set(0, WG_ERR_NOPEER);
        return microkit_msginfo_new(0, 1);
    }

    if (active_peer_count >= WG_MAX_PEERS && find_peer((uint8_t)peer_id) == NULL) {
        log_drain_write(16, 16, "[wg_net] ADD_PEER: peer table full\n");
        microkit_mr_set(0, WG_ERR_FULL);
        return microkit_msginfo_new(0, 1);
    }

    /* Validate staging bounds for pubkey */
    if (!wg_staging_vaddr
            || pubkey_off + WG_KEY_LEN > 0x20000u) {
        log_drain_write(16, 16, "[wg_net] ADD_PEER: staging not mapped or bad offset\n");
        microkit_mr_set(0, WG_ERR_CRYPTO);
        return microkit_msginfo_new(0, 1);
    }

    /* Find existing slot for this peer_id or allocate a new one */
    wg_peer_t *p = find_peer((uint8_t)peer_id);
    if (!p) {
        int slot = alloc_peer_slot();
        if (slot < 0) {
            microkit_mr_set(0, WG_ERR_FULL);
            return microkit_msginfo_new(0, 1);
        }
        p = &peers[slot];
        active_peer_count++;
    }

    /* Populate peer entry */
    p->peer_id      = (uint8_t)peer_id;
    p->active       = true;
    p->endpoint_ip  = endpoint_ip;
    p->endpoint_port= (uint16_t)(endpoint_port & 0xFFFFu);
    p->allowed_ip   = allowed_ip;
    p->allowed_mask = allowed_mask;
    p->tx_bytes     = 0;
    p->rx_bytes     = 0;
    p->last_handshake = 0;
    wg_zero(p->preshared_key, WG_KEY_LEN);

    /* Copy public key from staging */
    wg_copy_from_staging(p->pubkey,
                          WG_STAGING + pubkey_off,
                          WG_KEY_LEN);

    log_drain_write(16, 16, "[wg_net] ADD_PEER: id=");
    wg_log_dec(peer_id);
    log_drain_write(16, 16, " ep=");
    wg_log_hex(endpoint_ip);
    log_drain_write(16, 16, ":");
    wg_log_dec(endpoint_port);
    log_drain_write(16, 16, " allowed=");
    wg_log_hex(allowed_ip);
    log_drain_write(16, 16, "/");
    wg_log_hex(allowed_mask);
    log_drain_write(16, 16, "\n");

    microkit_mr_set(0, WG_OK);
    return microkit_msginfo_new(0, 1);
}

/* ── OP_WG_REMOVE_PEER ────────────────────────────────────────────────────── */

static microkit_msginfo handle_remove_peer(void) {
    uint32_t peer_id = (uint32_t)microkit_mr_get(1);

    wg_peer_t *p = find_peer((uint8_t)peer_id);
    if (!p) {
        log_drain_write(16, 16, "[wg_net] REMOVE_PEER: not found id=");
        wg_log_dec(peer_id);
        log_drain_write(16, 16, "\n");
        microkit_mr_set(0, WG_ERR_NOPEER);
        return microkit_msginfo_new(0, 1);
    }

    log_drain_write(16, 16, "[wg_net] REMOVE_PEER: id=");
    wg_log_dec(peer_id);
    log_drain_write(16, 16, " tx_bytes=");
    wg_log_dec((uint32_t)(p->tx_bytes & 0xFFFFFFFFu));
    log_drain_write(16, 16, " rx_bytes=");
    wg_log_dec((uint32_t)(p->rx_bytes & 0xFFFFFFFFu));
    log_drain_write(16, 16, "\n");

    /* Securely zero key material before deactivating */
    wg_zero(p->pubkey, WG_KEY_LEN);
    wg_zero(p->preshared_key, WG_KEY_LEN);
    p->active = false;
    p->peer_id = 0;

    if (active_peer_count > 0)
        active_peer_count--;

    microkit_mr_set(0, WG_OK);
    return microkit_msginfo_new(0, 1);
}

/* ── OP_WG_SEND ───────────────────────────────────────────────────────────── */

static microkit_msginfo handle_send(void) {
    uint32_t peer_id    = (uint32_t)microkit_mr_get(1);
    uint32_t data_off   = (uint32_t)microkit_mr_get(2);
    uint32_t data_len   = (uint32_t)microkit_mr_get(3);

    if (!wg_privkey_set) {
        log_drain_write(16, 16, "[wg_net] SEND: no private key set\n");
        microkit_mr_set(0, WG_ERR_NOKEY);
        microkit_mr_set(1, 0);
        return microkit_msginfo_new(0, 2);
    }

    wg_peer_t *p = find_peer((uint8_t)peer_id);
    if (!p) {
        log_drain_write(16, 16, "[wg_net] SEND: peer not found id=");
        wg_log_dec(peer_id);
        log_drain_write(16, 16, "\n");
        microkit_mr_set(0, WG_ERR_NOPEER);
        microkit_mr_set(1, 0);
        return microkit_msginfo_new(0, 2);
    }

    /* Bounds check: plaintext must fit in the TX staging region */
    if (!wg_staging_vaddr
            || data_off + data_len < data_off             /* overflow */
            || data_off + data_len > WG_STAGING_TX_MAX) {
        microkit_mr_set(0, WG_ERR_CRYPTO);
        microkit_mr_set(1, 0);
        return microkit_msginfo_new(0, 2);
    }

    /*
     * Encrypt the plaintext payload from wg_staging[TX + data_off].
     *
     * CRYPTO_INTEGRATION_POINT:
     *   Derive a per-peer session key via ECDH:
     *     uint8_t shared_secret[32];
     *     crypto_x25519(shared_secret, wg_privkey, p->pubkey);
     *   Mix with preshared_key using HKDF-like KDF from the WireGuard spec
     *   to produce the ChaCha20-Poly1305 session key.
     *   For now we use a placeholder zero key.
     */
    uint8_t session_key[WG_KEY_LEN];
    uint8_t nonce[12];
    wg_zero(session_key, WG_KEY_LEN);
    wg_zero(nonce, 12);

    /* Read plaintext from staging */
    const uint8_t *plain = (const uint8_t *)(wg_staging_vaddr
                                              + WG_STAGING_TX_OFF + data_off);

    /*
     * Construct WireGuard transport message in a temporary buffer:
     *   [0..15]   transport header
     *   [16..]    ciphertext + 16-byte Poly1305 tag
     */
    volatile uint8_t *out = WG_STAGING + WG_STAGING_TX_OFF;

    /* Transport header */
    out[0] = 4; out[1] = 0; out[2] = 0; out[3] = 0;   /* type=4 */
    out[4] = p->peer_id; out[5] = 0; out[6] = 0; out[7] = 0;
    for (int b = 8; b < 16; b++) out[b] = 0;            /* counter=0 stub */

    /* Encrypt into ciphertext region immediately after header */
    uint8_t *cipher_dst = (uint8_t *)(wg_staging_vaddr
                                       + WG_STAGING_TX_OFF
                                       + WG_TRANSPORT_HDR_LEN);
    uint32_t cipher_len = 0;

    int rc = wg_encrypt(session_key, nonce, plain, data_len,
                         cipher_dst, &cipher_len);
    if (rc != 0) {
        log_drain_write(16, 16, "[wg_net] SEND: encrypt failed for peer=");
        wg_log_dec(peer_id);
        log_drain_write(16, 16, "\n");
        microkit_mr_set(0, WG_ERR_CRYPTO);
        microkit_mr_set(1, 0);
        return microkit_msginfo_new(0, 2);
    }

    uint32_t pkt_len = WG_TRANSPORT_HDR_LEN + cipher_len;

    /* Forward encrypted packet to net_server via OP_NET_VNIC_SEND */
    microkit_mr_set(0, (uint64_t)OP_NET_VNIC_SEND);
    microkit_mr_set(1, 0u);   /* wg_net uses vNIC id=0 */
    microkit_mr_set(2, (uint32_t)WG_STAGING_TX_OFF);
    microkit_mr_set(3, pkt_len);
    microkit_ppcall(WG_CH_NET_SERVER, microkit_msginfo_new(OP_NET_VNIC_SEND, 4));

    p->tx_bytes += pkt_len;

    log_drain_write(16, 16, "[wg_net] SEND: peer=");
    wg_log_dec(peer_id);
    log_drain_write(16, 16, " plain_len=");
    wg_log_dec(data_len);
    log_drain_write(16, 16, " cipher_len=");
    wg_log_dec(cipher_len);
    log_drain_write(16, 16, "\n");

    microkit_mr_set(0, WG_OK);
    microkit_mr_set(1, cipher_len);
    return microkit_msginfo_new(0, 2);
}

/* ── OP_WG_RECV ───────────────────────────────────────────────────────────── */

static microkit_msginfo handle_recv(void) {
    uint32_t req_peer = (uint32_t)microkit_mr_get(1);

    if (!rx_pending) {
        /* No packet ready */
        microkit_mr_set(0, WG_OK);
        microkit_mr_set(1, 0u);   /* src_peer_id: undefined */
        microkit_mr_set(2, 0u);   /* data_offset */
        microkit_mr_set(3, 0u);   /* data_len = 0 means empty */
        return microkit_msginfo_new(0, 4);
    }

    /* Filter by peer if caller specified one */
    if (req_peer != 0xFFu && rx_peer_id != (uint8_t)req_peer) {
        microkit_mr_set(0, WG_OK);
        microkit_mr_set(1, 0u);
        microkit_mr_set(2, 0u);
        microkit_mr_set(3, 0u);
        return microkit_msginfo_new(0, 4);
    }

    /* Deliver: data is already at WG_STAGING_RX_OFF in staging */
    uint32_t peer_out = rx_peer_id;
    uint32_t len_out  = rx_data_len;
    rx_pending  = false;
    rx_data_len = 0;
    rx_peer_id  = 0;

    log_drain_write(16, 16, "[wg_net] RECV: peer=");
    wg_log_dec(peer_out);
    log_drain_write(16, 16, " len=");
    wg_log_dec(len_out);
    log_drain_write(16, 16, "\n");

    microkit_mr_set(0, WG_OK);
    microkit_mr_set(1, peer_out);
    microkit_mr_set(2, (uint32_t)WG_STAGING_RX_OFF);
    microkit_mr_set(3, len_out);
    return microkit_msginfo_new(0, 4);
}

/* ── OP_WG_STATUS ─────────────────────────────────────────────────────────── */

static microkit_msginfo handle_status(void) {
    uint64_t total_tx = 0, total_rx = 0;
    uint32_t last_hs  = 0;

    for (int i = 0; i < (int)WG_MAX_PEERS; i++) {
        if (!peers[i].active)
            continue;
        total_tx += peers[i].tx_bytes;
        total_rx += peers[i].rx_bytes;
        if (peers[i].last_handshake > last_hs)
            last_hs = peers[i].last_handshake;
    }

    microkit_mr_set(0, WG_OK);
    microkit_mr_set(1, active_peer_count);
    microkit_mr_set(2, (uint32_t)(total_tx & 0xFFFFFFFFu));
    microkit_mr_set(3, (uint32_t)(total_rx & 0xFFFFFFFFu));
    microkit_mr_set(4, last_hs);
    return microkit_msginfo_new(0, 5);
}

/* ── OP_WG_SET_PRIVKEY ────────────────────────────────────────────────────── */

static microkit_msginfo handle_set_privkey(void) {
    uint32_t key_off = (uint32_t)microkit_mr_get(1);

    if (!wg_staging_vaddr || key_off + WG_KEY_LEN > 0x20000u) {
        log_drain_write(16, 16, "[wg_net] SET_PRIVKEY: staging not mapped or bad offset\n");
        microkit_mr_set(0, WG_ERR_CRYPTO);
        return microkit_msginfo_new(0, 1);
    }

    /* Copy private key from staging */
    wg_copy_from_staging(wg_privkey, WG_STAGING + key_off, WG_KEY_LEN);

    /*
     * Derive public key via Curve25519 scalar multiplication.
     *
     * CRYPTO_INTEGRATION_POINT:
     *   static const uint8_t basepoint[32] = { 9 };
     *   crypto_x25519(wg_pubkey, wg_privkey, basepoint);
     */
    wg_derive_pubkey(wg_privkey, wg_pubkey);

    /* Write derived public key back to staging for caller to read */
    wg_copy_to_staging(WG_STAGING + WG_STAGING_PUBKEY_OFF,
                        wg_pubkey, WG_KEY_LEN);

    wg_privkey_set = true;

    log_drain_write(16, 16, "[wg_net] SET_PRIVKEY: key loaded, pubkey[0..3]=");
    wg_log_hex((uint32_t)(wg_pubkey[0])
                | ((uint32_t)wg_pubkey[1] << 8)
                | ((uint32_t)wg_pubkey[2] << 16)
                | ((uint32_t)wg_pubkey[3] << 24));
    log_drain_write(16, 16, "\n");

    microkit_mr_set(0, WG_OK);
    return microkit_msginfo_new(0, 1);
}

/* ── OP_WG_HEALTH ─────────────────────────────────────────────────────────── */

static microkit_msginfo handle_health(void) {
    microkit_mr_set(0, WG_OK);
    microkit_mr_set(1, active_peer_count);
    microkit_mr_set(2, wg_privkey_set ? 1u : 0u);
    return microkit_msginfo_new(0, 3);
}

/* ── protected() — synchronous PPC dispatch ──────────────────────────────── */

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msg) {
    (void)ch;
    uint64_t opcode = microkit_msginfo_get_label(msg);

    switch ((uint32_t)opcode) {
    case OP_WG_ADD_PEER:    return handle_add_peer();
    case OP_WG_REMOVE_PEER: return handle_remove_peer();
    case OP_WG_SEND:        return handle_send();
    case OP_WG_RECV:        return handle_recv();
    case OP_WG_STATUS:      return handle_status();
    case OP_WG_SET_PRIVKEY: return handle_set_privkey();
    case OP_WG_HEALTH:      return handle_health();

    default:
        log_drain_write(16, 16, "[wg_net] unknown opcode=");
        wg_log_hex((uint32_t)opcode);
        log_drain_write(16, 16, "\n");
        microkit_mr_set(0, 0xFFFFu);
        return microkit_msginfo_new(0xFFFF, 1);
    }
}

/* ── notified() — async timer / inbound packet notifications ─────────────── */

void notified(microkit_channel ch) {
    if (ch == CH_TIMER) {
        timer_tick++;
        if (timer_tick >= keepalive_due) {
            send_keepalives();
            keepalive_due = timer_tick + WG_KEEPALIVE_SECS;
        }
        return;
    }

    if (ch == WG_CH_NET_SERVER) {
        /*
         * net_server is notifying us that an inbound UDP packet arrived
         * on our wNIC that may be a WireGuard transport message.
         *
         * Poll net_server for the raw packet, parse the WireGuard transport
         * header, identify the sender peer by receiver_index, then decrypt.
         *
         * CRYPTO_INTEGRATION_POINT: full WireGuard transport parsing.
         * For now, stub a receive: attempt to pull one packet from net_server.
         */
        if (rx_pending) {
            /* Already have an unread packet — drop incoming */
            return;
        }

        /* Poll net_server for an inbound packet */
        microkit_mr_set(0, (uint64_t)OP_NET_VNIC_RECV);
        microkit_mr_set(1, 0u);   /* vnic_id=0 */
        microkit_mr_set(2, (uint32_t)WG_STAGING_RX_OFF);
        microkit_mr_set(3, (uint32_t)WG_STAGING_RX_MAX);
        microkit_msginfo reply =
            microkit_ppcall(WG_CH_NET_SERVER,
                            microkit_msginfo_new(OP_NET_VNIC_RECV, 4));

        uint32_t res      = (uint32_t)microkit_mr_get(0);
        uint32_t pkt_len  = (uint32_t)microkit_mr_get(1);

        if (res != NET_OK || pkt_len < WG_TRANSPORT_HDR_LEN + WG_AEAD_TAG_LEN) {
            (void)reply;
            return;
        }

        /*
         * Parse transport header: extract receiver_index (bytes 4..7)
         * to identify which peer sent this packet.
         */
        const uint8_t *raw = (const uint8_t *)(wg_staging_vaddr
                                                + WG_STAGING_RX_OFF);
        uint8_t msg_type  = raw[0];
        if (msg_type != 4u) {
            /* Not a transport data message — ignore (could be handshake) */
            return;
        }
        uint8_t recv_idx  = raw[4];   /* receiver_index low byte = peer_id */
        wg_peer_t *p      = find_peer(recv_idx);
        if (!p)
            return;

        /* Decrypt payload */
        uint8_t session_key[WG_KEY_LEN];
        uint8_t nonce[12];
        wg_zero(session_key, WG_KEY_LEN);
        wg_zero(nonce, 12);

        const uint8_t *cipher    = raw + WG_TRANSPORT_HDR_LEN;
        uint32_t       cipher_len = pkt_len - WG_TRANSPORT_HDR_LEN;
        uint8_t       *plain_dst  = (uint8_t *)(wg_staging_vaddr
                                                 + WG_STAGING_RX_OFF);
        uint32_t       plain_len  = 0;

        int rc = wg_decrypt(session_key, nonce, cipher, cipher_len,
                             plain_dst, &plain_len);
        if (rc != 0) {
            log_drain_write(16, 16, "[wg_net] RECV: decrypt failed, dropping\n");
            return;
        }

        p->rx_bytes     += pkt_len;
        rx_peer_id       = recv_idx;
        rx_data_len      = plain_len;
        rx_pending       = true;

        log_drain_write(16, 16, "[wg_net] notified: inbound packet from peer=");
        wg_log_dec(recv_idx);
        log_drain_write(16, 16, " plain_len=");
        wg_log_dec(plain_len);
        log_drain_write(16, 16, "\n");
        return;
    }

    if (ch == CH_CONTROLLER) {
        /* Controller notification — no-op for now */
        return;
    }
}

/* ── init() ──────────────────────────────────────────────────────────────── */

void init(void) {
    /* Zero peer table */
    for (int i = 0; i < (int)WG_MAX_PEERS; i++) {
        peers[i].active  = false;
        peers[i].peer_id = 0;
        wg_zero(peers[i].pubkey, WG_KEY_LEN);
        wg_zero(peers[i].preshared_key, WG_KEY_LEN);
        peers[i].tx_bytes      = 0;
        peers[i].rx_bytes      = 0;
        peers[i].last_handshake= 0;
    }
    active_peer_count = 0;

    /* Zero local key state */
    wg_zero(wg_privkey, WG_KEY_LEN);
    wg_zero(wg_pubkey, WG_KEY_LEN);
    wg_privkey_set = false;

    /* Timer state */
    timer_tick    = 0;
    keepalive_due = WG_KEEPALIVE_SECS;

    /* RX state */
    rx_pending  = false;
    rx_data_len = 0;
    rx_peer_id  = 0;

    /* Zero staging region key slots if mapped */
    if (wg_staging_vaddr) {
        volatile uint8_t *s = WG_STAGING;
        for (uint32_t i = 0; i < WG_KEY_LEN * 2; i++)
            s[i] = 0;
    }

    log_drain_write(16, 16, "[wg_net] WireGuard overlay PD online\n");
    log_drain_write(16, 16, "[wg_net]   max_peers=16, keepalive=25s, "
                "crypto=stub(CRYPTO_INTEGRATION_POINT)\n");
    log_drain_write(16, 16, "[wg_net]   call OP_WG_SET_PRIVKEY before OP_WG_SEND\n");

    /* Notify controller that wg_net is ready */
    microkit_notify(CH_CONTROLLER);
}
