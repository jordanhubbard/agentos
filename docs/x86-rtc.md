# Private x86 firmware RTC

The firmware VMM owns a private calendar in `aos_x86_config_t`; it does not
access a physical RTC, host clock service, device frame or port capability.
The interface is [x86_rtc.h](../platform/include/platform/x86_rtc.h).

Each new firmware instance starts at **2000-01-01 00:00:00 UTC**. This is an
explicit virtual boot epoch, not a claim about the current date. Time advances
from the VMM's measured invariant TSC, converted to the same 3,579,545 Hz
monotonic scale used by the private PM timer. There is no elapsed-time estimate
based on VM-exit count. The clock is neither host-synchronized nor persistent
across a new VMM instance; a guest can set its private calendar.

The CMOS index/data ports retain their existing byte-only interface. Reads of
the write-only index port return all ones and do not alter the index. RTC
calendar registers cover seconds, minutes, hours, weekday, day, month, year,
and century at index `0x32`. Other existing CMOS RAM-size and shutdown-status
fields remain handled separately by the configuration model.

Calendar reads support BCD/binary and 12/24-hour modes. The model admits the
Gregorian calendar from 1970 through 9999, including the century leap-year
rule. Weekday advances modulo seven from the guest's programmed weekday.
SET freezes the calendar for a write transaction. Individual fields are range
checked; clearing SET validates the complete date before committing it and
restarting elapsed-time accumulation. An invalid date leaves the transaction
in SET state so the guest can correct it. Rejected operations preserve state
and the caller's value.

Register A admits the normal divider with selectable periodic rate. UIP warns
during the final 244 microseconds before an atomic calendar update, and is
clear during SET. Register C latches elapsed update/periodic/alarm events and clears
on read. Register D reports valid virtual time. Writes to C/D are ignored,
preserving their read-only status, as in QEMU's
[MC146818 model](https://github.com/qemu/qemu/blob/v10.2.1/hw/rtc/mc146818rtc.c).
The control/format reference is the
[DS12885/DS12887 datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/DS12885-DS12C887A.pdf).

Alarm second/minute/hour registers retain their encoded values, including
the two-high-bits "don't care" encoding. Matching considers every elapsed
second in a bounded interval of at most one day, so a delayed observation
does not lose an alarm event. SET inhibits calendar/alarm updates while
periodic flags continue.

This is a calendar subset, not full RTC hardware qualification.
IRQ enables, square-wave output, automatic daylight savings and
divider-stop modes are rejected. No RTC IRQ is advertised or injected.
Backwards clock input and dates beyond the supported range are rejected.

`make test-x86-rtc-host` asserts elapsed-time rollover, leap years, SET
freeze/commit and rollback, weekday, BCD/binary and noon/midnight conversion,
UIP, read-to-clear flags, read-only writes, independent instances and rejection.
`make test-x86-config-host` checks the port adapter. These host checks do not
prove complete OVMF boot; the Intel firmware gate and retained runtime evidence
are separate requirements.

MAC: `task_dce9e2bb5f494c0081a811520b478fd9`.
