# x86 firmware bring-up

The opt-in reset variant runs a real, externally supplied OVMF image under
the seL4 VMM. It handles bootstrap CPU exits through the firmware's own
protected- and long-mode transitions, then emulates a bounded subset of
guest-owned configuration I/O. Unsupported operations stop with diagnostics.
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
diagnostics; at most 65,536 exits are processed before stopping.

The fixed bootstrap CPUID policy first checks the physical CPU for its
required instruction and address-width baseline. It advertises one virtual
processor, 36 physical/48 linear address bits, PAE, long mode, NX and the
baseline floating-point/SSE facilities. It does not expose host identity or
advertise APIC, VMX, XSAVE/AVX, MTRR, PAT, SEV or TDX. Unsupported leaves return
zero. This is a bootstrap policy, not a qualified desktop CPU profile.

Scalar I/O now uses private PCI configuration state for an i440FX host bridge
and PIIX4 power-management function. The PM timer requires enabled decode and
an invariant TSC with a nonzero CPUID.15H frequency; no clock frequency is
guessed. Host tests cover its wrap and enable semantics. The guest-owned
`fw_cfg` data supplies RAM/CPU counts and an E820 directory entry; CMOS supplies
RAM-size fields. The legacy PICs accept mask-all only: unmasking and commands
remain unsupported until interrupt routing and injection are implemented.
No operation forwards a host port or grants a hardware I/O capability.

The configuration gate requires reaching a firmware-data string I/O exit
after PCI reads, CPUID handling and the long-mode transition. **This gate is
not yet passing.** Intel execution advances through PCI access and PIC masking
and stops at RDMSR of `IA32_APIC_BASE` (`0x1b`) at `0xfffcc525`. The next
dependency is a guest-owned local APIC and timer model. String I/O still needs
bounded guest-memory access; scalar firmware
data and PM timer tests alone do not prove firmware consumption on target.

The [upstream EDK II transition](https://github.com/tianocore/edk2/blob/edk2-stable202402/UefiCpuPkg/ResetVector/Vtf0/Ia16/Real16ToFlat32.asm)
provides the source context for this early execution path. The
[qualification receipt](evidence/2026-09-17-spark/ovmf-reset.json) records the
earlier reset/protected-mode evidence. The
[long-mode receipt](evidence/2026-09-17-spark/ovmf-long-mode.json) records the
earlier CPU-exit path and retained evidence. The
[configuration receipt](evidence/2026-09-17-spark/ovmf-config.json) records the
new host tests, ordinary Spark gate and incomplete Intel run.

## Remaining boot implementation

Additional device/VM-exit handling must continue this
execution through firmware initialization. UEFI also requires an emulated
machine description, generated ACPI and interrupt topology, boot media over
canonical agentOS services, and actual Linux userspace evidence. The current
read-only firmware mapping does not implement persistent UEFI variables.
The default SDK and ordinary `make gate` remain unchanged.
