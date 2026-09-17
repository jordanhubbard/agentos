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
The VMM also receives writable private RAM at `0x80000000`
and read-only ROM at `0x90000000`. These aliases permit instruction fetch,
page-table inspection and validated emulated input writes. Mapping caps remain accounted to
the VMM. No physical APIC page or I/O capability is delegated.
The firmware variant uses the existing single-VMM qualification topology.
Allocation failure aborts boot; this is not a runtime guest-create path and
does not qualify capability reclamation or retry.
Supplying an [EFI boot payload](x86-boot-payload.md) selects 256 MiB private
guest RAM; the ordinary firmware-only variant retains 32 MiB.

The VMM starts at architectural reset address `0xfffffff0`, using the special
high CS cache and unrestricted real-address entry. It explicitly enables
VM-entry EFER loading so all guest EFER bits use the supplied state. A bounded
loop preserves all guest general registers across emulation and implements
CPUID, EFER reads/writes, and MOV-to-CR0/CR4 transitions. Paging activation
updates EFER.LMA and the IA-32e entry control together. Other exits stop with
diagnostics; at most 65,536 exits are processed before stopping.

The fixed bootstrap CPUID policy first checks the physical CPU for its
required instruction and address-width baseline. It advertises one virtual
processor, 36 physical/48 linear address bits, PAE, long mode, NX, SYSCALL,
the private local xAPIC and baseline floating-point/SSE facilities.
It does not expose host identity or advertise VMX, XSAVE/AVX, MTRR, PAT,
SEV or TDX. Unsupported leaves return
zero. This is a bootstrap policy, not a qualified desktop CPU profile.

The admitted invariant TSC is exposed through CPUID leaf `0x15`, with a
1:1 TSC/crystal ratio matching the private APIC bus clock. Leaf `0x16` supplies
nominal virtual CPU MHz; it makes no maximum or bus-frequency claim. The
clock profile requires 1 MHz through `UINT32_MAX` Hz, so crystal Hz fits its
architectural field without truncation. Unknown or out-of-profile clocks
fail admission. The invariant-TSC bit is supplied only for an admitted clock.

Scalar I/O now uses private PCI configuration state for an i440FX host bridge
and PIIX4 power-management function. The PM timer requires enabled decode and
an invariant TSC with an architectural ratio or identified KVM timing leaf;
no clock frequency is guessed. Host tests cover its wrap and enable semantics. The guest-owned
`fw_cfg` data supplies RAM/CPU counts and an E820 directory entry; CMOS supplies
RAM-size fields and a [private RTC calendar](x86-rtc.md). Its explicit virtual
boot date is 2000-01-01 UTC, advancing from the measured clock, with BCD/binary
and 12/24-hour reads, SET date transactions and polled alarm/status flags.
It does not claim host wall-clock synchronization, persistent time or RTC IRQs.
There is no legacy PIC or ISA interrupt source. The two absent command/mask
port pairs read `0xff` and discard byte writes, so a mask-presence probe
detects absence. Other widths and neighboring ports remain rejected. There
is no mutable PIC state or interrupt routing hidden behind those ports.
The [private PM1 aperture](x86-pm.md) now supports control readback and polled
timer status/W1C. Its legacy decode follows PMIOSE independently of PCI IOEN.
SCI event enables, sleep and SMI requests remain explicitly rejected.
No operation forwards a host port or grants a hardware I/O capability. CMOS
shutdown status reports a cold boot; there is no S3 resume image.
Firmware can acknowledge that cold boot by clearing the status. CPU discovery
reports one fixed BSP, matching fw_cfg, with no hotplug events. Port `0x92`
reports A20 enabled and rejects reset/disable. Platform-ID and microcode
signature MSRs return synthetic zero values; microcode update triggers remain
unsupported and never reach the host.

