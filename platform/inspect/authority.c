/*
 * Build and format an authority snapshot: a ledger of what the root task
 * recorded granting to each protection domain, by capability kind. No seL4.
 * No libc I/O. See platform/include/platform/authority.h for what this is
 * and, importantly, what it is not.
 */

#include <platform/authority.h>

#include <stddef.h>
#include <string.h>

_Static_assert(sizeof(aos_authority_snapshot_t) <= 4096,
               "authority snapshot fits one page");

static void copy_name(uint8_t dst[AOS_AUTHORITY_NAME_LEN], const char *src)
{
    uint32_t i;

    memset(dst, 0, AOS_AUTHORITY_NAME_LEN);
    if (src == NULL) {
        return;
    }
    for (i = 0u; i < AOS_AUTHORITY_NAME_LEN - 1u && src[i] != '\0'; i++) {
        dst[i] = (uint8_t)src[i];
    }
}

static uint32_t clamp_kind(uint32_t kind)
{
    if (kind >= AOS_AUTHORITY_KIND_COUNT) {
        return (uint32_t)AOS_AUTHORITY_KIND_OTHER;
    }
    return kind;
}

void aos_authority_init(aos_authority_snapshot_t *snap)
{
    if (snap == NULL) {
        return;
    }
    memset(snap, 0, sizeof(*snap));
    snap->version = AOS_AUTHORITY_VERSION;
}

int aos_authority_add(aos_authority_snapshot_t *snap, uint32_t pd_index,
                      const char *name, uint32_t kind)
{
    uint32_t i;
    uint32_t k;
    aos_authority_pd_t *row = NULL;

    if (snap == NULL) {
        return AOS_AUTHORITY_ERR_NULL;
    }

    k = clamp_kind(kind);

    for (i = 0u; i < snap->pd_count; i++) {
        if (snap->pds[i].pd_index == pd_index) {
            row = &snap->pds[i];
            break;
        }
    }

    if (row == NULL) {
        if (snap->pd_count < AOS_AUTHORITY_MAX_PDS) {
            row = &snap->pds[snap->pd_count];
            memset(row, 0, sizeof(*row));
            row->pd_index = pd_index;
            copy_name(row->name, name);
            snap->pd_count++;
        } else {
            snap->truncated_pds++;
            snap->total_recorded++;
            return AOS_AUTHORITY_OK;
        }
    }

    if (row->counts[k] == 0xFFFFu) {
        snap->saturated = 1u;
    } else {
        row->counts[k]++;
    }

    snap->total_recorded++;
    return AOS_AUTHORITY_OK;
}

int aos_authority_validate(const aos_authority_snapshot_t *snap)
{
    if (snap == NULL) {
        return AOS_AUTHORITY_ERR_NULL;
    }
    if (snap->version != AOS_AUTHORITY_VERSION) {
        return AOS_AUTHORITY_ERR_VERSION;
    }
    if (snap->pd_count > AOS_AUTHORITY_MAX_PDS) {
        return AOS_AUTHORITY_ERR_INVALID;
    }
    return AOS_AUTHORITY_OK;
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

static int line_u64(char **p, char *end, const char *key, uint64_t val)
{
    if (puts_raw(p, end, key) != 0 || putc_raw(p, end, '=') != 0
        || put_u64(p, end, val) != 0 || putc_raw(p, end, '\n') != 0) {
        return -1;
    }
    return 0;
}

static const char *kind_name(uint32_t kind)
{
    switch (kind) {
    case AOS_AUTHORITY_KIND_UNTYPED:
        return "untyped";
    case AOS_AUTHORITY_KIND_TCB:
        return "tcb";
    case AOS_AUTHORITY_KIND_ENDPOINT:
        return "endpoint";
    case AOS_AUTHORITY_KIND_NOTIFICATION:
        return "notification";
    case AOS_AUTHORITY_KIND_CNODE:
        return "cnode";
    case AOS_AUTHORITY_KIND_FRAME:
        return "frame";
    case AOS_AUTHORITY_KIND_VSPACE:
        return "vspace";
    case AOS_AUTHORITY_KIND_IRQ_HANDLER:
        return "irq_handler";
    case AOS_AUTHORITY_KIND_SCHED_CONTEXT:
        return "sched_context";
    case AOS_AUTHORITY_KIND_REPLY:
        return "reply";
    default:
        return "other";
    }
}

int aos_authority_format(const aos_authority_snapshot_t *snap, char *buf, size_t buflen)
{
    char *p;
    char *end;
    uint32_t i;
    uint32_t k;

    if (snap == NULL || buf == NULL) {
        return AOS_AUTHORITY_ERR_NULL;
    }
    if (buflen < 1u) {
        return AOS_AUTHORITY_ERR_TRUNC;
    }

    int valid = aos_authority_validate(snap);
    if (valid != AOS_AUTHORITY_OK) {
        buf[0] = '\0';
        return valid;
    }

    p = buf;
    end = buf + buflen - 1u;

    for (i = 0u; i < snap->pd_count && i < AOS_AUTHORITY_MAX_PDS; i++) {
        const aos_authority_pd_t *row = &snap->pds[i];
        uint32_t n;

        if (puts_raw(&p, end, "pd=") != 0) {
            *p = '\0';
            return AOS_AUTHORITY_ERR_TRUNC;
        }
        for (n = 0u; n < AOS_AUTHORITY_NAME_LEN && row->name[n] != 0u; n++) {
            if (putc_raw(&p, end, (char)row->name[n]) != 0) {
                *p = '\0';
                return AOS_AUTHORITY_ERR_TRUNC;
            }
        }
        if (puts_raw(&p, end, " index=") != 0 || put_u64(&p, end, row->pd_index) != 0) {
            *p = '\0';
            return AOS_AUTHORITY_ERR_TRUNC;
        }
        for (k = 0u; k < AOS_AUTHORITY_KIND_COUNT; k++) {
            if (putc_raw(&p, end, ' ') != 0
                || puts_raw(&p, end, kind_name(k)) != 0
                || putc_raw(&p, end, '=') != 0
                || put_u64(&p, end, row->counts[k]) != 0) {
                *p = '\0';
                return AOS_AUTHORITY_ERR_TRUNC;
            }
        }
        if (putc_raw(&p, end, '\n') != 0) {
            *p = '\0';
            return AOS_AUTHORITY_ERR_TRUNC;
        }
    }

    if (line_u64(&p, end, "total", snap->total_recorded) != 0) {
        *p = '\0';
        return AOS_AUTHORITY_ERR_TRUNC;
    }
    if (line_u64(&p, end, "truncated_pds", snap->truncated_pds) != 0) {
        *p = '\0';
        return AOS_AUTHORITY_ERR_TRUNC;
    }
    if (line_u64(&p, end, "saturated", snap->saturated) != 0) {
        *p = '\0';
        return AOS_AUTHORITY_ERR_TRUNC;
    }

    *p = '\0';
    return (int)(p - buf);
}
