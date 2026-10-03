/*
 * authority_kindmap.c — seL4 object-type to agentOS authority-kind mapping
 *
 * The capability accounting table (cap_accounting.h) records every
 * capability the root task created with the raw seL4 object-type constant
 * the kernel used to retype it. The authority snapshot (platform/authority.h)
 * is host-testable and therefore cannot reference seL4 headers. This file is
 * the one place that bridges the two: it owns the only switch statement that
 * turns an seL4_ObjectType / seL4_ArchObjectType value into an
 * AOS_AUTHORITY_KIND_* value.
 *
 * VSpace-root and frame object types are architecture-specific (ARM VSpace
 * vs. x86 PML4, ARM small/large pages vs. x86 4K/large pages). Rather than
 * hand-duplicate that split here, this reuses the seL4_ARM_VSpaceObject /
 * seL4_ARM_SmallPageObject / seL4_ARM_LargePageObject aliases that
 * boot_info.h already defines per architecture (AArch64, x86_64, RISC-V) --
 * the same aliases main.c itself uses unconditionally when it records these
 * very capabilities (see the cap_acct_record calls beside pd_cnode/vspace/
 * ipc_frame in main.c). Reusing them means this map can never disagree with
 * what was actually recorded, and it covers RISC-V too, not just the two
 * architectures the task brief named.
 *
 * Object types with no dedicated authority kind -- VCPU objects (ARM and
 * x86), x86 EPT paging-structure objects (guest nested-paging state, not the
 * root task's own VSpace) -- fall through to AOS_AUTHORITY_KIND_OTHER via
 * the default case, same as any object type not listed at all. Nothing is
 * ever silently dropped.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "authority_kindmap.h"

#include <platform/authority.h>

#ifdef AGENTOS_TEST_HOST

/*
 * Host-test build: no seL4 SDK available, and platform/authority.h must stay
 * seL4-free, so the five seL4_ObjectType values this map depends on for its
 * common (non-arch-specific) cases are supplied by the test harness via -D
 * flags computed from the real SDK header (see the test-authority-kindmap-host
 * Makefile target), not hand-copied here. That way the test and this
 * implementation cannot drift apart silently.
 */

#ifndef AOSTEST_SEL4_UNTYPED
#error "AGENTOS_TEST_HOST build of authority_kindmap.c requires AOSTEST_SEL4_* -D flags; see test-authority-kindmap-host in the top-level Makefile"
#endif

uint32_t aos_authority_kind_from_sel4(uint32_t obj_type)
{
    switch (obj_type) {
    case AOSTEST_SEL4_UNTYPED:      return AOS_AUTHORITY_KIND_UNTYPED;
    case AOSTEST_SEL4_TCB:          return AOS_AUTHORITY_KIND_TCB;
    case AOSTEST_SEL4_ENDPOINT:     return AOS_AUTHORITY_KIND_ENDPOINT;
    case AOSTEST_SEL4_NOTIFICATION: return AOS_AUTHORITY_KIND_NOTIFICATION;
    case AOSTEST_SEL4_CNODE:        return AOS_AUTHORITY_KIND_CNODE;
    default:                        return AOS_AUTHORITY_KIND_OTHER;
    }
}

#else /* real seL4 build */

#include "boot_info.h" /* seL4_Word, seL4_UntypedObject, seL4_ARM_VSpaceObject, ... */

uint32_t aos_authority_kind_from_sel4(uint32_t obj_type)
{
    switch ((seL4_Word)obj_type) {
    case seL4_UntypedObject:      return AOS_AUTHORITY_KIND_UNTYPED;
    case seL4_TCBObject:          return AOS_AUTHORITY_KIND_TCB;
    case seL4_EndpointObject:     return AOS_AUTHORITY_KIND_ENDPOINT;
    case seL4_NotificationObject: return AOS_AUTHORITY_KIND_NOTIFICATION;
    case seL4_CapTableObject:     return AOS_AUTHORITY_KIND_CNODE;
#ifdef CONFIG_KERNEL_MCS
    case seL4_SchedContextObject: return AOS_AUTHORITY_KIND_SCHED_CONTEXT;
    case seL4_ReplyObject:        return AOS_AUTHORITY_KIND_REPLY;
#endif
    /* Per-arch aliases from boot_info.h: ARM VSpace / x86 PML4 / RISC-V root
     * page table, and ARM small/large page / x86 4K/large page / RISC-V
     * 4K/mega page. */
    case seL4_ARM_VSpaceObject:    return AOS_AUTHORITY_KIND_VSPACE;
    case seL4_ARM_SmallPageObject: return AOS_AUTHORITY_KIND_FRAME;
    case seL4_ARM_LargePageObject: return AOS_AUTHORITY_KIND_FRAME;
    default:
        /* VCPU objects (seL4_ARM_VCPUObject, seL4_X86_VCPUObject) and x86 EPT
         * paging-structure objects (seL4_X86_EPTPML4Object and friends --
         * guest nested-paging state, not the root task's own VSpace) have no
         * dedicated kind in the ABI; bucket them as OTHER rather than guess. */
        return AOS_AUTHORITY_KIND_OTHER;
    }
}

#endif /* AGENTOS_TEST_HOST */
