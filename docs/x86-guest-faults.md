# x86 guest MSR fault delivery

The firmware VMM handles unsupported RDMSR/WRMSR and invalid values for its
implemented MSRs by injecting architectural `#GP(0)`. It preserves the
faulting RIP and guest register values instead of terminating the entire
qualification or returning invented MSR data. Host MSRs remain inaccessible.
This allows Linux's safe MSR probes to use their own exception handlers.

`platform/include/platform/x86_event.h` defines the entry decision. A
synchronous fault takes precedence over a pending external interrupt. The
LAPIC interrupt remains in IRR until a later entry accepts it; faults ignore
IF and do not request an interrupt window. The VM-entry interruption field
uses hardware-exception type, vector 13 and a valid error-code bit in
protected mode. Real-address mode omits the error-code bit. The VMM writes
zero to the entry error-code field and resumes the faulting instruction.
seL4 provides these VMCS operations without kernel changes.

The implementation still stops on an exit during IDT delivery, malformed
exit metadata and unsupported non-MSR operations. It does not claim general
exception reinjection, double-fault recovery, NMI handling or all CPU MSRs.
The event planner's real-address encoding is host-tested; actual handler
delivery is qualified in long mode only.

`make test-x86-event-host` checks exact entry bits, instruction advancement,
IRQ priority, LAPIC pending-state preservation, invalid input and unchanged
output on rejection. It is part of `make test-host`.

`make gate-x86_64-guest-faults SEL4_SDK_VERSION=2.3.0` runs a separate
private-page assembly guest on an Intel nested-KVM host. The guest executes
RDMSR and WRMSR for the unavailable index `0xffffffff`. Its own IDT handler
checks zero error codes and the exact faulting RIP, advances each saved RIP,
increments a guest-owned counter and returns with IRETQ. The guest checks
register canaries and both handler completions before its success HLT.
The VMM additionally requires the expected read/write exit sequence, and
root requires a distinct status and success RIP. The harness does not accept
the ordinary HLT smoke marker for this test.

This target proof passed on madmax at `866b6a2`. Linux continuation with the
same runtime fault handling passed the earlier MSR `0x3a` probe and stopped
at a byte write to PCI configuration port `0xcfb`. The
[receipt](evidence/2026-09-17-spark/x86-guest-faults.json) records the exact
images, logs and Spark regression gate. Neither result proves Linux userspace
or completes v0.4.

References: upstream [seL4 VCPU implementation](https://github.com/seL4/seL4/blob/master/src/arch/x86/object/vcpu.c),
Linux [safe feature-control probe](https://github.com/torvalds/linux/blob/v7.0/arch/x86/kernel/cpu/feat_ctl.c),
and [PCI mechanism discovery](https://github.com/torvalds/linux/blob/v7.0/arch/x86/pci/direct.c).
