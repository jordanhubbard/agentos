/* Root-installed execution capabilities in each VMM's private CSpace. */
#ifndef AGENTOS_GUEST_EXECUTION_CAPS_H
#define AGENTOS_GUEST_EXECUTION_CAPS_H

/* Retain libvmm's TCB/VCPU registration offsets. Each range reserves 64
 * vCPU entries and fits in the VMM's 1024-slot CNode. Only vCPU zero is
 * installed today. SC authority belongs only to the owning VMM. */
#define AOS_GUEST_TCB_CAP_BASE 266u
#define AOS_GUEST_VCPU_CAP_BASE 330u
#define AOS_GUEST_SC_CAP_BASE 394u

#endif
