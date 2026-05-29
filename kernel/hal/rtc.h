/*
 * rtc.h — CMOS real-time clock reader
 *
 * Phase 8.  Reads wall-clock time from the legacy MC146818 RTC via CMOS
 * I/O ports 0x70/0x71.  Used by the desktop shell to show a live clock.
 */

#pragma once

#include "../include/types.h"

typedef struct {
    UINT8  second;   /* 0–59 */
    UINT8  minute;   /* 0–59 */
    UINT8  hour;     /* 0–23 (24-hour) */
    UINT8  day;      /* 1–31 */
    UINT8  month;    /* 1–12 */
    UINT16 year;     /* full year, e.g. 2026 */
} RtcTime;

/* Read the current time, handling BCD/binary and 12/24-hour formats and
 * guarding against an in-progress update. */
void rtc_read(RtcTime *out);
