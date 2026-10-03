/*
 * agentOS authority snapshot ABI
 *
 * Read-only record of the authority the root task established at boot: for
 * each protection domain, how many capabilities of each kind it was granted.
 * Host-testable: no seL4 headers. Root publishes this as an immutable boot
 * observation mapped read-only into its readers.
 *
 * WHAT THIS IS NOT. seL4 exposes no capability-enumeration syscall --
 * seL4_DebugCapIdentify is CONFIG_DEBUG_BUILD-only and that is disabled in the
 * shipped release kernel -- so this is a LEDGER OF WHAT ROOT GRANTED, not a
 * reader of kernel state. It reports the intended relation and cannot detect a
 * divergence between that record and the kernel. It covers the boot-time static
 * set; runtime delegation must be separately reported by the delegating domain.
 * It does NOT verify the subsetting invariant: that is enforced by the kernel
 * unconditionally, since a domain cannot mint from a capability it does not
 * hold. This supplies visibility, not verification.
 */

#ifndef AOS_PLATFORM_AUTHORITY_H
#define AOS_PLATFORM_AUTHORITY_H

#include <stddef.h>
#include <stdint.h>

#define AOS_AUTHORITY_VERSION     1u
#define AOS_AUTHORITY_MAX_PDS     32u
#define AOS_AUTHORITY_NAME_LEN    32u

/* One 4 KiB page. 0x10009000 is the inspect boot page and 0x1000a000 /
 * 0x1000b000 are the log config and client pages (platform/log_ring.h), so
 * this sits above them in the first otherwise-unused slot. */
#define AOS_AUTHORITY_BOOT_VA     0x1000C000UL

/* agentOS-local capability kinds. seL4 object-type constants are mapped onto
 * these inside the root task, which has the seL4 headers; this header stays
 * host-testable. Order is ABI -- append only, never reorder. */
typedef enum {
    AOS_AUTHORITY_KIND_UNTYPED       = 0,
    AOS_AUTHORITY_KIND_TCB           = 1,
    AOS_AUTHORITY_KIND_ENDPOINT      = 2,
    AOS_AUTHORITY_KIND_NOTIFICATION  = 3,
    AOS_AUTHORITY_KIND_CNODE         = 4,
    AOS_AUTHORITY_KIND_FRAME         = 5,
    AOS_AUTHORITY_KIND_VSPACE        = 6,
    AOS_AUTHORITY_KIND_IRQ_HANDLER   = 7,
    AOS_AUTHORITY_KIND_SCHED_CONTEXT = 8,
    AOS_AUTHORITY_KIND_REPLY         = 9,
    AOS_AUTHORITY_KIND_OTHER         = 10,
} aos_authority_kind_t;

#define AOS_AUTHORITY_KIND_COUNT 11u

#define AOS_AUTHORITY_OK            0
#define AOS_AUTHORITY_ERR_NULL     (-1)
#define AOS_AUTHORITY_ERR_VERSION  (-2)
#define AOS_AUTHORITY_ERR_TRUNC    (-3)
#define AOS_AUTHORITY_ERR_INVALID  (-6)

typedef struct __attribute__((packed)) aos_authority_pd {
    uint32_t pd_index;
    uint8_t  name[AOS_AUTHORITY_NAME_LEN];
    uint16_t counts[AOS_AUTHORITY_KIND_COUNT];
    uint16_t reserved;
} aos_authority_pd_t;

typedef struct __attribute__((packed)) aos_authority_snapshot {
    uint32_t version;
    uint32_t pd_count;        /* rows in use, <= AOS_AUTHORITY_MAX_PDS */
    uint32_t total_recorded;  /* every add, including saturated increments */
    uint32_t truncated_adds;  /* add calls dropped because the domain table
                                * was full; may exceed the number of distinct
                                * domains dropped if one domain made several
                                * calls after the table filled */
    uint32_t saturated;       /* nonzero if any count hit UINT16_MAX */
    uint32_t reserved;
    aos_authority_pd_t pds[AOS_AUTHORITY_MAX_PDS];
} aos_authority_snapshot_t;

void aos_authority_init(aos_authority_snapshot_t *snap);
/*
 * aos_authority_add — record one capability grant of `kind` against
 * `pd_index`, creating that domain's row on first sight.
 *
 * `name` is honoured only the first time a given pd_index is seen (it names
 * the row then); on every later call for the same pd_index, `name` is
 * ignored and only the kind's count is incremented. Callers that record
 * several kinds for one domain should still pass the domain's name every
 * time -- consistently, not just on what the caller believes is the first
 * call -- since callers cannot generally know which recorded capability a
 * walk over an external table will visit first for a given domain.
 */
int  aos_authority_add(aos_authority_snapshot_t *snap, uint32_t pd_index,
                       const char *name, uint32_t kind);
int  aos_authority_validate(const aos_authority_snapshot_t *snap);
int  aos_authority_format(const aos_authority_snapshot_t *snap,
                          char *buf, size_t buflen);

#endif /* AOS_PLATFORM_AUTHORITY_H */
