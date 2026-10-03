/*
 * authority_kindmap.h — seL4 object-type to agentOS authority-kind mapping
 *
 * The root task is the only place with both the seL4 headers (object-type
 * constants) and the capability accounting table (platform/authority.h stays
 * host-testable and seL4-free). This declares the one function that bridges
 * them.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef AGENTOS_AUTHORITY_KINDMAP_H
#define AGENTOS_AUTHORITY_KINDMAP_H

#include <stdint.h>

/*
 * aos_authority_kind_from_sel4 — map a raw seL4 object-type constant
 * (seL4_ObjectType / seL4_ArchObjectType, as recorded in a
 * cap_acct_entry_t.obj_type) onto an AOS_AUTHORITY_KIND_* value.
 *
 * Object types the capability accounting table can hold but that have no
 * dedicated authority kind -- VCPU objects, x86 EPT paging-structure objects,
 * and anything not explicitly handled -- map to AOS_AUTHORITY_KIND_OTHER.
 * Nothing is ever dropped.
 */
uint32_t aos_authority_kind_from_sel4(uint32_t obj_type);

#endif /* AGENTOS_AUTHORITY_KINDMAP_H */
