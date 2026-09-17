# Private firmware ACPI PM registers

The VMM emulates the PIIX4 PM register aperture in private configuration state.
Its base comes from PCI configuration offset `0x40`; `PMIOSE` at offset `0x80`
enables decode independently of the PCI command I/O bit. This follows the
[QEMU PIIX4 board model](https://github.com/qemu/qemu/blob/v10.2.1/hw/acpi/piix4.c#L87).
No guest operation accesses a host port, frame or IRQ.

The register contract is part of
[x86_config.h](../platform/include/platform/x86_config.h):

| Offset | Register | Supported behavior |
| --- | --- | --- |
| `0x00` | PM1 status | Byte/aligned-word reads; write-one-to-clear |
| `0x02` | PM1 enable | Reads zero; zero writes accepted; interrupt enables rejected |
| `0x04` | PM1 control | Byte/aligned-word access; retains SCI_EN, BM_RLD and SLP_TYP |
| `0x08` | PM timer | Read-only 32-bit access to a 24-bit counter |

The timer uses the admitted clock at 3,579,545 Hz. Status bit TMR_STS latches
when counter bit 23 changes, including multiple skipped transitions. Clearing
it does not reset the counter. Backwards time is rejected without mutation.
Reserved status bits remain zero; writing them does not synthesize events.
Partial control writes preserve the untouched byte.

SCI_EN records the guest's mode bit; it does not grant interrupt routing.
All PM1 event enables remain zero. BM_RLD is retained with no bus-master wake
source. SLP_TYP can be prepared, but SLP_EN, GBL_RLS and unsupported control
bits are rejected before any state changes. There is no simulated successful
suspend, power-off, SMI, RTC wake or power-button event. Those operations need
explicit guest lifecycle and interrupt contracts before being admitted.

`make test-x86-config-host` asserts control readback, byte-lane preservation,
enabled/disabled decode, timer status and W1C across skipped wraps, state
isolation, widths/alignment, clock reversal, and atomic rejection of unsupported
SCI, SMI and sleep requests. It does not establish guest ACPI table correctness
or a completed UEFI boot. The Intel continuation is separately recorded in
[the PM receipt](evidence/2026-09-17-spark/ovmf-pm.json).

MAC: `task_7a994cf2265342a694917f99aa2986e5`.
