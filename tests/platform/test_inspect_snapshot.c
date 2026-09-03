/*
 * Host test for inspect snapshot fill + structured report. No seL4.
 *
 * gcc -I platform/include tests/platform/test_inspect_snapshot.c \
 *     platform/inspect/inspect_snapshot.c -o test_inspect_snapshot
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <platform/inspect.h>
#include <platform/net_layout.h>

#define PASS(name) do { printf("  PASS  %s\n", name); return 0; } while (0)
#define FAIL(msg)  do { printf("  FAIL  %s:%d: %s\n", __FILE__, __LINE__, msg); return 1; } while (0)
#define CHECK(cond) do { if (!(cond)) FAIL(#cond); } while (0)

static void set_name(aos_inspect_thread_t *t, const char *s)
{
    size_t n = strlen(s);
    memset(t->name, 0, AOS_INSPECT_NAME_LEN);
    if (n >= AOS_INSPECT_NAME_LEN) {
        n = AOS_INSPECT_NAME_LEN - 1u;
    }
    memcpy(t->name, s, n);
}

static void sample_view(aos_inspect_view_t *v)
{
    memset(v, 0, sizeof(*v));
    v->ut_total_bytes = 1024ull * 1024ull * 1024ull;
    v->ut_used_bytes = 64ull * 1024ull * 1024ull;
    v->guest_ram_bytes = 512ull * 1024ull * 1024ull;
    v->arch = AOS_INSPECT_ARCH_AARCH64;
    v->virtio_net_virq = AOS_VIRTIO_NET_VIRQ;
    v->uart_pa = 0x09000000ull;
    v->gic_dist_pa = 0x08000000ull;
    v->virtio_net_ipa = AOS_VIRTIO_NET_GUEST_IPA;
    v->thread_count = 3u;
    v->threads[0].pd_index = 0u;
    v->threads[0].prio = 245u;
    v->threads[0].state = AOS_INSPECT_THR_RUNNING;
    set_name(&v->threads[0], "nameserver");
    v->threads[1].pd_index = 1u;
    v->threads[1].prio = 250u;
    v->threads[1].state = AOS_INSPECT_THR_BLOCKED;
    set_name(&v->threads[1], "linux_vmm");
    v->threads[2].pd_index = 2u;
    v->threads[2].prio = 225u;
    v->threads[2].state = AOS_INSPECT_THR_IDLE;
    set_name(&v->threads[2], "serial_pd");
}

static int test_abi(void)
{
    CHECK(AOS_INSPECT_VERSION == 1u);
    CHECK(AOS_INSPECT_MAX_THREADS == 32u);
    CHECK(sizeof(aos_inspect_memory_t) == 32u);
    CHECK(sizeof(aos_inspect_hardware_t) == 32u);
    CHECK(sizeof(aos_inspect_thread_t) == 44u);
    CHECK(AOS_VIRTIO_NET_GUEST_IPA == 0x0A010000UL);
    PASS("test_abi");
}

static int test_fill_report(void)
{
    aos_inspect_view_t view;
    aos_inspect_snapshot_t snap;
    const aos_inspect_thread_t *thr;
    char buf[2048];
    int n;

    sample_view(&view);
    CHECK(aos_inspect_fill(&snap, &view) == AOS_INSPECT_OK);
    CHECK(snap.version == AOS_INSPECT_VERSION);
    CHECK((snap.flags & AOS_INSPECT_FLAG_PARTIAL) == 0u);
    CHECK(snap.mem.ut_total_bytes == view.ut_total_bytes);
    CHECK(snap.mem.pd_count == 3u);
    CHECK(snap.hw.virtio_net_ipa == AOS_VIRTIO_NET_GUEST_IPA);
    CHECK(snap.thread_count == 3u);

    CHECK(aos_inspect_thread_by_name(&snap, "linux_vmm", &thr) == AOS_INSPECT_OK);
    CHECK(thr->prio == 250u);
    CHECK(thr->state == AOS_INSPECT_THR_BLOCKED);
    CHECK(aos_inspect_thread_by_name(&snap, "missing", &thr) == AOS_INSPECT_ERR_NOT_FOUND);

    n = aos_inspect_format(&snap, buf, sizeof(buf));
    CHECK(n > 0);
    CHECK(strstr(buf, "inspect.version=1\n") != NULL);
    CHECK(strstr(buf, "memory.ut_total_bytes=1073741824\n") != NULL);
    CHECK(strstr(buf, "hardware.arch=aarch64\n") != NULL);
    CHECK(strstr(buf, "hardware.virtio_net_ipa=0xa010000\n") != NULL);
    CHECK(strstr(buf, "thread[1].name=linux_vmm\n") != NULL);
    CHECK(strstr(buf, "thread[1].state=blocked\n") != NULL);
    CHECK(strstr(buf, "thread[2].name=serial_pd\n") != NULL);
    PASS("test_fill_report");
}

static int test_errors(void)
{
    aos_inspect_view_t view;
    aos_inspect_snapshot_t snap;
    char tiny[8];
    char buf[64];

    CHECK(aos_inspect_fill(NULL, NULL) == AOS_INSPECT_ERR_NULL);

    sample_view(&view);
    view.thread_count = AOS_INSPECT_MAX_THREADS + 1u;
    CHECK(aos_inspect_fill(&snap, &view) == AOS_INSPECT_ERR_TOO_MANY);

    sample_view(&view);
    view.arch = AOS_INSPECT_ARCH_UNKNOWN;
    view.ut_total_bytes = 0u;
    CHECK(aos_inspect_fill(&snap, &view) == AOS_INSPECT_OK);
    CHECK((snap.flags & AOS_INSPECT_FLAG_PARTIAL) != 0u);

    sample_view(&view);
    CHECK(aos_inspect_fill(&snap, &view) == AOS_INSPECT_OK);
    CHECK(aos_inspect_format(&snap, tiny, sizeof(tiny)) == AOS_INSPECT_ERR_TRUNC);

    snap.version = 99u;
    CHECK(aos_inspect_format(&snap, buf, sizeof(buf)) == AOS_INSPECT_ERR_VERSION);
    PASS("test_errors");
}

int main(void)
{
    int fails = 0;

    printf("test_inspect_snapshot\n");
    fails += test_abi();
    fails += test_fill_report();
    fails += test_errors();
    return fails == 0 ? 0 : 1;
}
