/* Wall-clock time for ring-3 commands (UTC only). */
#ifndef LIBC_TIME_H
#define LIBC_TIME_H

#include <stddef.h>
#include <sys/types.h>

struct tm {
	int tm_sec;
	int tm_min;
	int tm_hour;
	int tm_mday;
	int tm_mon;
	int tm_year;
	int tm_wday;
	int tm_yday;
	int tm_isdst;
};

#define CLOCKS_PER_SEC 1000
typedef long clock_t;

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

struct timespec {
	time_t tv_sec;
	long tv_nsec;
};

time_t time(time_t *tloc);
struct tm *gmtime(const time_t *timer);
struct tm *localtime(const time_t *timer);
time_t mktime(struct tm *tm);
size_t strftime(char *s, size_t max, const char *format,
		const struct tm *tm);
char *ctime(const time_t *timer);
char *asctime(const struct tm *tm);
double difftime(time_t time1, time_t time0);

int clock_gettime(int clock_id, struct timespec *tp);
int clock_settime(int clock_id, const struct timespec *tp);
int nanosleep(const struct timespec *req, struct timespec *rem);

#endif
