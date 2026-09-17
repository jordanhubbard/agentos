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

Firmware integration must create matching `ACPI0007` processor Device objects
with integer `_UID` values, install the MADT through the guest's XSDT, and
provide the required FADT, DSDT and RSDP. The VMM must actually provision the
advertised vCPUs, emulate both interrupt controllers, and reserve controller
GPAs outside guest RAM. This builder does not perform those operations and
is not yet called by a bootable x86 Linux composition.

`make test-x86-acpi-host` checks an independent exact two-vCPU wire fixture,
the maximum topology, checksums, duplicate IDs, reserved broadcast IDs,
address alignment, GSI overflow and unchanged output on rejection. It is
included in `make test-host`. These are host table-construction tests, not
UEFI boot or interrupt-delivery qualification.
