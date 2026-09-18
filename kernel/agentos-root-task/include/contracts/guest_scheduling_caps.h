#ifndef AGENTOS_GUEST_SCHEDULING_CAPS_H
#define AGENTOS_GUEST_SCHEDULING_CAPS_H

/* Each ARM VMM shares one isolated capability exchange CNode with vm_manager.
 * Slots contain only that guest's TCB, scheduling context and VMM fault EP.
 * The VMM never receives SchedControl or manager/root TCB authority. Root
 * populates boot objects; reconstruction must publish replacements before
 * replying to CREATE. Manager configures them between CREATE and BOOT,
 * never through a synchronous VMM callback into the waiting manager. */
#define AOS_GUEST_SCHED_EXCHANGE_CAP 494u
#define AOS_GUEST_SCHED_EXCHANGE_BITS 2u
#define AOS_GUEST_SCHED_TCB 0u
#define AOS_GUEST_SCHED_SC 1u
#define AOS_GUEST_SCHED_FAULT_EP 2u
#define AOS_GUEST_SCHED_OBJECTS 3u
#define AOS_GUEST_SCHED_CLIENTS 2u

/* Manager-only slots. The inert authority TCB has MCP=150 and never runs.
 * SchedControl caps preserve each VMM's root-selected CPU placement. */
#define AOS_GUEST_SCHED_MANAGER_CNODE 600u
#define AOS_GUEST_SCHED_AUTHORITY 601u
#define AOS_GUEST_SCHED_EXCHANGE_BASE 602u
#define AOS_GUEST_SCHED_CONTROL_BASE 604u
#define AOS_GUEST_SCHED_SCRATCH_BASE 606u
#define AOS_GUEST_SCHED_MANAGER_BITS 10u
#define AOS_GUEST_SCHED_PRIORITY 150u
#define AOS_GUEST_SCHED_BUDGET_US 25000u
#define AOS_GUEST_SCHED_PERIOD_US 100000u

#endif
