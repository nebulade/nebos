#ifndef RTC_H
#define RTC_H

/* Write formatted date/time into buf: "YYYY-MM-DD HH:MM:SS" (19 chars + NUL). */
void rtc_read_datetime(char *buf);

#endif
