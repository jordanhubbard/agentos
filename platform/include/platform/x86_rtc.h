#ifndef AOS_X86_RTC_H
#define AOS_X86_RTC_H
#include <stdbool.h>
#include <stdint.h>

#define AOS_X86_RTC_HZ UINT64_C(3579545)
/* Deterministic private boot epoch, not host wall-clock or persistent time. */
#define AOS_X86_RTC_BOOT_EPOCH UINT64_C(946684800) /* 2000-01-01 UTC */
typedef struct {
    uint64_t epoch, base, last;
    uint8_t a, b, flags, weekday_bias;
    uint8_t staged[8]; /* sec, min, hour, weekday, day, month, year, century */
    uint8_t alarm[3]; /* raw sec/min/hour format; top two bits mean any */
} aos_x86_rtc_t;
bool aos_x86_rtc_init(aos_x86_rtc_t *r, uint64_t epoch, uint64_t ticks);
/* Private MC146818 calendar subset. Ticks are monotonic at RTC_HZ. Calendar
 * writes require SET; clearing SET validates and atomically commits the date.
 * IRQ enables, square wave and divider-stop modes are unsupported.
 * False preserves both state and value. Century is CMOS register 0x32. */
bool aos_x86_rtc_io(aos_x86_rtc_t *r, unsigned reg, bool write,
                    uint32_t *value, uint64_t ticks);
#endif
