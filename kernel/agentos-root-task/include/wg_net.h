/*
 * wg_net.h — WireGuard Overlay Network Protection Domain
 *
 * Provides an encrypted, peer-to-peer overlay network for agent-to-agent
 * communication using WireGuard's Noise_IKpsk2 handshake protocol backed
 * by Curve25519 ECDH and ChaCha20-Poly1305 AEAD.
 *
 * Architecture:
 *   - Up to WG_MAX_PEERS (16) peers in a static peer table
 *   - Each peer is identified by a uint8_t peer_id (0..WG_MAX_PEERS-1)
 *   - Peer public keys are Curve25519 (32 bytes each)
 *   - Optional preshared keys per peer for post-quantum resistance
 *   - Keepalive timer fires every 25 seconds (per WireGuard spec §6.1)
 *   - Encrypted payloads forwarded via net_server's OP_NET_VNIC_SEND
 *
 * Shared memory:
 *   wg_staging (mapped at setvar_vaddr): staging region for key material
 *   and send/receive packet buffers. Layout:
 *     [0x000000 .. 0x00001F]  local private key (32 bytes, write-once)
 *     [0x000020 .. 0x00003F]  local public key  (32 bytes, read-only after init)
 *     [0x001000 .. 0x001FFF]  peer pubkey staging (OP_WG_ADD_PEER uses this)
 *     [0x002000 .. 0x00FFFF]  TX packet staging  (OP_WG_SEND plaintext input)
 *     [0x010000 .. 0x01FFFF]  RX packet staging  (OP_WG_RECV decrypted output)
 *
 * Crypto integration:
 *   Curve25519 and ChaCha20-Poly1305 stubs are tagged:
 *     CRYPTO_INTEGRATION_POINT
 *   Wire up Monocypher by calling crypto_x25519() for ECDH and
 *   crypto_aead_lock() / crypto_aead_unlock() for AEAD at those sites.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── Limits ──────────────────────────────────────────────────────────────── */
#define WG_MAX_PEERS        16u   /* maximum simultaneous WireGuard peers */
#define WG_KEY_LEN          32u   /* Curve25519 key length in bytes */

/* ── Keepalive interval (WireGuard spec §6.1) ────────────────────────────── */
#define WG_KEEPALIVE_SECS   25u   /* send keepalive if no traffic for 25s */

/* ── IPC Opcodes ─────────────────────────────────────────────────────────── */

/*
 * OP_WG_ADD_PEER (0xD0) — register a new WireGuard peer
 *   MR1 = peer_id         (uint8_t, 0..WG_MAX_PEERS-1)
 *   MR2 = pubkey_offset   (byte offset into wg_staging for the 32-byte public key)
 *   MR3 = endpoint_ip_be  (IPv4 endpoint address, network byte order)
 *   MR4 = endpoint_port   (UDP port, host byte order)
 *   MR5 = allowed_ip_be   (allowed-IP network address, network byte order)
 *   MR6 = allowed_mask    (subnet mask in host byte order, e.g. 0xFFFFFF00 for /24)
 *   Reply:
 *   MR0 = result (WG_OK or WG_ERR_*)
 */
#define OP_WG_ADD_PEER      0xD0u

/*
 * OP_WG_REMOVE_PEER (0xD1) — deregister a peer and clear its keys
 *   MR1 = peer_id
 *   Reply:
 *   MR0 = result
 */
#define OP_WG_REMOVE_PEER   0xD1u

/*
 * OP_WG_SEND (0xD2) — encrypt and transmit a packet to a peer
 *   MR1 = peer_id       (identifies which peer's session keys to use)
 *   MR2 = data_offset   (byte offset into wg_staging TX region for plaintext)
 *   MR3 = data_len      (plaintext length in bytes; max 65535)
 *   Reply:
 *   MR0 = result
 *   MR1 = bytes_sent    (ciphertext length after encryption)
 *
 *   The wg_net PD encrypts the payload at wg_staging[data_offset..data_offset+data_len]
 *   using ChaCha20-Poly1305 (CRYPTO_INTEGRATION_POINT), prepends a WireGuard transport
 *   header, and forwards to net_server via OP_NET_VNIC_SEND on the wg_net vNIC.
 */
