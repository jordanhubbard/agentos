/*
 * test_boot_manifest.c — host test for the boot manifest format/validator.
 *
 * Builds blobs by hand in local buffers (no fixture files) and exercises
 * aos_boot_manifest_validate() / aos_boot_manifest_find() against each
 * rejection reason plus the happy path. This is a host-only structural
 * pre-filter (-DAGENTOS_TEST_HOST): it proves nothing about the Ed25519
 * signature, which this task does not check.
 */

#include "boot_manifest.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* A two-entry manifest, built by hand, with room to spare before the
 * trailing signature so truncation tests can shrink `len` precisely. */
typedef struct {
    aos_boot_manifest_hdr_t   hdr;
    aos_boot_manifest_entry_t entries[2];
    uint8_t                   sig[AOS_BOOT_MANIFEST_SIG_LEN];
} __attribute__((packed)) two_entry_blob_t;

static void fill_well_formed(two_entry_blob_t *b)
{
    memset(b, 0, sizeof(*b));
    b->hdr.magic = AOS_BOOT_MANIFEST_MAGIC;
    b->hdr.version = AOS_BOOT_MANIFEST_VERSION;
    b->hdr.count = 2;

    memcpy(b->entries[0].name, "controller", strlen("controller"));
    memset(b->entries[0].sha256, 0x11, sizeof(b->entries[0].sha256));

    /* Name filling all 48 bytes, deliberately with no NUL terminator
     * anywhere in the field. */
    memset(b->entries[1].name, 'x', sizeof(b->entries[1].name));
    memset(b->entries[1].sha256, 0x22, sizeof(b->entries[1].sha256));

    memset(b->sig, 0xAA, sizeof(b->sig));
}

static void test_well_formed_validates(void)
{
    two_entry_blob_t b;
    fill_well_formed(&b);

    int rc = aos_boot_manifest_validate((const uint8_t *)&b, (uint32_t)sizeof(b));
    assert(rc == AOS_BOOT_MANIFEST_OK);
    printf("PASS: well-formed blob validates\n");
}

static void test_too_short_header(void)
{
    uint8_t tiny[4] = {0};
    int rc = aos_boot_manifest_validate(tiny, sizeof(tiny));
    assert(rc == AOS_BOOT_MANIFEST_ERR_TOO_SHORT);

    /* Exactly one byte short of a full header must also be rejected. */
    uint8_t almost[sizeof(aos_boot_manifest_hdr_t) - 1];
    memset(almost, 0, sizeof(almost));
    rc = aos_boot_manifest_validate(almost, (uint32_t)sizeof(almost));
    assert(rc == AOS_BOOT_MANIFEST_ERR_TOO_SHORT);

    printf("PASS: blob shorter than header rejected\n");
}

static void test_truncated_entries_not_overread(void)
{
    two_entry_blob_t b;
    fill_well_formed(&b);

    /* Header claims 2 entries + signature, but len only covers the
     * header and a sliver of the first entry. A sanitizer/ASan build
     * running this test would catch any over-read here. */
    uint32_t short_len = (uint32_t)sizeof(b.hdr) + 4;
    int rc = aos_boot_manifest_validate((const uint8_t *)&b, short_len);
    assert(rc == AOS_BOOT_MANIFEST_ERR_TRUNCATED);

    /* One byte short of the full required size (header + entries + sig)
     * must still be rejected — off-by-one at the boundary. */
    rc = aos_boot_manifest_validate((const uint8_t *)&b, (uint32_t)sizeof(b) - 1);
    assert(rc == AOS_BOOT_MANIFEST_ERR_TRUNCATED);

    printf("PASS: declared count exceeding len rejected without over-reading\n");
}

static void test_wrong_magic(void)
{
    two_entry_blob_t b;
    fill_well_formed(&b);
    b.hdr.magic = UINT64_C(0xdeadbeefdeadbeef);

    int rc = aos_boot_manifest_validate((const uint8_t *)&b, (uint32_t)sizeof(b));
    assert(rc == AOS_BOOT_MANIFEST_ERR_MAGIC);
    printf("PASS: wrong magic rejected\n");
}

static void test_wrong_version(void)
{
    two_entry_blob_t b;
    fill_well_formed(&b);
    b.hdr.version = AOS_BOOT_MANIFEST_VERSION + 1;

    int rc = aos_boot_manifest_validate((const uint8_t *)&b, (uint32_t)sizeof(b));
    assert(rc == AOS_BOOT_MANIFEST_ERR_VERSION);
    printf("PASS: wrong version rejected\n");
}

