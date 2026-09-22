# x86 guest ACPI

`platform/include/platform/x86_acpi.h` defines the input contract for the
VMM's interrupt-topology table builder. `aos_x86_madt_write` emits a bounded
MADT following [ACPI 6.6 section 5.2.12](https://uefi.org/specs/ACPI/6.6/05_ACPI_Software_Programming_Model.html#multiple-apic-description-table-madt).
It encodes fields in little-endian byte order and computes the whole-table
checksum without relying on compiler structure packing or host alignment.

The contract describes one to 32 provisioned vCPUs and one emulated 24-input
I/O APIC. Each processor has a distinct integer ACPI UID and local APIC ID.
The I/O APIC ID must also be distinct. Controller addresses are nonzero,
page-aligned guest physical addresses; they must refer to VMM emulation,
never mappings of the host's controllers. The table advertises neither a
legacy 8259 PIC nor CPU hotplug. Invalid configurations and short buffers
return zero without altering output.

`aos_x86_cpu_ssdt_write` generates matching `ACPI0007` processor Device objects
under `\\_SB_`, with integer `_UID` values from the same topology. It uses
bounded AML templates and a correctly encoded package length for the enclosing
scope. Both builders validate the same input before modifying output.

The firmware-reset composition calls `aos_x86_acpi_bundle_init` and exposes
immutable `etc/acpi/tables`, `etc/acpi/rsdp` and `etc/table-loader` files through
its private fw_cfg interface. The bundle contains FACS, an empty DSDT, FADT,
MADT, CPU SSDT, RSDT, XSDT and RSDP. The loader uses the standard
[EDK2 table-loader commands](https://github.com/tianocore/edk2/blob/edk2-stable202402/OvmfPkg/Include/IndustryStandard/QemuLoader.h)
to allocate guest copies, relocate pointers and recalculate checksums. OVMF's
ACPI protocol installs the referenced tables; no VMM source buffer is mapped
into the guest.

This installed profile is deliberately fixed at the one provisioned vCPU:
CPU UID/APIC ID 0, LAPIC at `0xfee00000`, I/O APIC ID 1 at `0xfec00000`,
24 inputs starting at GSI 0. Both controller pages fault into VMM emulation.
The FADT describes the private PIIX4 PM block at I/O `0xb000`, its 24-bit
timer, ACPI-only mode and RTC century register `0x32`. There are no sleep
states, enabled SCI sources, legacy PIC, VGA, MSI, PCI endpoints or hotplug
objects. The 32-CPU serializer capacity is not a claim of runtime SMP support.

The I/O APIC retains masks, destinations and polarity, loses masked edges,
and retries asserted levels after matching LAPIC EOI. Sources supply logical
assertion rather than physical wire voltage. Remote IRR is independent of
guest RTE writes, including vector changes while an interrupt is in service.
Only fixed and lowest-priority routes to the sole eligible LAPIC are supported.
No host device frame or IRQ capability is added. Canonical guest device
backends have not yet been connected to these inputs.

`make test-x86-acpi-loader-host` interprets the actual firmware stream at
three allocation bases and checks directory sizes/selectors, relocation
bounds, all root pointers and checksums, and PM register agreement.
`make test-x86-ioapic-host` checks register policy, edge/level behavior, EOI,
reprogramming and per-instance isolation. These host tests are not a guest
interrupt-handler proof.

The [Intel integration receipt](evidence/2026-09-17-spark/ovmf-linux-acpi.json)
records Linux moving from periodic PIT setup to PIT shutdown, then stopping
on a read of MSR `0x3a` (IA32_FEATURE_CONTROL). The ACPI/ARAT combination
therefore removes the observed PIT dependency, but Linux userspace and the
complete interrupt/device path remain unqualified. Unsupported accesses
still stop this bounded qualification profile; guest exception delivery needs
its own implementation and target proof.

The subsequent [guest-fault implementation](x86-guest-faults.md) adds that
MSR fault path and qualifies long-mode handler recovery. Linux then reaches
PCI mechanism discovery; the overall Intel Linux gate remains incomplete.

`make test-x86-acpi-host` checks an independent exact two-vCPU wire fixture,
the maximum topology, checksums, duplicate IDs, reserved broadcast IDs,
address alignment, GSI overflow and unchanged output on rejection. It is
included in `make test-host`. These are host table-construction tests, not
UEFI boot or interrupt-delivery qualification.

`make test-x86-acpi-aml` additionally uses ACPICA's `iasl` to disassemble and
recompile the generated maximum-size CPU SSDT, then `acpiexec` to evaluate
representative UIDs and the hardware ID. Set `IASL` and `ACPIEXEC` to their
executable paths if not installed on PATH. This passed on Spark with ACPICA
20230628: 32 Device objects, no compiler errors or warnings, and the expected
integer UIDs and `ACPI0007` hardware ID. Generated AML and diagnostic files stay
in the build directory.
