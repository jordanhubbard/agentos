/*
 * boot_manifest.h — Signed boot-manifest format for protection-domain images
 *
 * The build emits one manifest per bundle: a fixed header, `count` entries
 * (one per PD, carrying its bare stem name and the SHA-256 of its ELF
 * bytes), and a trailing 64-byte Ed25519 signature over everything that
 * precedes it (header + entries).
 *
 *   +--------------------------+
 *   | aos_boot_manifest_hdr_t  |   sizeof == 32
 *   +--------------------------+
 *   | entry[0]                 |   sizeof == 80
 *   | entry[1]                 |
 *   | ...                      |
 *   | entry[count-1]           |
 *   +--------------------------+
 *   | ed25519 signature[64]    |
 *   +--------------------------+
 *
 * This header defines the format and a bounds-safe structural validator,
 * `aos_boot_manifest_validate()`.  It does NOT check the Ed25519 signature
 * — that is layered on top (see docs/superpowers/plans
 * /2026-10-03-t3-image-verification.md, Task 2) once a public key is
 * compiled into the root task.  Nothing in this file or boot_manifest.c
 * trusts an unverified manifest to mean "the PDs are authentic" — it only
 * answers "is this blob shaped correctly and safe to read further".
 *
 * aos_boot_manifest_validate() is the boot-time gate for untrusted bytes:
 * every bounds check happens before any field derived from attacker-
 * controlled input (in particular `count`) is used to index the buffer.
 * A manifest with count == 0 is rejected — an "empty" manifest must never
 * be read as "nothing to verify".
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Magic value for aos_boot_manifest_hdr_t.magic.
 *
 * Bytes, in memory order (little-endian uint64_t): 'A' 'O' 'S' 'B' 'M' 'A'
 * 'N' '1' — i.e. the ASCII string "AOSBMAN1". Distinct from the PD bundle
 * magic AGENTOS_IMAGE_MAGIC_BUNDLE (kernel/agentos-root-task/src/main.c)
 * so a bundle can never be mistaken for a manifest or vice versa.
 */
#define AOS_BOOT_MANIFEST_MAGIC UINT64_C(0x314e414d42534f41)

/* Manifest format version understood by this validator. */
#define AOS_BOOT_MANIFEST_VERSION 1u

/* Upper bound on the number of PD entries a manifest may declare. */
#define AOS_BOOT_MANIFEST_MAX_PDS 32u

/* Length, in bytes, of the trailing Ed25519 signature. */
#define AOS_BOOT_MANIFEST_SIG_LEN 64u

/* Width, in bytes, of aos_boot_manifest_entry_t.name. */
#define AOS_BOOT_MANIFEST_NAME_LEN 48u

/*
 * aos_boot_manifest_hdr_t — fixed 32-byte header at offset 0 of the blob.
 *
 * `reserved` is zero-filled padding for future use; the validator does not
 * interpret it.
 */
typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t count;
    uint8_t  reserved[16];
} __attribute__((packed)) aos_boot_manifest_hdr_t;

/*
 * aos_boot_manifest_entry_t — one PD's name and expected ELF digest.
 *
 * `name` holds the PD's bare stem name (e.g. "controller"). It is NOT
 * guaranteed NUL-terminated: a name exactly 48 bytes long fills the field
 * with no terminator. Callers (and aos_boot_manifest_find) must compare
 * with an explicit bound, never strlen()/strcmp() on this field directly.
 */
typedef struct {
    char    name[AOS_BOOT_MANIFEST_NAME_LEN];
    uint8_t sha256[32];
} __attribute__((packed)) aos_boot_manifest_entry_t;

/* aos_boot_manifest_validate() return codes. Zero is success; every
 * rejection reason has a distinct, stable, negative code so a boot
 * diagnostic can say specifically why a manifest was refused. */
enum {
    AOS_BOOT_MANIFEST_OK              = 0,
    AOS_BOOT_MANIFEST_ERR_TOO_SHORT   = -1, /* blob shorter than the header */
    AOS_BOOT_MANIFEST_ERR_MAGIC       = -2, /* header magic mismatch */
    AOS_BOOT_MANIFEST_ERR_VERSION     = -3, /* header version mismatch */
    AOS_BOOT_MANIFEST_ERR_COUNT_ZERO  = -4, /* count == 0 (empty manifest) */
    AOS_BOOT_MANIFEST_ERR_COUNT_MAX   = -5, /* count exceeds AOS_BOOT_MANIFEST_MAX_PDS */
    AOS_BOOT_MANIFEST_ERR_TRUNCATED   = -6, /* len too small for header+entries+sig */
    AOS_BOOT_MANIFEST_ERR_BAD_ARGS    = -7, /* NULL blob or similar misuse */
};

/*
 * aos_boot_manifest_validate — structural + bounds validation of a boot
 * manifest blob. Does NOT check the Ed25519 signature.
 *
 * `blob` need not be aligned or trusted in any way; `len` is the number of
 * bytes available at `blob`. Every field read from `blob` is bounds-
 * checked against `len` before use; in particular the attacker-controlled
 * `count` field is validated (with overflow-safe arithmetic) against `len`
 * before any entry is read, and before the trailing signature bytes are
 * assumed present.
 *
 * Returns AOS_BOOT_MANIFEST_OK (0) if the blob is well-formed, or one of
 * the negative AOS_BOOT_MANIFEST_ERR_* codes above describing the first
 * rejection reason found.
 */
int aos_boot_manifest_validate(const uint8_t *blob, uint32_t len);

/*
 * aos_boot_manifest_find — look up a PD entry by name.
 *
 * `blob`/`len` must already have passed aos_boot_manifest_validate(); this
 * function does not re-run structural validation and will not read past
 * `len` given bounds-checked input. `name` is a NUL-terminated C string
 * (the lookup key); it is compared against each entry's 48-byte `name`
 * field without assuming that field is NUL-terminated, so a name that
 * fills all 48 bytes is matched correctly and never over-read.
 *
 * Returns a pointer into `blob` for the matching entry, or NULL if no
 * entry's name matches.
 */
const aos_boot_manifest_entry_t *aos_boot_manifest_find(const uint8_t *blob,
                                                          uint32_t len,
                                                          const char *name);

#ifdef __cplusplus
}
#endif
