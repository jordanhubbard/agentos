# x86 firmware bring-up

The opt-in reset variant runs a real, externally supplied OVMF image under
the seL4 VMM. It handles bootstrap CPU exits through the firmware's own
protected- and long-mode transitions, then stops at the first long-mode I/O exit.
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
high CS cache and unrestricted real-address entry. It explicitly enables
VM-entry EFER loading so all guest EFER bits use the supplied state. A bounded
loop preserves all guest general registers across emulation and implements
CPUID, EFER reads/writes, and MOV-to-CR0/CR4 transitions. Paging activation
updates EFER.LMA and the IA-32e entry control together. Other exits stop with
diagnostics; at most 1,024 exits are processed before stopping.

The fixed bootstrap CPUID policy first checks the physical CPU for its
required instruction and address-width baseline. It advertises one virtual
processor, 36 physical/48 linear address bits, PAE, long mode, NX and the
baseline floating-point/SSE facilities. It does not expose host identity or
advertise APIC, VMX, XSAVE/AVX, MTRR, PAT, SEV or TDX. Unsupported leaves return
zero. This is a bootstrap policy, not a qualified desktop CPU profile.

Success now requires an I/O exit after CPUID handling, with EFER.LMA,
CR0.PG and a 64-bit code segment. The pinned image reached a four-byte read
of PCI configuration address port `0xcf8` at linear RIP `0xfffcdf76`.
That I/O is observed but not emulated or forwarded to the host.

The [upstream EDK II transition](https://github.com/tianocore/edk2/blob/edk2-stable202402/UefiCpuPkg/ResetVector/Vtf0/Ia16/Real16ToFlat32.asm)
provides the source context for this early execution path. The
[qualification receipt](evidence/2026-09-17-spark/ovmf-reset.json) records the
earlier reset/protected-mode evidence. The
[long-mode receipt](evidence/2026-09-17-spark/ovmf-long-mode.json) records the
current CPU-exit path and retained evidence.

## Remaining boot implementation

PCI configuration and additional device/VM-exit handling must continue this
execution through firmware initialization. UEFI also requires an emulated
machine description, generated ACPI and interrupt topology, boot media over
canonical agentOS services, and actual Linux userspace evidence. The current
read-only firmware mapping does not implement persistent UEFI variables.
The default SDK and ordinary `make gate` remain unchanged.
