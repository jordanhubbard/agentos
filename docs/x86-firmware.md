# x86 firmware bring-up

The opt-in reset variant runs a real, externally supplied OVMF image under
the seL4 VMM. It currently resumes the firmware's initial `MOV CR0,EAX`
transition into unpaged protected mode and stops at the next CPUID exit.
This is firmware bring-up, not a completed UEFI boot or Linux qualification.

## Reproduce

Use an Intel Linux host exposing nested VMX through KVM and Microkit 2.3.0:

```sh
make gate-x86_64-firmware-reset SEL4_SDK_VERSION=2.3.0 \
  X86_FIRMWARE_IMAGE=/usr/share/ovmf/OVMF.fd \
  X86_FIRMWARE_SHA256=101afad5f520753224b60b9555df60add435cf50c7a82b781d7d7d8c60db7eb5
```

The qualified image is the full 4 MiB `OVMF.fd` from Ubuntu's
`ovmf 2024.02-2ubuntu0.9` package. Its package copyright identifies EDK II
and the BSD-2-Clause-Patent license. The firmware binary is not committed
here. Both its byte length and caller-supplied SHA-256 must match before
embedding. A different package version needs its own provenance and target
qualification; do not replace the digest just to make a build pass.

Root provisions 32 MiB of zeroed private guest RAM at GPA zero and 4 MiB
of read-only firmware at `0xffc00000`. These are newly allocated frames,
not host MMIO or passthrough devices. Root drops each temporary initialization
mapping before installing its EPT mapping and gives the VMM its VCPU cap.
The firmware variant uses the existing single-VMM qualification topology.
Allocation failure aborts boot; this is not a runtime guest-create path and
does not qualify capability reclamation or retry.

The VMM starts at architectural reset address `0xfffffff0`, using the special
high CS cache and unrestricted real-address entry. It accepts only the initial
unpaged `MOV CR0,EAX` transition, updates CR0 and its read shadow, then resumes
at the following instruction. The firmware executes its own far jump and
segment setup. Success requires a subsequent two-byte CPUID exit inside the
firmware region, with a flat 32-bit code segment. Unsupported exits stop with
diagnostics; they are not skipped or reported as completed firmware boot.

The [upstream EDK II transition](https://github.com/tianocore/edk2/blob/edk2-stable202402/UefiCpuPkg/ResetVector/Vtf0/Ia16/Real16ToFlat32.asm)
provides the source context for this early execution path. The
[qualification receipt](evidence/2026-09-17-spark/ovmf-reset.json) records the
actual image, source revisions and retained evidence.

## Remaining boot implementation

CPUID policy and additional VM-exit handling must continue this execution
into long mode and firmware initialization. UEFI also requires an emulated
machine description, generated ACPI and interrupt topology, boot media over
canonical agentOS services, and actual Linux userspace evidence. The current
read-only firmware mapping does not implement persistent UEFI variables.
The default SDK and ordinary `make gate` remain unchanged.
