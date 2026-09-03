/*
 * Host test for aos_gpa_to_hva. No seL4. No libvmm.
 *
 * gcc -I platform/include tests/platform/test_gpa_translate.c \
 *     platform/guest-ram/gpa_translate.c -o test_gpa_translate
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <platform/guest_ram.h>

#define PASS(name) do { printf("  PASS  %s\n", name); return 0; } while (0)
#define FAIL(msg)  do { printf("  FAIL  %s:%d: %s\n", __FILE__, __LINE__, msg); return 1; } while (0)
#define CHECK(cond) do { if (!(cond)) FAIL(#cond); } while (0)

static uint8_t g_fake_ram[4096];

static int test_valid_gpa(void)
{
    aos_guest_ram_t ram;
    void *p;
    uint8_t *hva;

    memset(g_fake_ram, 0xa5, sizeof(g_fake_ram));
    ram.gpa_base = 0x40000000ULL;
    ram.hva_base = (uintptr_t)g_fake_ram;
    ram.size     = sizeof(g_fake_ram);

    p = aos_gpa_to_hva(&ram, 0x40000000ULL, 16u);
    CHECK(p == (void *)g_fake_ram);

    p = aos_gpa_to_hva(&ram, 0x40000010ULL, 4u);
    hva = (uint8_t *)p;
    CHECK(hva == g_fake_ram + 0x10);
    CHECK(hva[0] == 0xa5);

    /* Entire window. */
    p = aos_gpa_to_hva(&ram, 0x40000000ULL, sizeof(g_fake_ram));
    CHECK(p == (void *)g_fake_ram);

    /* Zero-length at last byte and one-past-end. */
    p = aos_gpa_to_hva(&ram, 0x40000000ULL + sizeof(g_fake_ram) - 1u, 0u);
    CHECK(p == (void *)(g_fake_ram + sizeof(g_fake_ram) - 1u));
    p = aos_gpa_to_hva(&ram, 0x40000000ULL + sizeof(g_fake_ram), 0u);
    CHECK(p == (void *)(g_fake_ram + sizeof(g_fake_ram)));

    PASS("test_valid_gpa");
}

static int test_oob_gpa(void)
{
    aos_guest_ram_t ram;

    ram.gpa_base = 0x40000000ULL;
    ram.hva_base = (uintptr_t)g_fake_ram;
    ram.size     = sizeof(g_fake_ram);

    CHECK(aos_gpa_to_hva(&ram, 0x3fffffffULL, 1u) == NULL);
    CHECK(aos_gpa_to_hva(&ram, 0x40000000ULL + sizeof(g_fake_ram), 1u) == NULL);
    CHECK(aos_gpa_to_hva(&ram, 0x40000000ULL + sizeof(g_fake_ram) - 8u, 16u) == NULL);
    CHECK(aos_gpa_to_hva(NULL, 0x40000000ULL, 1u) == NULL);

    ram.size = 0u;
    CHECK(aos_gpa_to_hva(&ram, 0x40000000ULL, 1u) == NULL);

    PASS("test_oob_gpa");
}

static int test_wrap_gpa(void)
{
    aos_guest_ram_t ram;

    ram.gpa_base = 0x40000000ULL;
    ram.hva_base = (uintptr_t)g_fake_ram;
    ram.size     = sizeof(g_fake_ram);

    /* gpa + len wraps uint64. */
    CHECK(aos_gpa_to_hva(&ram, UINT64_MAX - 8u, 16u) == NULL);
    CHECK(aos_gpa_to_hva(&ram, UINT64_MAX, 1u) == NULL);

    /* Window itself wrapping is rejected. */
    ram.gpa_base = UINT64_MAX - 8u;
    ram.size     = 32u;
    CHECK(aos_gpa_to_hva(&ram, ram.gpa_base, 1u) == NULL);

    PASS("test_wrap_gpa");
}

static int test_configured_window(void)
{
    void *p;

    aos_guest_ram_configure(0x40000000ULL, (uintptr_t)g_fake_ram, sizeof(g_fake_ram));
    p = aos_gpa_to_hva_configured(0x40000100ULL, 8u);
    CHECK(p == (void *)(g_fake_ram + 0x100));
    CHECK(aos_gpa_to_hva_configured(0x0ULL, 1u) == NULL);

    aos_guest_ram_configure(0x40000000ULL, (uintptr_t)g_fake_ram, 0u);
    CHECK(aos_gpa_to_hva_configured(0x40000000ULL, 1u) == NULL);

    PASS("test_configured_window");
}

int main(void)
{
    int failed = 0;

    printf("gpa_translate\n");
    failed += test_valid_gpa();
    failed += test_oob_gpa();
    failed += test_wrap_gpa();
    failed += test_configured_window();
    if (failed) {
        printf("%d test(s) failed\n", failed);
        return 1;
    }
    printf("All gpa_translate tests passed\n");
    return 0;
}
