/*
 * rtc.c — CMOS real-time clock reader (MC146818)
 *
 * Reads CMOS registers through ports 0x70 (index) / 0x71 (data).  Values
 * may be in BCD and the hour in 12-hour form depending on Status Register
 * B; we normalize both to plain binary, 24-hour.
 */

#include "rtc.h"
#include "../arch/x86_64/cpu.h"

#define CMOS_INDEX  0x70
#define CMOS_DATA   0x71

#define RTC_SECONDS 0x00
#define RTC_MINUTES 0x02
#define RTC_HOURS   0x04
#define RTC_DAY     0x07
#define RTC_MONTH   0x08
#define RTC_YEAR    0x09
#define RTC_STATUS_A 0x0A
#define RTC_STATUS_B 0x0B

static UINT8 cmos_read(UINT8 reg)
{
    /* Bit 7 of the index port disables NMI; keep it set to avoid toggling. */
    outb(CMOS_INDEX, (UINT8)(0x80 | reg));
    io_wait();
    return inb(CMOS_DATA);
}

static bool update_in_progress(void)
{
    return (cmos_read(RTC_STATUS_A) & 0x80) != 0;
}

static UINT8 bcd_to_bin(UINT8 v)
{
    return (UINT8)((v & 0x0F) + ((v >> 4) * 10));
}

void rtc_read(RtcTime *out)
{
    if (!out) return;

    /* Wait for any in-progress update to finish, then take a coherent read. */
    while (update_in_progress()) { }

    UINT8 sec   = cmos_read(RTC_SECONDS);
    UINT8 min   = cmos_read(RTC_MINUTES);
    UINT8 hour  = cmos_read(RTC_HOURS);
    UINT8 day   = cmos_read(RTC_DAY);
    UINT8 month = cmos_read(RTC_MONTH);
    UINT8 year  = cmos_read(RTC_YEAR);
    UINT8 regb  = cmos_read(RTC_STATUS_B);

    bool bcd     = (regb & 0x04) == 0;   /* bit2 clear → BCD */
    bool hour12  = (regb & 0x02) == 0;   /* bit1 clear → 12-hour */
    bool pm      = (hour & 0x80) != 0;   /* in 12h mode, bit7 = PM */

    if (bcd) {
        sec   = bcd_to_bin(sec);
        min   = bcd_to_bin(min);
        hour  = bcd_to_bin((UINT8)(hour & 0x7F));
        day   = bcd_to_bin(day);
        month = bcd_to_bin(month);
        year  = bcd_to_bin(year);
    } else {
        hour &= 0x7F;
    }

    if (hour12) {
        /* Convert 12-hour to 24-hour. */
        if (hour == 12) hour = 0;
        if (pm) hour = (UINT8)(hour + 12);
    }

    out->second = sec;
    out->minute = min;
    out->hour   = hour;
    out->day    = day;
    out->month  = month;
    out->year   = (UINT16)(2000 + year);
}
