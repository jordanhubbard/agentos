/*
 * Guest virtio-blk path: DTB + VMM fault wiring.
 *
 * Host-only. Does not prove a Linux boot. It does assert:
 *   1. Buildroot DTB advertises IPA 0xa020000 / SPI 20
 *   2. That overlay has no QEMU virtio-blk at 0xa000200
 *   3. linux_vmm pumps after fault_handle
 *   4. VMM logs probed / DRIVER_OK / pumped
 *
 * gcc -I platform/include -DAOS_REPO_ROOT='"/path/"' \
 *     tests/platform/test_virtio_blk_guest_path.c \
 *     -o test_virtio_blk_guest_path
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <platform/blk_layout.h>
#include <platform/net_layout.h>

#ifndef AOS_REPO_ROOT
#define AOS_REPO_ROOT "./"
#endif

static int g_failed;
static int g_testno;

static int tap_ok(int cond, const char *name)
{
    g_testno++;
    if (cond) {
        printf("ok %d - %s\n", g_testno, name);
        return 0;
    }
    printf("not ok %d - %s\n", g_testno, name);
    g_failed++;
    return 1;
}

static char *read_file(const char *rel)
{
    char path[1024];
    FILE *f;
    long sz;
    char *buf;
    size_t n;

    snprintf(path, sizeof(path), "%s%s", AOS_REPO_ROOT, rel);
    f = fopen(path, "rb");
    if (!f) {
        printf("# cannot open %s\n", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    sz = ftell(f);
    if (sz < 0 || sz > 4 * 1024 * 1024) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = (char *)malloc((size_t)sz + 1u);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

static int node_has(const char *node, const char *needle, size_t window)
{
    char tmp[512];
    size_t n;

    if (!node) {
        return 0;
    }
    n = window;
    if (n >= sizeof(tmp)) {
        n = sizeof(tmp) - 1u;
    }
    memcpy(tmp, node, n);
    tmp[n] = '\0';
    return strstr(tmp, needle) != NULL;
}

static int src_contains(const char *rel, const char *needle)
{
    char *text = read_file(rel);
    int ok;

    if (!text) {
        return 0;
    }
    ok = strstr(text, needle) != NULL;
    free(text);
    return ok;
}

static int src_contains_in_order(const char *rel, const char *first, const char *second)
{
    char *text = read_file(rel);
    const char *a;
    const char *b;
    int ok;

    if (!text) {
        return 0;
    }
    a = strstr(text, first);
    b = a ? strstr(a, second) : NULL;
    ok = a != NULL && b != NULL;
    free(text);
    return ok;
}

static int dts_emulated_blk_ok(const char *rel)
{
    char *text = read_file(rel);
    const char *node;
    int ok;

    if (!text) {
        return 0;
    }
    node = strstr(text, "virtio_mmio@a020000");
    ok = node
      && node_has(node, "compatible = \"virtio,mmio\"", 400)
      && node_has(node, "0xa020000", 400)
      && node_has(node, "0x1000", 400)
      && node_has(node, "0x14", 400);
    free(text);
    return ok;
}

static int test_abi(void)
{
    int ok = (AOS_VIRTIO_BLK_GUEST_IPA == 0x0A020000UL)
          && (AOS_VIRTIO_BLK_MMIO_SIZE == 0x1000UL)
          && (AOS_VIRTIO_BLK_VIRQ == 52u)
          && (AOS_VIRTIO_BLK_DTB_SPI == 20u)
          && (AOS_VIRTIO_BLK_VIRQ == 32u + AOS_VIRTIO_BLK_DTB_SPI)
          && (AOS_VIRTIO_BLK_GUEST_IPA != 0x0A000000UL)
          && (AOS_VIRTIO_BLK_GUEST_IPA != 0x0A000200UL)
          && (AOS_VIRTIO_BLK_GUEST_IPA != AOS_VIRTIO_NET_GUEST_IPA)
          && (AOS_VIRTIO_BLK_GUEST_IPA >= 0x0A000000UL + 32u * 0x200u);
    return tap_ok(ok, "abi IPA 0x0A020000 IRQ 52 (SPI 20) outside QEMU page");
}

int main(void)
{
    printf("TAP version 14\n");
    printf("# suite: virtio_blk_guest_path (host pre-filter, not guest-boot proof)\n");

    (void)test_abi();
    (void)tap_ok(dts_emulated_blk_ok(
                     "libvmm/examples/simple/board/qemu_virt_aarch64/overlay.dts"),
                 "DTB buildroot overlay.dts has virtio_mmio@a020000");
    (void)tap_ok(!src_contains("libvmm/examples/simple/board/qemu_virt_aarch64/overlay.dts",
                               "virtio_mmio@a000200"),
                 "buildroot overlay.dts has no QEMU virtio-blk at 0xa000200");
    (void)tap_ok(src_contains("kernel/agentos-root-task/src/linux_vmm.c",
                              "aos_vmm_virtio_blk_init"),
                 "linux_vmm calls aos_vmm_virtio_blk_init");
    (void)tap_ok(src_contains_in_order(
                     "kernel/agentos-root-task/src/linux_vmm.c",
                     "fault_handle(vcpu_id, msginfo)",
                     "aos_vmm_virtio_blk_after_fault()"),
                 "linux_vmm: fault_handle then virtio-blk after_fault");
    (void)tap_ok(src_contains("platform/blk-virt/vmm_virtio_blk.c",
                              "emulated virtio-blk: guest probed"),
                 "VMM logs guest virtio-blk probe");
    (void)tap_ok(src_contains("platform/blk-virt/vmm_virtio_blk.c",
                              "emulated virtio-blk: guest DRIVER_OK"),
                 "VMM logs guest virtio-blk DRIVER_OK");
    (void)tap_ok(src_contains("platform/blk-virt/vmm_virtio_blk.c",
                              "emulated virtio-blk: pumped"),
                 "VMM logs first pumped blk request");
    (void)tap_ok(src_contains("libvmm/src/virtio/block.c",
                              "virtio_copy_from_gpa") &&
                 src_contains("libvmm/src/virtio/block.c",
                              "virtio_copy_to_gpa") &&
                 !src_contains("libvmm/src/virtio/block.c",
                               "(void *)virtq->desc[curr_desc].addr"),
                 "libvmm block payloads use bounds-checked GPA translation");
    (void)tap_ok(src_contains("kernel/agentos-root-task/src/main.c",
                              "AGENTOS_HOST_BLK_MMIO_PA") &&
                 src_contains("kernel/agentos-root-task/src/main.c",
                              "AGENTOS_BLK_SHARED_VA") &&
                 src_contains("kernel/agentos-root-task/src/main.c",
                              "name_eq(pd->name, \"virtio_blk\")"),
                 "root maps isolated host MMIO and shared DMA to virtio_blk");
    (void)tap_ok(src_contains("kernel/agentos-root-task/src/virtio_blk.c",
                              "shared->paddr") &&
                 src_contains("kernel/agentos-root-task/src/virtio_blk.c",
                              "VIRTIO_MMIO_DRIVER_FEATURES, 1u") &&
                 src_contains("kernel/agentos-root-task/include/virtio_blk.h",
                              "VIRTIO_BLK_QUEUE_SIZE           8u"),
                 "host driver uses explicit DMA PA and valid modern queue");
    (void)tap_ok(src_contains("xtask/src/cmd_test.rs",
                              "virtio-mmio-bus.8") &&
                 src_contains("xtask/src/cmd_test.rs",
                              "drive=agentos_hd"),
                 "Ubuntu QEMU launch attaches ISO only as agentOS host hardware");
    (void)tap_ok(src_contains("kernel/agentos-root-task/src/system_desc_aarch64.c",
                              "{ SVC_ID_VIRTIO_BLK, 12u }") &&
                 src_contains("platform/blk-virt/vmm_virtio_blk.c",
                              "aos_blk_virt_set_backend") &&
                 src_contains("platform/blk-virt/vmm_virtio_blk.c",
                              "host-media read"),
                 "linux_vmm routes emulated block requests to virtio_blk");
    (void)tap_ok(src_contains("kernel/agentos-root-task/Makefile",
                              "-DAGENTOS_GUEST_UBUNTU=1") &&
                 src_contains_in_order("kernel/agentos-root-task/src/main.c",
                                       "#if !defined(AGENTOS_GUEST_UBUNTU)",
                                       "g_virtio_mmio_frame_cap"),
                 "Ubuntu guest VSpace excludes the QEMU passthrough page");

    printf("1..%d\n", g_testno);
    if (g_failed) {
        printf("# %d failed (host pre-filter only)\n", g_failed);
        return 1;
    }
    printf("# all virtio_blk_guest_path tests passed (not boot-proven)\n");
    return 0;
}
