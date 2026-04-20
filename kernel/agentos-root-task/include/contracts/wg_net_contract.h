/*
 * WireGuard Overlay Network IPC Contract
 *
 * wg_net is a passive seL4 protection domain (priority 140) that implements
 * an encrypted agent-to-agent overlay network using the WireGuard protocol
 * (Noise_IKpsk2 with Curve25519 key agreement, ChaCha20-Poly1305 AEAD).
 *
 * Channel: WG_CH_CONTROLLER (0) — controller PPCs into wg_net.
 *
 * Shared memory:
 *   wg_staging (128KB) layout:
 *     [0x000000..0x00001F]  local private key  (32 bytes, write-once)
 *     [0x000020..0x00003F]  local public key   (32 bytes, derived)
 *     [0x001000..0x001FFF]  peer pubkey staging (OP_WG_ADD_PEER)
 *     [0x002000..0x00FFFF]  TX plaintext staging (OP_WG_SEND)
 *     [0x010000..0x01FFFF]  RX plaintext staging (OP_WG_RECV)
 *
 * Invariants:
 *   - The private key must be written to wg_staging[0x00..0x1F] and
 *     OP_WG_SET_PRIVKEY called before any OP_WG_SEND or OP_WG_ADD_PEER.
 *   - key_offset in OP_WG_SET_PRIVKEY is a byte offset into wg_staging
 *     where the 32-byte key resides (typically 0).
 *   - pubkey_off in OP_WG_ADD_PEER is a byte offset into wg_staging where
 *     the peer's 32-byte Curve25519 public key has been placed.
 *   - endpoint_ip and allowed_ip are IPv4 addresses in host byte order.
 *   - data_off in OP_WG_SEND is a byte offset into the TX region.
 *   - data_offset in the OP_WG_RECV reply is a byte offset into the RX region.
 *   - OP_WG_RECV with req_peer == 0xFFFFFFFF accepts from any peer.
 */

#pragma once
#include "../agentos.h"

/* ─── Channel IDs ────────────────────────────────────────────────────────── */
#define WG_CH_CONTROLLER   0u   /* controller → wg_net (from wg_net's perspective) */

/* ─── Opcodes (placed in MR0) ────────────────────────────────────────────── */
#define OP_WG_SET_PRIVKEY  0x01u  /* install local private key from wg_staging */
#define OP_WG_ADD_PEER     0x02u  /* add or update a peer entry */
#define OP_WG_REMOVE_PEER  0x03u  /* remove a peer by peer_id */
#define OP_WG_SEND         0x04u  /* encrypt and queue a packet to a peer */
#define OP_WG_RECV         0x05u  /* receive and decrypt a queued packet */
#define OP_WG_STATUS       0x06u  /* return overlay statistics */
#define OP_WG_HEALTH       0x07u  /* return active_peers + privkey_set flag */

/* ─── Request structs ────────────────────────────────────────────────────── */

/* OP_WG_SET_PRIVKEY
 * MR0=op, MR1=key_offset
 */
struct __attribute__((packed)) wg_net_req_set_privkey {
    uint32_t op;         /* OP_WG_SET_PRIVKEY */
    uint32_t key_offset; /* byte offset in wg_staging to 32-byte private key */
};

/* OP_WG_ADD_PEER
 * MR0=op, MR1=peer_id, MR2=pubkey_off, MR3=endpoint_ip, MR4=endpoint_port,
 * MR5=allowed_ip, MR6=allowed_mask
 */
struct __attribute__((packed)) wg_net_req_add_peer {
    uint32_t op;
    uint32_t peer_id;       /* caller-assigned peer identifier */
    uint32_t pubkey_off;    /* byte offset in wg_staging to 32-byte pubkey */
    uint32_t endpoint_ip;   /* peer endpoint IPv4 address (host order) */
    uint32_t endpoint_port; /* peer endpoint UDP port */
    uint32_t allowed_ip;    /* allowed source IPv4 for this peer */
    uint32_t allowed_mask;  /* subnet mask for allowed_ip */
};

/* OP_WG_REMOVE_PEER
 * MR0=op, MR1=peer_id
 */
struct __attribute__((packed)) wg_net_req_remove_peer {
    uint32_t op;
    uint32_t peer_id;
};

/* OP_WG_SEND
 * MR0=op, MR1=peer_id, MR2=data_off, MR3=data_len
 */
struct __attribute__((packed)) wg_net_req_send {
    uint32_t op;
    uint32_t peer_id;
    uint32_t data_off; /* byte offset into TX staging region */
    uint32_t data_len; /* plaintext byte count */
};

/* OP_WG_RECV
 * MR0=op, MR1=req_peer  (0xFFFFFFFF = any peer)
 */
struct __attribute__((packed)) wg_net_req_recv {
    uint32_t op;
    uint32_t req_peer; /* peer_id to receive from, or 0xFFFFFFFF for any */
};

/* OP_WG_STATUS
 * MR0=op
 */
struct __attribute__((packed)) wg_net_req_status {
    uint32_t op;
};

/* OP_WG_HEALTH
 * MR0=op
 */
struct __attribute__((packed)) wg_net_req_health {
    uint32_t op;
};

/* ─── Reply structs ──────────────────────────────────────────────────────── */

/* OP_WG_SET_PRIVKEY reply: MR0=status */
struct __attribute__((packed)) wg_net_reply_set_privkey {
    uint32_t status;   /* enum wg_net_error */
};

/* OP_WG_ADD_PEER reply: MR0=status */
struct __attribute__((packed)) wg_net_reply_add_peer {
    uint32_t status;
};

/* OP_WG_REMOVE_PEER reply: MR0=status */
struct __attribute__((packed)) wg_net_reply_remove_peer {
    uint32_t status;
};

/* OP_WG_SEND reply: MR0=status, MR1=cipher_len */
struct __attribute__((packed)) wg_net_reply_send {
    uint32_t status;
    uint32_t cipher_len; /* encrypted byte count placed on the wire */
};

/* OP_WG_RECV reply: MR0=status, MR1=peer_id, MR2=data_offset, MR3=data_len */
struct __attribute__((packed)) wg_net_reply_recv {
    uint32_t status;
    uint32_t peer_id;      /* which peer sent the packet */
    uint32_t data_offset;  /* byte offset into RX staging region */
    uint32_t data_len;     /* decrypted byte count */
};

/* OP_WG_STATUS reply: MR0=ok, MR1=active_peers, MR2=total_tx_lo, MR3=total_rx_lo, MR4=last_handshake */
struct __attribute__((packed)) wg_net_reply_status {
    uint32_t ok;
    uint32_t active_peers;
    uint32_t total_tx_lo;     /* low 32 bits of total transmitted bytes */
    uint32_t total_rx_lo;     /* low 32 bits of total received bytes */
    uint32_t last_handshake;  /* timestamp of most recent completed handshake */
};

/* OP_WG_HEALTH reply: MR0=ok, MR1=active_peers, MR2=privkey_set */
struct __attribute__((packed)) wg_net_reply_health {
    uint32_t ok;
    uint32_t active_peers;
    uint32_t privkey_set; /* 1 if private key has been installed, 0 otherwise */
};

/* ─── Error codes ────────────────────────────────────────────────────────── */

enum wg_net_error {
    WG_OK         = 0,
    WG_ERR_NOPEER = 1,  /* peer_id not found */
    WG_ERR_FULL   = 2,  /* peer table full */
    WG_ERR_CRYPTO = 3,  /* cryptographic operation failed */
    WG_ERR_NOKEY  = 4,  /* private key not yet installed */
};
