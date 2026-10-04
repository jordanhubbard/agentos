/*
 * agentOS inspect snapshot ABI
 *
 * Read-only view of memory, threads (protection domains), and hardware
 * attributes. Host-testable: no seL4 headers. Root publishes an immutable
 * boot observation mapped read-only into CC, which returns this packed ABI.
 *
 * This is not a UI. The formatter emits structured key=value lines for a
 * client (guest Hermes, or any serial_virt consumer).
 */

#ifndef AOS_PLATFORM_INSPECT_H
#define AOS_PLATFORM_INSPECT_H

#include <stddef.h>
#include <stdint.h>

/*
 * AOS_INSPECT_VERSION — bumped 1 -> 2 by T10.
 *
 * The LAYOUT did not change (the snapshot is still 1488 bytes) but the
 * MEANING of a field did: the always-zero `reserved` word became
 * `anchor_tier`, and aos_inspect_validate()'s check on it changed from
 * "must be zero" to "must be a tier or the unverified sentinel". A
 * consumer built before that change, pointed at a system running a VENDOR
 * or MOK image, would read 1 or 2 in a word it believes is reserved and
 * reject the whole snapshot as malformed -- reporting a broken system
 * rather than a tier. The version field is the only signal such a
 * mismatched pair has, so it has to move even though sizeof() did not.
 *
 * In-tree this cannot happen: agentctl is built from the same tree, and
 * tools/agentctl/Makefile depends on the trust anchor contract header so
 * it rebuilds when the tier model changes. The bump is for anything built
 * outside this tree against an older copy of this ABI.
 */
#define AOS_INSPECT_VERSION            2u
#define AOS_INSPECT_MAX_THREADS        32u
#define AOS_INSPECT_NAME_LEN           32u

#define AOS_INSPECT_ARCH_UNKNOWN       0u
#define AOS_INSPECT_ARCH_AARCH64       1u
#define AOS_INSPECT_ARCH_X86_64        2u
#define AOS_INSPECT_ARCH_RISCV64       3u

#define AOS_INSPECT_THR_UNKNOWN        0u
#define AOS_INSPECT_THR_RUNNING        1u
#define AOS_INSPECT_THR_BLOCKED        2u
#define AOS_INSPECT_THR_IDLE           3u

#define AOS_INSPECT_FLAG_PARTIAL       1u
#define AOS_INSPECT_FLAG_BOOT          2u /* immutable root observation */
#define AOS_INSPECT_FLAG_USED_LOWER_BOUND 4u /* accounted pages, not free RAM */
#define AOS_INSPECT_FLAG_KNOWN (AOS_INSPECT_FLAG_PARTIAL | AOS_INSPECT_FLAG_BOOT | AOS_INSPECT_FLAG_USED_LOWER_BOUND)
#define AOS_INSPECT_BOOT_VA 0x10009000UL /* CC only, read-only, one 4 KiB page */

#define AOS_INSPECT_OK                 0
#define AOS_INSPECT_ERR_NULL          (-1)
#define AOS_INSPECT_ERR_VERSION       (-2)
#define AOS_INSPECT_ERR_TRUNC         (-3)
#define AOS_INSPECT_ERR_TOO_MANY      (-4)
#define AOS_INSPECT_ERR_NOT_FOUND     (-5)
#define AOS_INSPECT_ERR_INVALID       (-6)

typedef struct __attribute__((packed)) aos_inspect_memory {
    uint64_t ut_total_bytes;
    uint64_t ut_used_bytes;
    uint64_t guest_ram_bytes;
    uint32_t pd_count;
    uint32_t reserved;
} aos_inspect_memory_t;

typedef struct __attribute__((packed)) aos_inspect_hardware {
    uint32_t arch;
    uint32_t virtio_net_virq;
    uint64_t uart_pa;
    uint64_t gic_dist_pa;
    uint64_t virtio_net_ipa;
} aos_inspect_hardware_t;

typedef struct __attribute__((packed)) aos_inspect_thread {
    uint32_t pd_index;
    uint32_t prio;
    uint32_t state;
    uint8_t  name[AOS_INSPECT_NAME_LEN];
} aos_inspect_thread_t;

typedef struct __attribute__((packed)) aos_inspect_snapshot {
    uint32_t version;
    uint32_t flags;
    aos_inspect_memory_t mem;
    aos_inspect_hardware_t hw;
    uint32_t thread_count;
    /*
     * anchor_tier -- the trust anchor tier (aos_anchor_tier_t: 0=NONE,
     * 1=VENDOR, 2=MOK, 3=HARDWARE) THIS image booted under, or
     * AOS_ANCHOR_UNVERIFIED (0xFFFFFFFF) on an architecture that embeds no
     * PD bundle and therefore runs no tier at all -- RISC-V today. That
     * sentinel is deliberately NOT a tier: reporting a real tier on a box
     * that verifies nothing would be a lie in the one field an operator
     * reads to find out what their machine enforces. Was `reserved`
     * (always zero); repurposed by T10 so an operator can ask a LIVE
     * machine which anchor it booted under, not just read a boot log that
     * has long since scrolled away. This struct's own layout stays
     * dependency-free (plain uint32_t, no enum type), but
     * platform/inspect/inspect_snapshot.c's formatter calls
     * aos_anchor_tier_name() (kernel/agentos-root-task/include/contracts/
     * trust_anchor.h) as the single source for the tier's display name --
     * see that header for the tier semantics. Do not reintroduce a second
     * copy of that name table; a prior revision did, hand-synced, with no
     * test catching disagreement between the boot log and this field's
     * formatted output.
     */
    uint32_t anchor_tier;
    aos_inspect_thread_t threads[AOS_INSPECT_MAX_THREADS];
} aos_inspect_snapshot_t;

/*
 * Observation injected by the root task (or a host test). Not a live scheduler
 * query. BOOT reports successfully started PDs, unknown current thread states,
 * reserved guest RAM, and hardware configuration. USED_LOWER_BOUND identifies
 * accounted pages from the managed non-device pool: sub-page kernel objects
 * and alignment loss are not accounted, so subtraction does not give free RAM.
 */
typedef struct aos_inspect_view {
    uint32_t flags;
    uint64_t ut_total_bytes;
    uint64_t ut_used_bytes;
    uint64_t guest_ram_bytes;
    uint32_t arch;
    uint32_t virtio_net_virq;
    uint64_t uart_pa;
    uint64_t gic_dist_pa;
    uint64_t virtio_net_ipa;
    uint32_t thread_count;
    /* See aos_inspect_snapshot_t.anchor_tier above. CAUTION for the next
     * producer: 0 == AOS_ANCHOR_NONE, the PERMISSIVE tier, is also the
     * zero-initialised default of this field. A producer that forgets to
     * set it reports "none (development, not gating)" rather than
     * anything obviously wrong -- fail-safe (understates trust, never
     * overstates it) but silent. Always set this explicitly. */
    uint32_t anchor_tier;
    aos_inspect_thread_t threads[AOS_INSPECT_MAX_THREADS];
} aos_inspect_view_t;

int aos_inspect_fill(aos_inspect_snapshot_t *snap, const aos_inspect_view_t *view);
int aos_inspect_validate(const aos_inspect_snapshot_t *snap);
int aos_inspect_format(const aos_inspect_snapshot_t *snap, char *buf, size_t buflen);
int aos_inspect_thread_by_name(const aos_inspect_snapshot_t *snap,
                               const char *name,
                               const aos_inspect_thread_t **out);

#endif /* AOS_PLATFORM_INSPECT_H */
