#ifndef RTC_H
#define RTC_H

#include "syscall.h"

/* Write formatted date/time into buf: "YYYY-MM-DD HH:MM:SS" (19 chars + NUL). */
void rtc_read_datetime(char *buf);

/* Read current hour (0-23), minute (0-59), second (0-59). */
void rtc_read_hms(unsigned int *hour, unsigned int *minute, unsigned int *second);

/* Read the full date/time.  The two-digit RTC year is expanded with a
 * 1970-2069 window (70-99 -> 1900s, 00-69 -> 2000s), so a dead-battery
 * default such as 1980 or 1990 stays in the past.  A failed read keeps the
 * last good value. */
void rtc_read_datetime_full(struct sys_datetime *out);

/* Validate and write the RTC; returns 0 on success, -1 on a bad field or a
 * write that could not be sequenced (see rtc.c).  Accepts 1970-2069. */
int rtc_set_datetime(const struct sys_datetime *in);

#endif
