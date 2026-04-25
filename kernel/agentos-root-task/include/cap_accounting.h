/*
 * cap_accounting.h — capability ownership tracking for the root task
 *
 * Records every kernel object capability created during boot in a static
 * ownership table.  Each entry records the CPtr, object type, owning PD
 * index, and a short name for diagnostic output.
 *
 * This table is used by the monitor PD and fault handler to reason about
 * which capabilities belong to which protection domain, and to perform
 * revocation during PD teardown.
 *
 * No dynamic allocation: the table is a fixed-size static array.
 *
 * Copyright (c) 2026 The agentOS Project
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "boot_info.h"   /* seL4_BootInfo, seL4_CPtr, seL4_Word */
#include <stdint.h>

/* Maximum number of capability records that can be tracked. */
#define CAP_ACCT_MAX_ENTRIES  1024u

/*
 * cap_acct_entry_t — one capability record in the accounting table.
 *
 * Fields:
 *   cap       seL4 capability pointer
 *   obj_type  seL4 object type constant (e.g. seL4_TCBObject)
 *   pd_index  index of the owning PD in the system descriptor (0 = root task)
 *   name      short human-readable name (truncated to 15 chars + NUL)
 */
typedef struct {
    seL4_CPtr  cap;
    uint32_t   obj_type;
    uint32_t   pd_index;
    char       name[16];
} cap_acct_entry_t;

/*
 * cap_acct_init — initialise the capability accounting subsystem.
 *
 * Must be called once during boot, before any calls to cap_acct_record.
 * Clears the static table and records the root task's initial capabilities
 * from the BootInfo.
 *
 * Parameters:
 *   bi   pointer to the seL4 BootInfo structure
 */
void cap_acct_init(const seL4_BootInfo *bi);

/*
 * cap_acct_record — add one capability to the accounting table.
 *
 * Parameters:
 *   parent    parent capability (seL4_CapNull if directly from untyped)
 *   cap       the capability to record
 *   obj_type  seL4 object type of the capability
 *   pd_index  owning PD index (matches system_desc_t.pds[] index)
 *   name      short name (NUL-terminated; truncated after 15 chars)
 *
 * Returns 0 on success, -1 if the table is full.
 */
int cap_acct_record(seL4_CPtr   parent,
                    seL4_CPtr   cap,
                    uint32_t    obj_type,
                    uint32_t    pd_index,
                    const char *name);

/*
 * cap_acct_count — return the number of entries currently recorded.
 */
uint32_t cap_acct_count(void);

/*
 * cap_acct_get — retrieve one entry by index (0-based).
 *
 * Returns NULL if index is out of range.
 */
const cap_acct_entry_t *cap_acct_get(uint32_t index);