static void test_count_zero_rejected(void)
{
    two_entry_blob_t b;
    fill_well_formed(&b);
    b.hdr.count = 0;

    int rc = aos_boot_manifest_validate((const uint8_t *)&b, (uint32_t)sizeof(b));
    assert(rc == AOS_BOOT_MANIFEST_ERR_COUNT_ZERO);
    printf("PASS: count == 0 rejected (empty manifest is not \"nothing to verify\")\n");
}

static void test_count_exceeds_max(void)
{
    two_entry_blob_t b;
    fill_well_formed(&b);
    b.hdr.count = AOS_BOOT_MANIFEST_MAX_PDS + 1;

    int rc = aos_boot_manifest_validate((const uint8_t *)&b, (uint32_t)sizeof(b));
    assert(rc == AOS_BOOT_MANIFEST_ERR_COUNT_MAX);
    printf("PASS: count exceeding AOS_BOOT_MANIFEST_MAX_PDS rejected\n");
}

static void test_count_near_uint32_max_no_overread(void)
{
    /* The classic attacker-controlled-multiplication case: count large
     * enough that count * sizeof(entry) would overflow a 32-bit
     * computation. Must be rejected via the MAX_PDS bound, well before
     * any overflow-prone arithmetic, and must not read past the buffer. */
    two_entry_blob_t b;
    fill_well_formed(&b);
    b.hdr.count = 0xFFFFFFFFu;

    int rc = aos_boot_manifest_validate((const uint8_t *)&b, (uint32_t)sizeof(b));
    assert(rc == AOS_BOOT_MANIFEST_ERR_COUNT_MAX);
    printf("PASS: huge count rejected via MAX_PDS bound, no overflow/over-read\n");
}

static void test_find_present_and_absent(void)
{
    two_entry_blob_t b;
    fill_well_formed(&b);

    int rc = aos_boot_manifest_validate((const uint8_t *)&b, (uint32_t)sizeof(b));
    assert(rc == AOS_BOOT_MANIFEST_OK);

    const aos_boot_manifest_entry_t *found =
        aos_boot_manifest_find((const uint8_t *)&b, (uint32_t)sizeof(b), "controller");
    assert(found != NULL);
    assert(found == &b.entries[0]);
    assert(memcmp(found->sha256, b.entries[0].sha256, 32) == 0);

    const aos_boot_manifest_entry_t *absent =
        aos_boot_manifest_find((const uint8_t *)&b, (uint32_t)sizeof(b), "no-such-pd");
    assert(absent == NULL);

    printf("PASS: find returns the right entry by name, NULL when absent\n");
}

static void test_find_non_nul_terminated_name_no_overread(void)
{
    two_entry_blob_t b;
    fill_well_formed(&b);

    int rc = aos_boot_manifest_validate((const uint8_t *)&b, (uint32_t)sizeof(b));
    assert(rc == AOS_BOOT_MANIFEST_OK);

    /* entries[1].name is 48 bytes of 'x' with no NUL terminator anywhere
     * in the field. A 48-byte search key of the same bytes must match
     * without reading a 49th byte of either the key or the stored name. */
    char key[AOS_BOOT_MANIFEST_NAME_LEN + 1];
    memset(key, 'x', AOS_BOOT_MANIFEST_NAME_LEN);
    key[AOS_BOOT_MANIFEST_NAME_LEN] = '\0';

    const aos_boot_manifest_entry_t *found =
        aos_boot_manifest_find((const uint8_t *)&b, (uint32_t)sizeof(b), key);
    assert(found != NULL);
    assert(found == &b.entries[1]);

    /* A key one byte longer than the field can never match. */
    char too_long[AOS_BOOT_MANIFEST_NAME_LEN + 2];
    memset(too_long, 'x', AOS_BOOT_MANIFEST_NAME_LEN + 1);
    too_long[AOS_BOOT_MANIFEST_NAME_LEN + 1] = '\0';
    const aos_boot_manifest_entry_t *none =
        aos_boot_manifest_find((const uint8_t *)&b, (uint32_t)sizeof(b), too_long);
    assert(none == NULL);

    printf("PASS: 48-byte non-NUL-terminated name matched without over-read\n");
}

int main(void)
{
    test_well_formed_validates();
    test_too_short_header();
    test_truncated_entries_not_overread();
    test_wrong_magic();
    test_wrong_version();
    test_count_zero_rejected();
    test_count_exceeds_max();
    test_count_near_uint32_max_no_overread();
    test_find_present_and_absent();
    test_find_non_nul_terminated_name_no_overread();

    printf("All boot_manifest tests passed.\n");
    return 0;
}