The bootstrap xAPIC has a fixed enabled BSP base at `0xfee00000`, APIC ID zero,
SVR/TPR and timer registers. Timer counts derive from invariant host TSC ticks,
with one virtual APIC bus tick per TSC tick and the programmed divider. Both
one-shot and periodic counting are covered by host tests. The fixed-host KVM
qualification uses `host,migratable=off` so QEMU does not hide `invtsc`; the
VMM still checks that capability. The VMX preemption timer now supplies
asynchronous wakeups, using the rate obtained through the VCPU capability.
Private IRR/ISR state retains pending vectors, applies priority and handles
EOI. Injection checks IF and STI/MOVSS blocking, requests interrupt-window
exits when needed, and clears halt state only for an eligible interrupt.
Fixed edge-triggered IPIs route only to the sole provisioned vCPU through
the same private IRR/ISR state. Physical, flat/cluster logical and shorthand
destinations are supported; absent destinations deliver nothing. NMI/INIT/SIPI
delivery, base relocation and x2APIC remain rejected. LDR/DFR, ESR clearing
and thermal/performance/error LVT readback are private controller state;
there are no connected thermal or performance sources. Host tests cover
these paths; dedicated guest IPI/handler assertions remain required.
Live divider changes preserve
the remaining countdown and restart the fractional prescaler phase. Expiries
while masked do not create pending interrupts; unmasked expiries are retained.
LINT0/LINT1 configuration is private and retained for firmware virtual-wire
setup, but no external pin sources are connected.

MMIO faults use the hardware-reported GPA and a bounded decoder for
byte and 32-bit MOV register/immediate forms and byte/word MOVZX in a
64-bit code segment. APIC accesses still require 32-bit operands. A
four-level guest page-table walk checks the advertised 36-bit physical width,
permissions and supported 4 KiB/2 MiB leaves. Instruction bytes may come only
from this guest's RAM or ROM; the decoded operand must translate to the fault
GPA. Unknown instructions and other mappings stop without accessing host
memory. This is not a general x86 instruction emulator.

The earlier APIC gate stopped at REP INSB from `0x511`. The VMM now resumes
that instruction, transferring at most 1024 bytes per exit. It validates every
destination page before advancing fw_cfg or modifying RAM, sets accessed/dirty
bits, preserves forward/backward direction and zero-count semantics, and
re-enters the same instruction when RCX remains nonzero. Only exact long-mode
`F3 6C` is supported; other string widths/address modes stop explicitly.
ROM and device GPAs cannot become writable input destinations.

This continuation no longer reports success at the old string-exit checkpoint.
The Intel gate remains incomplete while firmware executes beyond that point;
host stream tests and an ordinary Spark gate do not prove complete UEFI boot.
The historical [string-input receipt](evidence/2026-09-17-spark/ovmf-string.json)
records the TPM probe stop. Bounded, naturally aligned reads of one, two or
four bytes from `0xfed40000..0xfed44fff` now return all ones for this machine's
absent TPM. Writes remain rejected; there is no TPM or measured-boot emulation.
Validated stores within firmware ROM complete without changing its bytes,
allowing firmware to distinguish ROM from writable flash. Both ROM mappings
remain read-only; this does not implement persistent variables.

The historical [discovery receipt](evidence/2026-09-17-spark/ovmf-discovery.json) records
execution through these probes and APIC divider changes to the first unmasked
periodic timer expiry, vector `0x20`. The VMM stops with reason `0x495251`
at RIP `0x00168bf5`. The later
[timer receipt](evidence/2026-09-17-spark/ovmf-timer.json) records 139 timer
exits, three injections and three guest EOI writes before an unsupported
RTC register-A write of `0x26`, at RIP `0x01acd70c`. The firmware gate still
fails. Dedicated halt and interrupt-window target assertions remain required;
these counters do not qualify all interrupt delivery cases or complete UEFI.

The [RTC continuation receipt](evidence/2026-09-17-spark/ovmf-rtc.json)
records subsequent execution beyond RTC initialization: 178 preemption exits
and 29 injection/EOI pairs before a 16-bit ACPI PM control read at port `0xb004`
stops with reason `0x1e`, RIP `0x0006ff9e`, qualification `0xb0040009`.
This is another incomplete firmware run, not a successful UEFI handoff.

