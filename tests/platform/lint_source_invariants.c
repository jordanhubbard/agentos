/*
 * lint_source_invariants.c — SOURCE LINT, NOT A TEST.
 *
 * This program proves no runtime behaviour. It reads checked-in artifacts
 * (headers, the compiled AArch64 topology, guest FDT templates, guest
 * profiles, QEMU launch tooling) and fails when one of the architecture
 * invariants from docs/TCB.md ("I/O invariant", items 1-5) is no longer
 * visible in the source tree.
 *
 * It is wired into `make test-host` via `make lint-source`, alongside
 * `policy-check`. It is deliberately NOT counted as a guest-path or I/O test:
 * a green run here says "the tree still describes the intended architecture",
 * never "the OS does I/O". Runtime proof is `make gate` / `gate-guest-io`.
 *
 * Rules for adding a check:
 *   - Phrase it as the invariant it protects, not as the code that happens
 *     to implement it today.
 *   - Prefer compiled data (headers, system_desc) over text. Text checks are
 *     acceptable only for data files (FDT, TOML, launch args), and negative
 *     text checks for patterns that must never reappear.
 *   - Do not pin function names, log strings, Makefile lines, or comments.
 *     Those belong in a behavioural test or nowhere.
 *
 * Output is TAP so it runs in the same harness as the host suite.
 */

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <platform/blk_host_layout.h>
#include <platform/blk_layout.h>
#include <platform/net_layout.h>
#include <platform/serial_layout.h>
#include "contracts/block-service/interface.h"
#include "system_desc.h"

#ifndef AOS_REPO_ROOT
#define AOS_REPO_ROOT "./"
#endif

extern const system_desc_t system_desc_aarch64;

static int g_failed;
static int g_checkno;

static void ok(int condition, const char *invariant)
{
    g_checkno++;
    printf("%s %d - %s\n", condition ? "ok" : "not ok", g_checkno, invariant);
    if (!condition) {
        g_failed++;
    }
}

/* ── file helpers ─────────────────────────────────────────────────────── */

