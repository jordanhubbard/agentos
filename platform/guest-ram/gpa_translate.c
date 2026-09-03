#include <platform/guest_ram.h>

static aos_guest_ram_t g_emulated_ram;
static int             g_emulated_ram_ok;

void *aos_gpa_to_hva(const aos_guest_ram_t *ram, uint64_t gpa, size_t len)
{
    uint64_t gpa_end;
    uint64_t off;
    uint64_t ram_end;

    if (ram == NULL || ram->size == 0u) {
        return NULL;
    }

    ram_end = ram->gpa_base + (uint64_t)ram->size;
    /* Window itself must not wrap. */
    if (ram_end < ram->gpa_base) {
        return NULL;
    }

    if (len == 0u) {
        if (gpa < ram->gpa_base || gpa > ram_end) {
            return NULL;
        }
        off = gpa - ram->gpa_base;
        return (void *)(ram->hva_base + (uintptr_t)off);
    }

    gpa_end = gpa + (uint64_t)len;
    if (gpa_end < gpa) {
        return NULL; /* wrap */
    }
    if (gpa < ram->gpa_base || gpa_end > ram_end) {
        return NULL; /* OOB */
    }

    off = gpa - ram->gpa_base;
    return (void *)(ram->hva_base + (uintptr_t)off);
}

void aos_guest_ram_configure(uint64_t gpa_base, uintptr_t hva_base, size_t size)
{
    g_emulated_ram.gpa_base = gpa_base;
    g_emulated_ram.hva_base = hva_base;
    g_emulated_ram.size     = size;
    g_emulated_ram_ok       = (size > 0u) ? 1 : 0;
}

void *aos_gpa_to_hva_configured(uint64_t gpa, size_t len)
{
    if (!g_emulated_ram_ok) {
        return NULL;
    }
    return aos_gpa_to_hva(&g_emulated_ram, gpa, len);
}
