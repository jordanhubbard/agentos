/*
 * Fill and format an inspect snapshot. No seL4. No libc I/O.
 */

#include <platform/inspect.h>

#include <stddef.h>
#include <string.h>

static void copy_name(uint8_t dst[AOS_INSPECT_NAME_LEN], const uint8_t src[AOS_INSPECT_NAME_LEN])
{
    uint32_t i;
    uint32_t saw_nul = 0u;

    for (i = 0u; i < AOS_INSPECT_NAME_LEN; i++) {
        if (saw_nul) {
            dst[i] = 0u;
        } else {
            dst[i] = src[i];
            if (src[i] == 0u) {
                saw_nul = 1u;
            }
        }
    }
    dst[AOS_INSPECT_NAME_LEN - 1u] = 0u;
}

static int names_equal(const uint8_t a[AOS_INSPECT_NAME_LEN], const char *b)
{
    uint32_t i;

    if (b == NULL) {
        return 0;
    }
    for (i = 0u; i < AOS_INSPECT_NAME_LEN; i++) {
        uint8_t ca = a[i];
        uint8_t cb = (uint8_t)b[i];

        if (ca != cb) {
            return 0;
        }
        if (ca == 0u) {
            return 1;
        }
    }
    return 0;
}

int aos_inspect_fill(aos_inspect_snapshot_t *snap, const aos_inspect_view_t *view)
{
    uint32_t i;

    if (snap == NULL || view == NULL) {
        return AOS_INSPECT_ERR_NULL;
    }
    if (view->thread_count > AOS_INSPECT_MAX_THREADS) {
        return AOS_INSPECT_ERR_TOO_MANY;
    }

    memset(snap, 0, sizeof(*snap));
    snap->version = AOS_INSPECT_VERSION;
    snap->flags = 0u;
    snap->mem.ut_total_bytes = view->ut_total_bytes;
    snap->mem.ut_used_bytes = view->ut_used_bytes;
    snap->mem.guest_ram_bytes = view->guest_ram_bytes;
    snap->mem.pd_count = view->thread_count;
    snap->hw.arch = view->arch;
    snap->hw.virtio_net_virq = view->virtio_net_virq;
    snap->hw.uart_pa = view->uart_pa;
    snap->hw.gic_dist_pa = view->gic_dist_pa;
    snap->hw.virtio_net_ipa = view->virtio_net_ipa;
    snap->thread_count = view->thread_count;

    for (i = 0u; i < view->thread_count; i++) {
        snap->threads[i].pd_index = view->threads[i].pd_index;
        snap->threads[i].prio = view->threads[i].prio;
        snap->threads[i].state = view->threads[i].state;
        copy_name(snap->threads[i].name, view->threads[i].name);
    }

    if (view->arch == AOS_INSPECT_ARCH_UNKNOWN || view->ut_total_bytes == 0u) {
        snap->flags |= AOS_INSPECT_FLAG_PARTIAL;
    }

    return AOS_INSPECT_OK;
}

int aos_inspect_thread_by_name(const aos_inspect_snapshot_t *snap,
                               const char *name,
                               const aos_inspect_thread_t **out)
{
    uint32_t i;

    if (snap == NULL || name == NULL || out == NULL) {
        return AOS_INSPECT_ERR_NULL;
    }
    if (snap->version != AOS_INSPECT_VERSION) {
        return AOS_INSPECT_ERR_VERSION;
    }
    for (i = 0u; i < snap->thread_count && i < AOS_INSPECT_MAX_THREADS; i++) {
        if (names_equal(snap->threads[i].name, name)) {
            *out = &snap->threads[i];
            return AOS_INSPECT_OK;
        }
    }
    return AOS_INSPECT_ERR_NOT_FOUND;
}

static int putc_raw(char **p, char *end, char c)
{
    if (*p >= end) {
        return -1;
    }
    **p = c;
    (*p)++;
    return 0;
}

static int puts_raw(char **p, char *end, const char *s)
{
    while (*s != '\0') {
        if (putc_raw(p, end, *s) != 0) {
            return -1;
        }
        s++;
    }
    return 0;
}

