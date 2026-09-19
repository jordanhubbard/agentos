# x86 architectural context dependency

Linux SMP remains unqualified. The strict gate rejects recurring userspace
faults with both the reused disk and a journal-recovered baseline copy.
Disabling nested VMX VPID does not eliminate the failure.

At `41b3e9b`, native scratch-RAM tests pass for distinct GPR, x87 and SSE
values across two runner switches. Separate guest page-table roots also
retain their mappings, including guest MOV CR3 changes and subsequent peer
execution. Both tests repeat after reconstruction; the full Spark gate passes.
See [the receipt](evidence/2026-09-18-spark/x86-native-context.json).
These bounded tests do not establish complete architectural context isolation.

## Source-level concern

Microkit 2.3.0 pins seL4 revision
`6e7c3b733d296cfd88d5fbf635c96e447a882374`. Inspection of its
[VCPU storage](https://github.com/seL4/seL4/blob/6e7c3b733d296cfd88d5fbf635c96e447a882374/include/arch/x86/arch/object/vcpu.h),
[entry path](https://github.com/seL4/seL4/blob/6e7c3b733d296cfd88d5fbf635c96e447a882374/src/arch/x86/64/c_traps.c),
[exit path](https://github.com/seL4/seL4/blob/6e7c3b733d296cfd88d5fbf635c96e447a882374/src/arch/x86/64/traps.S)
and VCPU invocation implementation found no per-VCPU CR2 storage or
save/restore path, nor a userspace invocation for managing that register.
CR2 contains the guest page-fault address. This is an architectural-context
dependency and a plausible explanation for the observed Linux faults;
causation has not been established by a corrected-kernel comparison.

## Proposed change boundary

The constitution says: "seL4 is the only kernel-mode code. Never modify it."
No kernel source or SDK binary has been changed in this investigation.
An explicit project exception is required before pursuing a kernel change.

The proposed exception is limited to upstream-compatible preservation of
CR2 as native VCPU architectural state, including initialization and separate
host state where required. It does not extend agentOS device authority or
replace seL4 with a project fork. An approved change would require a pinned,
reproducible SDK build and renewed native, single-CPU, two-CPU, lifecycle and
full Spark qualification. No release may infer Linux SMP success from the
short native tests or from this source audit. Upstream publication requires
separate authorization.
