/*
 * boot_manifest.c — bounds-safe structural validator for boot manifests.
 *
 * aos_boot_manifest_validate() runs on untrusted bytes at boot time (and,
 * identically, on host-constructed test blobs). Every field read from the
 * blob is bounds-checked against the caller-supplied `len` *before* it is
 * trusted, and the attacker-controlled `count` field is never used to
 * index or size a read until it has been validated. See boot_manifest.h
 * for the on-disk layout and the full contract.
 */

#include "boot_manifest.h"

#include <stddef.h>

int aos_boot_manifest_validate(const uint8_t *blob, uint32_t len)
{
    if (blob == NULL) {
        return AOS_BOOT_MANIFEST_ERR_BAD_ARGS;
    }

    /* Reject a blob too short to hold the header before reading it. */
    if (len < sizeof(aos_boot_manifest_hdr_t)) {
        return AOS_BOOT_MANIFEST_ERR_TOO_SHORT;
    }

    aos_boot_manifest_hdr_t hdr;
    __builtin_memcpy(&hdr, blob, sizeof(hdr));

    if (hdr.magic != AOS_BOOT_MANIFEST_MAGIC) {
        return AOS_BOOT_MANIFEST_ERR_MAGIC;
    }

    if (hdr.version != AOS_BOOT_MANIFEST_VERSION) {
        return AOS_BOOT_MANIFEST_ERR_VERSION;
    }

    /* An "empty" manifest is a rejection, never "nothing to verify". */
    if (hdr.count == 0) {
        return AOS_BOOT_MANIFEST_ERR_COUNT_ZERO;
    }

    /* Bound count before it is ever used in arithmetic sizing a read.
     * AOS_BOOT_MANIFEST_MAX_PDS is small (32), so this also makes the
     * size computation below incapable of overflowing a uint64_t even
     * before considering the explicit widening used there. */
    if (hdr.count > AOS_BOOT_MANIFEST_MAX_PDS) {
        return AOS_BOOT_MANIFEST_ERR_COUNT_MAX;
    }

    /*
     * Overflow-safe required-size computation: widen every operand to
     * uint64_t before multiplying/adding, so no intermediate can wrap a
     * 32-bit type even in principle. `len` (the only trusted bound) is
     * compared against this 64-bit total — the entries and signature are
     * never assumed present until this check passes.
     */
    uint64_t required = (uint64_t)sizeof(aos_boot_manifest_hdr_t)
                       + (uint64_t)hdr.count * (uint64_t)sizeof(aos_boot_manifest_entry_t)
                       + (uint64_t)AOS_BOOT_MANIFEST_SIG_LEN;

    if (required > (uint64_t)len) {
        return AOS_BOOT_MANIFEST_ERR_TRUNCATED;
    }

    return AOS_BOOT_MANIFEST_OK;
}

const aos_boot_manifest_entry_t *aos_boot_manifest_find(const uint8_t *blob,
                                                          uint32_t len,
                                                          const char *name)
{
    if (blob == NULL || name == NULL) {
        return NULL;
    }

    /* Re-derive count the same bounds-checked way validate() did; this
     * function must not be the first thing to trust the blob, so repeat
     * the cheap checks rather than assume a prior validate() call. */
    if (len < sizeof(aos_boot_manifest_hdr_t)) {
        return NULL;
    }

    aos_boot_manifest_hdr_t hdr;
    __builtin_memcpy(&hdr, blob, sizeof(hdr));

    if (hdr.magic != AOS_BOOT_MANIFEST_MAGIC ||
        hdr.version != AOS_BOOT_MANIFEST_VERSION ||
        hdr.count == 0 ||
        hdr.count > AOS_BOOT_MANIFEST_MAX_PDS) {
        return NULL;
    }

    uint64_t required = (uint64_t)sizeof(aos_boot_manifest_hdr_t)
                       + (uint64_t)hdr.count * (uint64_t)sizeof(aos_boot_manifest_entry_t)
                       + (uint64_t)AOS_BOOT_MANIFEST_SIG_LEN;
    if (required > (uint64_t)len) {
        return NULL;
    }

    /* Length of the NUL-terminated search key, capped at the field width:
     * a key longer than the name field can never match a stored name. */
    size_t name_len = 0;
    while (name_len < AOS_BOOT_MANIFEST_NAME_LEN && name[name_len] != '\0') {
        name_len++;
    }
    if (name[name_len] != '\0') {
        /* Search key longer than the fixed-width field: cannot match. */
        return NULL;
    }

    const aos_boot_manifest_entry_t *entries =
        (const aos_boot_manifest_entry_t *)(blob + sizeof(aos_boot_manifest_hdr_t));

    for (uint32_t i = 0; i < hdr.count; i++) {
        const aos_boot_manifest_entry_t *e = &entries[i];

        /* e->name is NOT guaranteed NUL-terminated: compare exactly
         * name_len bytes, then require every remaining byte (if any) in
         * the 48-byte field to be '\0' so "foo" does not match a stored
         * "foobar...". This never reads past the 48-byte field. */
        size_t j = 0;
        for (; j < name_len; j++) {
            if (e->name[j] != name[j]) {
                break;
            }
        }
        if (j != name_len) {
            continue;
        }

        int rest_is_nul = 1;
        for (size_t k = name_len; k < sizeof(e->name); k++) {
            if (e->name[k] != '\0') {
                rest_is_nul = 0;
                break;
            }
        }
        if (!rest_is_nul) {
            continue;
        }

        return e;
    }

    return NULL;
}