static int put_u64(char **p, char *end, uint64_t v)
{
    char tmp[20];
    uint32_t n = 0u;
    uint32_t i;

    if (v == 0u) {
        return putc_raw(p, end, '0');
    }
    while (v > 0u && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    for (i = n; i > 0u; i--) {
        if (putc_raw(p, end, tmp[i - 1u]) != 0) {
            return -1;
        }
    }
    return 0;
}

static int put_hex64(char **p, char *end, uint64_t v)
{
    static const char digits[] = "0123456789abcdef";
    char tmp[16];
    uint32_t n = 0u;
    uint32_t i;

    if (puts_raw(p, end, "0x") != 0) {
        return -1;
    }
    if (v == 0u) {
        return putc_raw(p, end, '0');
    }
    while (v > 0u && n < sizeof(tmp)) {
        tmp[n++] = digits[v & 0xfu];
        v >>= 4u;
    }
    for (i = n; i > 0u; i--) {
        if (putc_raw(p, end, tmp[i - 1u]) != 0) {
            return -1;
        }
    }
    return 0;
}

static const char *arch_name(uint32_t arch)
{
    switch (arch) {
    case AOS_INSPECT_ARCH_AARCH64:
        return "aarch64";
    case AOS_INSPECT_ARCH_X86_64:
        return "x86_64";
    case AOS_INSPECT_ARCH_RISCV64:
        return "riscv64";
    default:
        return "unknown";
    }
}

static const char *thr_state_name(uint32_t state)
{
    switch (state) {
    case AOS_INSPECT_THR_RUNNING:
        return "running";
    case AOS_INSPECT_THR_BLOCKED:
        return "blocked";
    case AOS_INSPECT_THR_IDLE:
        return "idle";
    default:
        return "unknown";
    }
}

static int line_u64(char **p, char *end, const char *key, uint64_t val)
{
    if (puts_raw(p, end, key) != 0 || putc_raw(p, end, '=') != 0
        || put_u64(p, end, val) != 0 || putc_raw(p, end, '\n') != 0) {
        return -1;
    }
    return 0;
}

static int line_hex(char **p, char *end, const char *key, uint64_t val)
{
    if (puts_raw(p, end, key) != 0 || putc_raw(p, end, '=') != 0
        || put_hex64(p, end, val) != 0 || putc_raw(p, end, '\n') != 0) {
        return -1;
    }
    return 0;
}

static int line_str(char **p, char *end, const char *key, const char *val)
{
    if (puts_raw(p, end, key) != 0 || putc_raw(p, end, '=') != 0
        || puts_raw(p, end, val) != 0 || putc_raw(p, end, '\n') != 0) {
        return -1;
    }
    return 0;
}

static int thread_prefix(char *key, size_t keylen, uint32_t idx, const char *field)
{
    char *kp = key;
    char *kend = key + keylen - 1u;

    if (puts_raw(&kp, kend, "thread[") != 0 || put_u64(&kp, kend, idx) != 0
        || puts_raw(&kp, kend, "].") != 0 || puts_raw(&kp, kend, field) != 0) {
        return -1;
    }
    *kp = '\0';
    return 0;
}

int aos_inspect_format(const aos_inspect_snapshot_t *snap, char *buf, size_t buflen)
{
    char *p;
    char *end;
    uint32_t i;

    if (snap == NULL || buf == NULL) {
        return AOS_INSPECT_ERR_NULL;
    }
    if (buflen < 1u) {
        return AOS_INSPECT_ERR_TRUNC;
    }
    if (snap->version != AOS_INSPECT_VERSION) {
        buf[0] = '\0';
        return AOS_INSPECT_ERR_VERSION;
    }

    p = buf;
    end = buf + buflen - 1u;

    if (line_u64(&p, end, "inspect.version", snap->version) != 0
        || line_u64(&p, end, "inspect.flags", snap->flags) != 0
        || line_u64(&p, end, "memory.ut_total_bytes", snap->mem.ut_total_bytes) != 0
        || line_u64(&p, end, "memory.ut_used_bytes", snap->mem.ut_used_bytes) != 0
        || line_u64(&p, end, "memory.guest_ram_bytes", snap->mem.guest_ram_bytes) != 0
        || line_u64(&p, end, "memory.pd_count", snap->mem.pd_count) != 0
        || line_str(&p, end, "hardware.arch", arch_name(snap->hw.arch)) != 0
        || line_hex(&p, end, "hardware.uart_pa", snap->hw.uart_pa) != 0
        || line_hex(&p, end, "hardware.gic_dist_pa", snap->hw.gic_dist_pa) != 0
        || line_hex(&p, end, "hardware.virtio_net_ipa", snap->hw.virtio_net_ipa) != 0
        || line_u64(&p, end, "hardware.virtio_net_virq", snap->hw.virtio_net_virq) != 0
        || line_u64(&p, end, "thread.count", snap->thread_count) != 0) {
        *p = '\0';
        return AOS_INSPECT_ERR_TRUNC;
    }

    for (i = 0u; i < snap->thread_count && i < AOS_INSPECT_MAX_THREADS; i++) {
        char key[48];
        const uint8_t *nm = snap->threads[i].name;
        uint32_t n;

        if (thread_prefix(key, sizeof(key), i, "name") != 0
            || puts_raw(&p, end, key) != 0 || putc_raw(&p, end, '=') != 0) {
            *p = '\0';
            return AOS_INSPECT_ERR_TRUNC;
        }
        for (n = 0u; n < AOS_INSPECT_NAME_LEN && nm[n] != 0u; n++) {
            if (putc_raw(&p, end, (char)nm[n]) != 0) {
                *p = '\0';
                return AOS_INSPECT_ERR_TRUNC;
            }
        }
        if (putc_raw(&p, end, '\n') != 0) {
            *p = '\0';
            return AOS_INSPECT_ERR_TRUNC;
        }

        if (thread_prefix(key, sizeof(key), i, "pd_index") != 0
            || line_u64(&p, end, key, snap->threads[i].pd_index) != 0
            || thread_prefix(key, sizeof(key), i, "prio") != 0
            || line_u64(&p, end, key, snap->threads[i].prio) != 0
            || thread_prefix(key, sizeof(key), i, "state") != 0
            || line_str(&p, end, key, thr_state_name(snap->threads[i].state)) != 0) {
            *p = '\0';
            return AOS_INSPECT_ERR_TRUNC;
        }
    }

    *p = '\0';
    return (int)(p - buf);
}
