/*
 * Guest virtio-console path: ABI, Ubuntu DTB, VMM wiring, GPA safety.
 * Host-only pre-filter; make test-guest-console is the runtime proof.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <platform/serial_layout.h>

#ifndef AOS_REPO_ROOT
#define AOS_REPO_ROOT "./"
#endif

static int failed;
static int testno;

static void ok(int condition, const char *name)
{
    testno++;
    printf("%s %d - %s\n", condition ? "ok" : "not ok", testno, name);
    if (!condition) {
        failed++;
    }
}

static char *read_file(const char *relative)
{
    char path[1024];
    FILE *file;
    long size;
    char *text;
    size_t read_size;

    snprintf(path, sizeof(path), "%s%s", AOS_REPO_ROOT, relative);
    file = fopen(path, "rb");
    if (!file || fseek(file, 0, SEEK_END) != 0) {
        if (file) fclose(file);
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
    int found = text && strstr(text, needle);
    free(text);
    return found;
}

static int contains_after(const char *relative, const char *first,
                          const char *second)
{
    char *text = read_file(relative);
    char *start = text ? strstr(text, first) : NULL;
    int found = start && strstr(start, second);
    free(text);
    return found;
}

int main(void)
{
    const char *overlay =
        "kernel/agentos-root-task/ubuntu-iso-overlay.dts.in";
    const char *vmm = "kernel/agentos-root-task/src/linux_vmm.c";
    const char *console = "libvmm/src/virtio/console.c";

    printf("TAP version 14\n");
    printf("# suite: virtio_console_guest_path (host pre-filter)\n");

    ok(AOS_VIRTIO_CONSOLE_GUEST_IPA == 0x0A030000UL &&
       AOS_VIRTIO_CONSOLE_MMIO_SIZE == 0x1000UL &&
       AOS_VIRTIO_CONSOLE_VIRQ == 53u &&
       AOS_VIRTIO_CONSOLE_DTB_SPI == 21u,
       "console ABI is IPA 0xa030000, SPI 21, INTID 53");
    ok(contains(overlay, "virtio_mmio@a030000") &&
       contains(overlay, "interrupts = <0x00 0x15 0x01>"),
       "Ubuntu DTB advertises agentOS virtio-console");
    ok(contains(overlay, "virtio_mmio@a010000") &&
       contains(overlay, "virtio_mmio@a020000") &&
       !contains(overlay, "virtio_mmio@a000000") &&
       !contains(overlay, "virtio_mmio@a000200") &&
       !contains(overlay, "virtio_mmio@a000600"),
       "Ubuntu DTB advertises agentOS net/blk and no QEMU passthrough");
    ok(contains("kernel/agentos-root-task/vmm.mk", "console=hvc0"),
       "Ubuntu primary console is hvc0");
    ok(contains(overlay, "pl011@9000000") &&
       contains(overlay, "status = \"disabled\"") &&
       !contains(overlay, "stdout-path"),
       "PL011 is earlycon-only and not an advertised Ubuntu console");
    ok(contains_after(vmm, "aos_vmm_virtio_console_init()",
                     "aos_vmm_virtio_console_after_fault()"),
       "linux_vmm initializes and services virtio-console");
    ok(contains(vmm, "aos_vmm_virtio_console_drain_tx") &&
       contains(vmm, "aos_vmm_virtio_console_push_rx"),
       "guest console contract bridges both TX and RX");
    ok(contains("kernel/agentos-root-task/include/contracts/cc_contract.h",
                "#define CC_INPUT_TEXT       0x05u") &&
       contains("kernel/agentos-root-task/src/cc_pd.c",
                "cc_forward_vibe_input(req->mr[0], event, text, text_len)") &&
       contains("services/vibe-engine/vibe_engine.c",
                "uint32_t input_len = req->length - 4u;") &&
       contains("kernel/agentos-root-task/src/vm_manager.c",
                "payload, 4u + input_len") &&
       contains("platform/guest-vmm/runtime.c",
                "event_type == CC_INPUT_TEXT") &&
       contains(vmm, "aos_vmm_virtio_console_push_rx_bytes") &&
       contains("kernel/agentos-root-task/src/freebsd_vmm.c",
                "freebsd_vmm_push_input") &&
       contains("xtask/src/cmd_test.rs",
                "const CC_INPUT_TEXT_CHUNK: usize = 20;"),
       "bounded CC text input is forwarded through both guest VMM paths");
    ok(contains("kernel/agentos-root-task/src/cc_pd.c",
                "One downstream call per host frame keeps CC latency bounded") &&
       contains("kernel/agentos-root-task/src/cc_pd.c",
                "Do not monopolize the VMM with a 4 KiB loop"),
       "CC console polls perform one bounded downstream IPC");
    ok(contains("kernel/agentos-root-task/src/cc_pd.c",
                "cc_retry_cache_replay") &&
       contains("kernel/agentos-root-task/src/cc_pd.c",
                "cc_retry_cache_record") &&
       contains("kernel/agentos-root-task/src/cc_pd.c",
                "virtio_serial_recover_tx"),
       "lost CC reply retries replay once after TX queue reset");
    ok(contains("kernel/agentos-root-task/include/contracts/cc_contract.h",
                "CC_VIRTIO_STARTUP_VERSION") &&
       contains("kernel/agentos-root-task/src/main.c",
                "pd_vspace_map_device_frame(vspace, vq_cap,") &&
       contains("kernel/agentos-root-task/src/main.c", "vq_vas[vqp]") &&
       !contains("kernel/agentos-root-task/src/main.c",
                 "pd_vspace_map_device_frame(vspace, vq_cap, vq_pas[vqp])") &&
       contains("kernel/agentos-root-task/src/cc_pd.c",
                "#define TX_BUFFER ((void *)(uintptr_t)CC_VIRTIO_TX_BUFFER_VA)") &&
       contains("kernel/agentos-root-task/src/cc_pd.c",
                "TX_DESC[0].addr  = (uint64_t)g_vq_pa[1]"),
       "CC CPU mappings are distinct from VirtIO DMA physical addresses");
    ok(contains(vmm, "Guest console bytes belong to the per-guest virtual TTY") &&
       contains(vmm, "console_tx_push(byte);"),
       "guest virtual TTY is not synchronously mirrored to physical PL011");
    ok(contains("kernel/agentos-root-task/src/freebsd_vmm.c",
                "Guest PL011 output belongs to this guest's virtual TTY") &&
       !contains_after("kernel/agentos-root-task/src/freebsd_vmm.c",
                       "static void guest_console_write(uint8_t byte)",
                       "serial_log_putc(&g_vmm_log, (char)byte)"),
       "FreeBSD virtual TTY is not synchronously mirrored to physical PL011");
    ok(contains("xtask/src/cmd_test.rs", "TRACE_PD_FREEBSD_VMM") &&
       contains("xtask/src/cmd_test.rs",
                ".call(MSG_CC_LOG_STREAM, guest_handle, pd_id, 0, &[])"),
       "dual guest console drain identifies the FreeBSD VMM stream");
    ok(contains(console, "virtio_copy_from_gpa") &&
       contains(console, "virtio_copy_to_gpa"),
       "virtio-console payloads use bounds-checked GPA translation");
    ok(contains(console, "serial_dequeue(console->rxq, &c) == 0"),
       "empty pre-driver RX queue terminates instead of spinning");
    ok(contains("xtask/src/cmd_test.rs", "if guest_os != \"ubuntu\"") &&
       contains("xtask/src/cmd_test.rs",
                "virtio-blk-device,drive=agentos_hd,bus=virtio-mmio-bus.8") &&
       contains("platform/include/platform/blk_host_layout.h",
                "AGENTOS_HOST_BLK_MMIO_PA         0x0A001000UL"),
       "single Ubuntu host block device is owned by agentOS on isolated bus.8");

    printf("1..%d\n", testno);
    return failed ? 1 : 0;
}
