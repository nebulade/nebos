#ifndef RTC_H
#define RTC_H

/* Write formatted date/time into buf: "YYYY-MM-DD HH:MM:SS" (19 chars + NUL). */
void rtc_read_datetime(char *buf);

/* Read current hour (0-23), minute (0-59), second (0-59). */
void rtc_read_hms(unsigned int *hour, unsigned int *minute, unsigned int *second);

#endif