The later [PM receipt](evidence/2026-09-17-spark/ovmf-pm.json) records
continuation beyond that access to the fixed 65,536-exit diagnostic limit.
The repeat run reports 10,771 timer exits, 4,885 injections, 4,884 EOIs and
4,613 HLT exits. The last observed RIP is `0x0018b0d1`, with VMX timer exit
reason 52. The limit remains a failure result; these counters do not establish
a completed UEFI boot, identify the waiting firmware component, or prove
guest payload handoff. The bound has not been increased to hide the result.

The later [endpoint investigation](evidence/2026-09-17-spark/ovmf-endpoint.json)
localizes this wait. Failure reports now carry bounded private-RAM code and
stack snapshots for the returned budget exit and the most recent HLT (or
PM-timer poll before any HLT), plus six checked frame-pointer links. The release SDK disables `DebugPutChar`, so
these observations use the existing qualification IPC endpoint. Root copies
all report words before diagnostic output. The VMM performs every translation;
root only prints the report and gains no guest-memory inspection policy.
Invalid, unaligned or non-increasing frame links stop the diagnostic chain.

The Intel HLT bytes match `CpuDxe`'s `CpuSleep`. Its retained caller chain
passes through DXE event dispatch and `CoreWaitForEvent` to return address
`0x782c4`. In the exact pinned OVMF binary, that return matches `BdsDxe`
RVA `0xa2c4`: a wait for `ConIn->WaitForKey` immediately following the
no-bootable-option/device message. This identifies the boot-manager key wait
as the next obstacle, rather than another unsupported register access.
The receipt distinguishes exact code-byte matches from module bases inferred
through consistent return sites; it is not a complete firmware unwind.

The source context for the idle path is
[DXE CoreWaitForEvent](https://github.com/tianocore/edk2/blob/edk2-stable202402/MdeModulePkg/Core/Dxe/Event/Event.c)
and [CpuDxe IdleLoopEventCallback](https://github.com/tianocore/edk2/blob/edk2-stable202402/UefiCpuPkg/CpuDxe/CpuDxe.c).
The next implementation must establish actual
UEFI handoff alongside generated ACPI and canonical guest I/O. Reaching the
key wait is useful bring-up evidence but remains a failing firmware gate.
The full Spark gate passed at runtime revision `7682937`.

The later [payload receipt](evidence/2026-09-17-spark/ovmf-boot-payload.json)
records full fw_cfg consumption of a pinned 17,295,752-byte EFI-stub Linux
kernel. Firmware then exhausts the same exit budget in Metronome's PM-timer
read at port `0xb008`. This proves blob delivery and a changed execution path,
not kernel entry or successful boot. The [payload interface](x86-boot-payload.md)
also supports bounded optional initrd and command-line inputs; neither was
provided in that first Intel run.

The [upstream EDK II transition](https://github.com/tianocore/edk2/blob/edk2-stable202402/UefiCpuPkg/ResetVector/Vtf0/Ia16/Real16ToFlat32.asm)
provides the source context for this early execution path. The
[qualification receipt](evidence/2026-09-17-spark/ovmf-reset.json) records the
earlier reset/protected-mode evidence. The
[long-mode receipt](evidence/2026-09-17-spark/ovmf-long-mode.json) records the
earlier CPU-exit path and retained evidence. The
[configuration receipt](evidence/2026-09-17-spark/ovmf-config.json) records the
initial host tests, ordinary Spark gate and incomplete Intel run. The
[APIC receipt](evidence/2026-09-17-spark/ovmf-apic.json) records the later passing
Intel gate, private-memory/MMIO tests and another full Spark gate.

## Remaining boot implementation

Additional device/VM-exit handling must continue this
execution through firmware initialization. UEFI also requires an emulated
machine description, generated ACPI and interrupt topology, boot media over
canonical agentOS services, and actual Linux userspace evidence. The current
read-only firmware mapping does not implement persistent UEFI variables.
The default SDK and ordinary `make gate` remain unchanged.
