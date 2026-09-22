/* Root-installed execution capabilities in each VMM's private CSpace. */
#ifndef AGENTOS_GUEST_EXECUTION_CAPS_H
#define AGENTOS_GUEST_EXECUTION_CAPS_H

/* Retain libvmm's TCB/VCPU registration offsets. Each range reserves 64
 * vCPU entries below the VMM's guest RAM capability ranges. Only vCPU zero is
 * installed on ARM; x86 firmware reserves VCPUs zero and one in separate
 * native runners. SC authority belongs only to the owning VMM. */
#define AOS_GUEST_TCB_CAP_BASE 266u
#define AOS_GUEST_VCPU_CAP_BASE 330u
#define AOS_GUEST_SC_CAP_BASE 394u

/* ARM owns one dedicated non-device child untyped containing guest TCB,
 * VCPU, IPC frame and (MCS) scheduling context. Root moves its sole pool
 * capability after configuration. Revocation removes every descendant,
 * including root's original object caps, without affecting the VMM itself.
 * Stop guest execution and quiesce all device references before revoking.
 * These slots follow the three guest-RAM CNode/VSpace grants. */
#define AOS_GUEST_EXECUTION_POOL_CAP 461u
#define AOS_GUEST_EXECUTION_POOL_BITS 16u
#define AOS_GUEST_IPC_FRAME_CAP 462u
#define AOS_GUEST_IPC_BUFFER_VA 0x10002000UL

#endif