#define OP_WG_SEND          0xD2u

/*
 * OP_WG_RECV (0xD3) — poll for a received, decrypted packet from a peer
 *   MR1 = peer_id       (0xFF = receive from any peer)
 *   Reply:
 *   MR0 = result
 *   MR1 = src_peer_id   (actual peer the packet came from)
 *   MR2 = data_offset   (byte offset into wg_staging RX region for plaintext)
 *   MR3 = data_len      (plaintext length; 0 if no packet pending)
 *
 *   Non-blocking: returns WG_OK with data_len=0 if no packet is available.
 *   The caller must read decrypted data from wg_staging before the next call.
 */
#define OP_WG_RECV          0xD3u

/*
 * OP_WG_STATUS (0xD4) — query WireGuard overlay status
 *   Reply:
 *   MR0 = result (WG_OK)
 *   MR1 = active_peer_count
 *   MR2 = total_tx_bytes (lower 32 bits)
 *   MR3 = total_rx_bytes (lower 32 bits)
 *   MR4 = last_handshake_secs (seconds since epoch of most recent handshake, or 0)
 */
#define OP_WG_STATUS        0xD4u

/*
 * OP_WG_SET_PRIVKEY (0xD5) — set the local WireGuard private key
 *   MR1 = key_offset    (byte offset into wg_staging; 32 bytes of Curve25519 key)
 *   Reply:
 *   MR0 = result (WG_OK or WG_ERR_CRYPTO if key is degenerate)
 *
 *   After setting the private key, the PD derives the public key via Curve25519
 *   scalar multiplication (CRYPTO_INTEGRATION_POINT) and stores it in
 *   wg_staging[0x20..0x3F] for the caller to read.
 *   This opcode must be called before any OP_WG_SEND will succeed.
 */
#define OP_WG_SET_PRIVKEY   0xD5u

/*
 * OP_WG_HEALTH (0xD6) — liveness check
 *   Reply:
 *   MR0 = WG_OK (0)
 *   MR1 = active_peer_count
 *   MR2 = 1 if private key is set, 0 otherwise
 */
#define OP_WG_HEALTH        0xD6u

/* ── Result codes (MR0 in replies) ──────────────────────────────────────── */
#define WG_OK               0u   /* success */
#define WG_ERR_NOPEER       1u   /* peer_id not found or not active */
#define WG_ERR_NOKEY        2u   /* local private key not yet configured */
#define WG_ERR_FULL         3u   /* peer table full (WG_MAX_PEERS reached) */
#define WG_ERR_CRYPTO       4u   /* cryptographic operation failed */

/* ── Peer table entry ────────────────────────────────────────────────────── */
/*
 * Stored statically in the wg_net PD's BSS (peers[WG_MAX_PEERS]).
 * Public for documentation and test-harness purposes.
 */
typedef struct {
    uint8_t   peer_id;                  /* 0..WG_MAX_PEERS-1; only valid when active */
    bool      active;                   /* true if this slot is in use */
    uint8_t   pubkey[WG_KEY_LEN];       /* peer's Curve25519 public key */
    uint8_t   preshared_key[WG_KEY_LEN];/* optional pre-shared key (zeros = none) */
    uint32_t  endpoint_ip;              /* UDP endpoint IPv4, network byte order */
    uint16_t  endpoint_port;            /* UDP endpoint port, host byte order */
    uint8_t   _ep_pad[2];               /* alignment */
    uint32_t  allowed_ip;               /* allowed-IP network address, net byte order */
    uint32_t  allowed_mask;             /* subnet mask, host byte order */
    uint64_t  tx_bytes;                 /* encrypted bytes sent to this peer */
    uint64_t  rx_bytes;                 /* decrypted bytes received from this peer */
    uint32_t  last_handshake;           /* monotonic tick of last successful handshake */
    uint8_t   _pad[4];                  /* explicit pad to 8-byte alignment */
} wg_peer_t;                            /* ~112 bytes */
