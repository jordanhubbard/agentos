/*
 * boot_manifest_roundtrip_check.c — ABI round-trip checker for the boot
 * manifest format.
 *
 * This program is NOT part of any test suite by itself.  It exists only to
 * be compiled and run on demand by the Rust-side round-trip test in
 * xtask/src/boot_manifest.rs: that test builds a manifest blob with the
 * Rust writer, writes it to a temp file, and shells out to a `cc` build of
 * this program to validate it with the real C reader
 * (kernel/agentos-root-task/src/boot_manifest.c).  If the Rust writer and
 * the C reader ever disagree on layout, this fails loudly here instead of
 * surfacing later as a mysterious boot refusal.
 *
 * Usage: boot_manifest_roundtrip_check <blob-path> <pd-name> <hex-sha256>
 *   exit 0  — blob validates, entry found, digest matches (prints "OK")
 *   exit 1  — blob read but failed validation / lookup / digest compare
 *   exit 2  — usage / I/O error (not an ABI finding)
 */

#include "boot_manifest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hex_to_bytes(const char *hex, uint8_t *out, size_t out_len)
{
    if (strlen(hex) != out_len * 2) {
        return -1;
    }
    for (size_t i = 0; i < out_len; i++) {
        unsigned byte = 0;
        if (sscanf(hex + 2 * i, "%2x", &byte) != 1) {
            return -1;
        }
        out[i] = (uint8_t)byte;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s <blob-path> <pd-name> <hex-sha256>\n", argv[0]);
        return 2;
    }

    const char *blob_path = argv[1];
    const char *pd_name   = argv[2];
    const char *hex_sha   = argv[3];

    FILE *f = fopen(blob_path, "rb");
    if (!f) {
        perror("fopen");
        return 2;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        perror("fseek");
        fclose(f);
        return 2;
    }
    long sz = ftell(f);
    if (sz < 0) {
        perror("ftell");
        fclose(f);
        return 2;
    }
    rewind(f);

    uint8_t *buf = malloc((size_t)sz);
    if (!buf) {
        fprintf(stderr, "out of memory\n");
        fclose(f);
        return 2;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "short read on %s\n", blob_path);
        free(buf);
        fclose(f);
        return 2;
    }
    fclose(f);

    int vrc = aos_boot_manifest_validate(buf, (uint32_t)sz);
    if (vrc != AOS_BOOT_MANIFEST_OK) {
        fprintf(stderr, "aos_boot_manifest_validate failed: %d\n", vrc);
        free(buf);
        return 1;
    }

    const aos_boot_manifest_entry_t *e =
        aos_boot_manifest_find(buf, (uint32_t)sz, pd_name);
    if (!e) {
        fprintf(stderr, "aos_boot_manifest_find: no entry for '%s'\n", pd_name);
        free(buf);
        return 1;
    }

    uint8_t expected[32];
    if (hex_to_bytes(hex_sha, expected, sizeof(expected)) != 0) {
        fprintf(stderr, "bad hex sha256 argument\n");
        free(buf);
        return 2;
    }

    if (memcmp(e->sha256, expected, sizeof(expected)) != 0) {
        fprintf(stderr, "sha256 mismatch for '%s'\n", pd_name);
        free(buf);
        return 1;
    }

    free(buf);
    printf("OK\n");
    return 0;
}
