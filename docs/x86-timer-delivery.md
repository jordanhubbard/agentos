# x86 timer delivery: SDK interface qualification

Status: source/API investigation complete; target implementation and execution
qualification remain outstanding. OVMF still stops at its first unmasked APIC
timer expiry. This document is not evidence of interrupt delivery.

## Pinned upstream interfaces

The [Microkit 2.3.0 manifest](https://github.com/seL4/microkit-manifest/blob/2.3.0/main.xml)
pins seL4 `6e7c3b733d296cfd88d5fbf635c96e447a882374`.
Inspection of that revision's
[vcpu.c](https://github.com/seL4/seL4/blob/6e7c3b733d296cfd88d5fbf635c96e447a882374/src/arch/x86/object/vcpu.c)
establishes the following:

- `decodeWriteVMCS` and `decodeReadVMCS` permit the guest preemption timer
  value (lines 876 and 1012).
- Pin execution controls are writable subject to the kernel's fixed-bit
  filtering (lines 902–904). Read back the enable bit; a successful call
  alone does not establish hardware support.
- Under `CONFIG_X86_64_VTX_64BIT_GUESTS`, `decodeVCPUReadMSR` admits
  `IA32_VMX_MISC_MSR`, and `invokeReadMSR` obtains its hardware value
  (lines 649–702). This gives the VMM a supported capability-mediated route
  to the timer rate; no direct privileged instruction is needed.

The source was retrieved through the GitHub contents API on 2026-09-17.
Its decoded SHA-256 is
`f9bfe97f45b4dfbbc9bf10c548fe55d8c9209773c27a27592b37f0524a696331`.
The installed SDK's x86 VTX invocation enumeration includes `X86VCPUReadMSR`.

## Implementation consequence

Use the VMX preemption timer as the asynchronous wakeup source for this VMM.
Obtain its rate through the existing VCPU cap, verify the enable control,
and calculate bounded countdowns from the admitted TSC frequency. Do not
assume that a raw timer count denotes microseconds. Re-arm on each entry;
advance the emulated APIC from elapsed TSC time when handling the exit.
Unsupported capability, frequency or control must produce a diagnostic failure.

No physical timer frame, I/O port, IRQ capability, root-task runtime policy,
or seL4 kernel modification is required by this approach. Pending/in-service
interrupt state belongs to the private guest APIC. Delivery still needs
priority filtering, EOI handling, IF/STI/MOVSS readiness, interrupt-window
exits, and correct halted-guest resumption. A timer wakeup alone does not
establish any of those properties.

## Required target evidence

The Intel qualification must demonstrate timer exits from a guest loop that
does not otherwise exit, repeated timer operation, and a halted guest. It
must then demonstrate guest handler execution and EOI, blocked delivery
followed by interrupt-window delivery, and continued OVMF execution beyond
the current expiry stop. Retain exact image/revision/log identities and
timer-rate/control diagnostics. Host tests must assert APIC state and
countdown outputs, including priority, masking, duplicate expiry and EOI.
Spark's full OS gate remains necessary for shared-code regressions.

MAC: `task_aeec2ea4e28b4f1aa68657a1fce11218`.