static char *read_file(const char *relative)
{
    char path[1024];
    FILE *file;
    long size;
    char *text;
    size_t read_size;

    snprintf(path, sizeof(path), "%s%s", AOS_REPO_ROOT, relative);
    file = fopen(path, "rb");
    if (!file) {
        printf("# cannot open %s\n", path);
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    size = ftell(file);
    if (size < 0 || size > 4 * 1024 * 1024) {
        fclose(file);
        return NULL;
    }
    rewind(file);
    text = malloc((size_t)size + 1u);
    if (!text) {
        fclose(file);
        return NULL;
    }
    read_size = fread(text, 1u, (size_t)size, file);
    fclose(file);
    text[read_size] = '\0';
    return text;
}

static int contains(const char *relative, const char *needle)
{
    char *text = read_file(relative);
    int found = text && strstr(text, needle) != NULL;
    free(text);
    return found;
}

/* Does any file in dir ending in suffix contain needle? Returns the count. */
static int dir_files_containing(const char *dir, const char *suffix,
                                const char *needle)
{
    char path[1024];
    DIR *d;
    struct dirent *ent;
    int hits = 0;

    snprintf(path, sizeof(path), "%s%s", AOS_REPO_ROOT, dir);
    d = opendir(path);
    if (!d) {
        printf("# cannot open dir %s\n", path);
        return -1;
    }
    while ((ent = readdir(d)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        size_t slen = strlen(suffix);
        char rel[1024];

        if (nlen < slen || strcmp(ent->d_name + nlen - slen, suffix) != 0) {
            continue;
        }
        snprintf(rel, sizeof(rel), "%s/%s", dir, ent->d_name);
        if (contains(rel, needle)) {
            printf("# %s contains \"%s\"\n", rel, needle);
            hits++;
        }
    }
    closedir(d);
    return hits;
}

/* ── guest FDT discovery from guest-profiles TOML ─────────────────────── */

#define MAX_DTS 16
#define DTS_PATH_MAX 256

typedef struct {
    char path[DTS_PATH_MAX];
    int is_template; /* template = ... (adds agentOS nodes) vs base = ... */
} dts_ref_t;

static dts_ref_t g_dts[MAX_DTS];
static int g_dts_count;

static void add_dts(const char *path, int is_template)
{
    int i;

    for (i = 0; i < g_dts_count; i++) {
        if (strcmp(g_dts[i].path, path) == 0) {
            g_dts[i].is_template |= is_template;
            return;
        }
    }
    if (g_dts_count < MAX_DTS) {
        snprintf(g_dts[g_dts_count].path, DTS_PATH_MAX, "%s", path);
        g_dts[g_dts_count].is_template = is_template;
        g_dts_count++;
    }
}

/* Collect every `key = "value"` line (key = base|template) whose value ends
 * in .dts or .dts.in. Line-based on purpose: this is a lint over data files,
 * not a TOML parser. */
static void collect_dts_from_profile(const char *rel)
{
    char *text = read_file(rel);
    char *line;
    char *save = NULL;

    if (!text) {
        return;
    }
    for (line = strtok_r(text, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        const char *key = NULL;
        const char *p = line;
        int is_template = 0;
        const char *q1;
        const char *q2;
        char value[DTS_PATH_MAX];
        size_t vlen;

        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (strncmp(p, "template", 8) == 0) {
            key = p + 8;
            is_template = 1;
        } else if (strncmp(p, "base", 4) == 0) {
            key = p + 4;
        } else {
            continue;
        }
        while (*key == ' ' || *key == '\t') {
            key++;
        }
        if (*key != '=') {
            continue;
        }
        q1 = strchr(key, '"');
        q2 = q1 ? strchr(q1 + 1, '"') : NULL;
        if (!q1 || !q2) {
            continue;
        }
        vlen = (size_t)(q2 - q1 - 1);
        if (vlen == 0 || vlen >= sizeof(value)) {
            continue;
        }
        memcpy(value, q1 + 1, vlen);
        value[vlen] = '\0';
        if (strstr(value, ".dts") == NULL) {
            continue;
        }
        add_dts(value, is_template);
    }
    free(text);
}

static void collect_guest_dts(void)
{
    char path[1024];
    DIR *d;
    struct dirent *ent;

    snprintf(path, sizeof(path), "%sguest-profiles", AOS_REPO_ROOT);
    d = opendir(path);
    if (!d) {
        printf("# cannot open dir %s\n", path);
        return;
    }
    while ((ent = readdir(d)) != NULL) {
        size_t n = strlen(ent->d_name);
        char rel[1024];

        if (n < 5 || strcmp(ent->d_name + n - 5, ".toml") != 0) {
            continue;
        }
        snprintf(rel, sizeof(rel), "guest-profiles/%s", ent->d_name);
        collect_dts_from_profile(rel);
    }
    closedir(d);
}

/* A virtio_mmio node at `ipa` with GIC SPI `spi` (interrupts = <0 spi 1>),
 * page-sized reg, within 400 bytes of the node label. The needles are built
 * from the platform headers so the FDT is checked against the ABI, not
 * against a literal copied into this file. */
static int dts_advertises(const char *rel, unsigned long ipa, unsigned spi)
{
    char *text = read_file(rel);
    char label[64];
    char reg[64];
    char irq[64];
    char window[401];
    const char *node;
    size_t n;
    int found;

    if (!text) {
        return 0;
    }
    snprintf(label, sizeof(label), "virtio_mmio@%lx", ipa);
    snprintf(reg, sizeof(reg), "0x%lx 0x00 0x1000", ipa);
    snprintf(irq, sizeof(irq), "interrupts = <0x00 0x%x 0x01>", spi);
    node = strstr(text, label);
    if (!node) {
        free(text);
        return 0;
    }
    n = strlen(node);
    if (n > 400) {
        n = 400;
    }
    memcpy(window, node, n);
    window[n] = '\0';
    found = strstr(window, "compatible = \"virtio,mmio\"") != NULL &&
            strstr(window, reg) != NULL &&
            strstr(window, irq) != NULL;
    free(text);
    return found;
}

/* ── topology helpers ─────────────────────────────────────────────────── */

#define QEMU_VIRT_UART_PA   0x09000000ULL
#define QEMU_VIRT_UART_IRQ  33u

static const pd_desc_t *find_pd(const char *name)
{
    uint32_t i;

    for (i = 0u; i < system_desc_aarch64.pd_count; i++) {
        if (strcmp(system_desc_aarch64.pds[i].name, name) == 0) {
            return &system_desc_aarch64.pds[i];
        }
    }
    return NULL;
}

static unsigned frame_owner_count(uint64_t paddr, const pd_desc_t **owner)
{
    unsigned count = 0u;
    uint32_t i;
    uint8_t j;

    *owner = NULL;
    for (i = 0u; i < system_desc_aarch64.pd_count; i++) {
        const pd_desc_t *pd = &system_desc_aarch64.pds[i];
        for (j = 0u; j < pd->device_frame_count; j++) {
            if (pd->device_frames[j].paddr == paddr) {
                count++;
                *owner = pd;
            }
        }
    }
    return count;
}

static unsigned irq_owner_count(uint32_t irq, const pd_desc_t **owner)
{
    unsigned count = 0u;
    uint32_t i;
    uint8_t j;

    *owner = NULL;
    for (i = 0u; i < system_desc_aarch64.pd_count; i++) {
        const pd_desc_t *pd = &system_desc_aarch64.pds[i];
        for (j = 0u; j < pd->irq_count; j++) {
            if (pd->irqs[j].irq_number == irq) {
                count++;
                *owner = pd;
            }
        }
    }
    return count;
}

/* Every device frame and every IRQ in the topology has exactly one owner. */
static int topology_single_owner(void)
{
    uint32_t i;
    uint8_t j;
    int unique = 1;

    for (i = 0u; i < system_desc_aarch64.pd_count; i++) {
        const pd_desc_t *pd = &system_desc_aarch64.pds[i];
        for (j = 0u; j < pd->device_frame_count; j++) {
            const pd_desc_t *owner;
            if (frame_owner_count(pd->device_frames[j].paddr, &owner) != 1u) {
                printf("# device frame 0x%llx has more than one owner\n",
                       (unsigned long long)pd->device_frames[j].paddr);
                unique = 0;
            }
        }
        for (j = 0u; j < pd->irq_count; j++) {
            const pd_desc_t *owner;
            if (irq_owner_count(pd->irqs[j].irq_number, &owner) != 1u) {
                printf("# IRQ %u has more than one owner\n",
                       pd->irqs[j].irq_number);
                unique = 0;
            }
        }
    }
    return unique;
}

/* Guest VMM PDs receive guest RAM and endpoints, never a device frame or
 * hardware IRQ; all device access goes through a virtualizer. */
static int guest_vmms_own_no_hardware(void)
{
    uint32_t i;
    int seen = 0;
    int clean = 1;

    for (i = 0u; i < system_desc_aarch64.pd_count; i++) {
        const pd_desc_t *pd = &system_desc_aarch64.pds[i];
        if (strncmp(pd->name, "guest_vmm", 9) != 0) {
            continue;
        }
        seen++;
        if (pd->device_frame_count != 0u || pd->irq_count != 0u) {
            printf("# %s owns %u device frame(s) and %u IRQ(s)\n", pd->name,
                   pd->device_frame_count, pd->irq_count);
            clean = 0;
        }
    }
    return seen > 0 && clean;
}

/* ── guest ABI: emulated virtio devices live outside the QEMU host window ── */

/* QEMU virt places 32 host virtio-mmio transports at 0x0A000000 + n*0x200. */
#define QEMU_VIRTIO_MMIO_BASE 0x0A000000UL
#define QEMU_VIRTIO_MMIO_END  (QEMU_VIRTIO_MMIO_BASE + 32u * 0x200u)

static int emulated_ipa_outside_host_window(unsigned long ipa,
                                            unsigned long size,
                                            unsigned virq, unsigned spi)
{
    return size == 0x1000UL &&
           (ipa & 0xFFFUL) == 0UL &&
           (ipa >= QEMU_VIRTIO_MMIO_END || ipa + size <= QEMU_VIRTIO_MMIO_BASE) &&
           virq == 32u + spi;
}

int main(void)
{
    const pd_desc_t *owner;
    int i;

    printf("TAP version 14\n");
    printf("# lint: source invariants (docs/TCB.md I/O invariant 1-5)\n");
    printf("# this is a source lint, not a guest-path or I/O test\n");

    /* ── TCB invariant 1: one owner per device frame and IRQ ─────────── */

    ok(topology_single_owner(),
       "inv1: every device frame and IRQ in the AArch64 topology has exactly one owner PD");
    ok(find_pd("serial_pd") != NULL,
       "inv1: serial_pd exists in the AArch64 topology");
    ok(frame_owner_count(QEMU_VIRT_UART_PA, &owner) == 1u && owner &&
       strcmp(owner->name, "serial_pd") == 0,
       "inv1: the PL011 frame (0x09000000) is owned by serial_pd and no other PD");
    ok(irq_owner_count(QEMU_VIRT_UART_IRQ, &owner) == 1u && owner &&
       strcmp(owner->name, "serial_pd") == 0,
       "inv1: the PL011 IRQ (33) is owned by serial_pd and no other PD");

    /* ── TCB invariant 5: no guest host-device passthrough ────────────── */

    ok(guest_vmms_own_no_hardware(),
       "inv5: guest VMM PDs own no device frame and no hardware IRQ");
    ok(dir_files_containing("xtask/src", ".rs", "bus=virtio-mmio-bus.0") == 0 &&
       !contains("Makefile", "bus=virtio-mmio-bus.0"),
       "inv5: no QEMU launch plan attaches a virtio device to virtio-mmio-bus.0 (the guest-visible transport)");

    /* ── TCB invariant 3: emulated virtio is a different device from host virtio ── */

    ok(emulated_ipa_outside_host_window(AOS_VIRTIO_NET_GUEST_IPA,
                                        AOS_VIRTIO_NET_MMIO_SIZE,
                                        AOS_VIRTIO_NET_VIRQ,
                                        AOS_VIRTIO_NET_DTB_SPI),
       "inv3: emulated virtio-net IPA is a page outside the QEMU host virtio-mmio window, VIRQ = 32 + SPI");
    ok(emulated_ipa_outside_host_window(AOS_VIRTIO_BLK_GUEST_IPA,
                                        AOS_VIRTIO_BLK_MMIO_SIZE,
                                        AOS_VIRTIO_BLK_VIRQ,
                                        AOS_VIRTIO_BLK_DTB_SPI),
       "inv3: emulated virtio-blk IPA is a page outside the QEMU host virtio-mmio window, VIRQ = 32 + SPI");
    ok(emulated_ipa_outside_host_window(AOS_VIRTIO_CONSOLE_GUEST_IPA,
                                        AOS_VIRTIO_CONSOLE_MMIO_SIZE,
                                        AOS_VIRTIO_CONSOLE_VIRQ,
                                        AOS_VIRTIO_CONSOLE_DTB_SPI),
       "inv3: emulated virtio-console IPA is a page outside the QEMU host virtio-mmio window, VIRQ = 32 + SPI");
    ok(AOS_VIRTIO_NET_GUEST_IPA != AOS_VIRTIO_BLK_GUEST_IPA &&
       AOS_VIRTIO_NET_GUEST_IPA != AOS_VIRTIO_CONSOLE_GUEST_IPA &&
       AOS_VIRTIO_BLK_GUEST_IPA != AOS_VIRTIO_CONSOLE_GUEST_IPA &&
       AOS_VIRTIO_NET_DTB_SPI != AOS_VIRTIO_BLK_DTB_SPI &&
       AOS_VIRTIO_NET_DTB_SPI != AOS_VIRTIO_CONSOLE_DTB_SPI &&
       AOS_VIRTIO_BLK_DTB_SPI != AOS_VIRTIO_CONSOLE_DTB_SPI,
       "inv3: emulated net/blk/console use distinct IPAs and distinct SPIs");

    /* ── TCB invariants 4 + 5: guests are image + FDT; the FDT advertises only
     *    the emulated devices and never a QEMU host transport ─────────── */

    collect_guest_dts();
    ok(g_dts_count > 0,
       "inv4: guest profiles reference at least one FDT base/template");
    for (i = 0; i < g_dts_count; i++) {
        char name[DTS_PATH_MAX + 96];

        snprintf(name, sizeof(name),
                 "inv5: %s advertises no QEMU host virtio-mmio node (virtio_mmio@a00xxxx)",
                 g_dts[i].path);
        ok(!contains(g_dts[i].path, "virtio_mmio@a00"), name);

        if (!g_dts[i].is_template) {
            continue;
        }
        snprintf(name, sizeof(name),
                 "inv4: %s advertises the emulated virtio-net at the header IPA/SPI",
                 g_dts[i].path);
        ok(dts_advertises(g_dts[i].path, AOS_VIRTIO_NET_GUEST_IPA,
                          AOS_VIRTIO_NET_DTB_SPI), name);
        snprintf(name, sizeof(name),
                 "inv4: %s advertises the emulated virtio-blk at the header IPA/SPI",
                 g_dts[i].path);
        ok(dts_advertises(g_dts[i].path, AOS_VIRTIO_BLK_GUEST_IPA,
                          AOS_VIRTIO_BLK_DTB_SPI), name);
        snprintf(name, sizeof(name),
                 "inv4: %s advertises the emulated virtio-console at the header IPA/SPI",
                 g_dts[i].path);
        ok(dts_advertises(g_dts[i].path, AOS_VIRTIO_CONSOLE_GUEST_IPA,
                          AOS_VIRTIO_CONSOLE_DTB_SPI), name);
    }

    /* ── TCB invariant 5: the VMM translates guest GPA; libvmm device models
     *    never cast a guest descriptor address to a host pointer ───────── */
    {
        static const char *const models[] = {
            "libvmm/src/virtio/net.c",
            "libvmm/src/virtio/block.c",
            "libvmm/src/virtio/console.c",
            "libvmm/src/virtio/sound.c",
        };
        size_t m;

        for (m = 0; m < sizeof(models) / sizeof(models[0]); m++) {
            char name[256];

            snprintf(name, sizeof(name),
                     "inv5: %s reaches guest memory only through the GPA translation API",
                     models[m]);
            ok((contains(models[m], "virtio_copy_from_gpa") ||
                contains(models[m], "virtio_copy_to_gpa") ||
                contains(models[m], "virtio_gpa_to_hva")) &&
               !contains(models[m], "(void *)virtq->desc[") &&
               !contains(models[m], "(void *)desc->addr") &&
               !contains(models[m], "(uintptr_t)virtq->desc[") &&
               !contains(models[m], "(uintptr_t)desc->addr"),
               name);
        }
    }

    /* ── TCB invariant 2: virtualizer queues are the mux; shared block DMA
     *    windows are disjoint per media and fit the shared region ─────── */

    ok(sizeof(blk_svc_req_t) == 20u &&
       BLK_SVC_MEDIA_PRIMARY == AOS_HOST_BLK_MEDIA_PRIMARY &&
       BLK_SVC_MEDIA_SECONDARY == AOS_HOST_BLK_MEDIA_SECONDARY &&
       BLK_SVC_MEDIA_COUNT == AOS_HOST_BLK_MEDIA_COUNT,
       "inv2: block-service request is a 20-byte packed wire struct and its media ids match the host block layout");
    ok(AGENTOS_BLK_MEDIA_DMA_OFF(1u) + AGENTOS_BLK_MEDIA_DMA_SIZE(1u) <=
           AGENTOS_BLK_MEDIA_DMA_OFF(0u) &&
       AGENTOS_BLK_MEDIA_DMA_OFF(0u) + AGENTOS_BLK_MEDIA_DMA_SIZE(0u) <=
           AGENTOS_BLK_SHARED_SIZE &&
       AGENTOS_BLK_SHARED_DMA_DATA_OFF +
           AGENTOS_BLK_MEDIA_DMA_MAX_SECTORS(0u) * AOS_HOST_BLK_SECTOR_SIZE <=
           AGENTOS_BLK_MEDIA_DMA_SIZE(0u) &&
       AGENTOS_BLK_SHARED_DMA_DATA_OFF +
           AGENTOS_BLK_MEDIA_DMA_MAX_SECTORS(1u) * AOS_HOST_BLK_SECTOR_SIZE <=
           AGENTOS_BLK_MEDIA_DMA_SIZE(1u) &&
       AOS_BLK_GUEST_MAX_SEGMENT_SIZE <= AOS_BLK_DATA_BYTES,
       "inv2: per-media block DMA windows are disjoint, bounded by their sector limits, and fit the shared region");

    /* ── Guest-control liveness: the control relay chain outranks the guests
     *    it services (vm_manager > vibe_engine > cc_pd). Not a TCB.md item;
     *    kept because a regression deadlocks guest create/attach. ──────── */
    {
        const pd_desc_t *vmm = find_pd("vm_manager");
        const pd_desc_t *vibe = find_pd("vibe_engine");
        const pd_desc_t *cc = find_pd("cc_pd");

        ok(vmm && vibe && cc &&
           vmm->priority > vibe->priority && vibe->priority > cc->priority,
           "liveness: guest-control priorities are strictly ordered vm_manager > vibe_engine > cc_pd");
    }

    /* ── TCB.md museum: the default image spawns TCB PDs plus vibe_engine
     *    only.  The non-TCB service PDs dropped by MAC
     *    task_f95d118416a24fa484c2c43f0d955b56 must not creep back into the
     *    descriptor (event_bus is test-image-only). ─────────────────────── */
    {
        static const char *const dropped[] = {
            "controller", "event_bus", "init_agent", "agentfs", "vfs_server",
            "net_server", "framebuffer_pd", "usb_pd",
        };
        size_t d;

        for (d = 0; d < sizeof(dropped) / sizeof(dropped[0]); d++) {
            char name[128];

            snprintf(name, sizeof(name),
                     "museum: %s is not spawned by the default aarch64 descriptor",
                     dropped[d]);
            ok(find_pd(dropped[d]) == NULL, name);
        }
        ok(find_pd("cc_pd") != NULL && contains("kernel/agentos-root-task/src/cc_pd.c",
                                                "agentOS boot complete"),
           "museum: cc_pd prints the harness boot-complete marker (controller is gone)");
    }

    printf("1..%d\n", g_checkno);
    if (g_failed) {
        printf("# %d invariant(s) not visible in source (lint, not proof)\n",
               g_failed);
        return 1;
    }
    printf("# source invariants hold (lint only; not I/O proof)\n");
    return 0;
}
