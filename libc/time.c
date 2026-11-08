/* UTC wall-clock support for ring-3 commands.
 *
 * The CMOS RTC is exposed through SYS_RTC_GET/SYS_RTC_SET.  time_t is 64-bit
 * so the RTC's 1970-2069 range is representable (a signed 32-bit time_t would
 * stop at 2038).  There is no timezone database: localtime == gmtime. */

#include <time.h>
#include <sys/time.h>
#include <errno.h>
#include <string.h>
#include <ucmd_lib.h>

static const char *const wday_abbr[7] = {
	"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};
static const char *const wday_full[7] = {
	"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
	"Saturday"
};
static const char *const mon_abbr[12] = {
	"Jan", "Feb", "Mar", "Apr", "May", "Jun",
	"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};
static const char *const mon_full[12] = {
	"January", "February", "March", "April", "May", "June",
	"July", "August", "September", "October", "November", "December"
};

static long long days_from_civil(int y, unsigned m, unsigned d)
{
	long long era;
	unsigned yoe, doy, doe;

	y -= m <= 2;
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = (unsigned)(y - era * 400);
	doy = (153u * (m + (m > 2 ? (unsigned)-3 : 9u)) + 2u) / 5u + d - 1u;
	doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
	return era * 146097LL + (long long)doe - 719468LL;
}

static void civil_from_days(long long z, int *year, unsigned *month,
			    unsigned *day)
{
	long long era;
	unsigned doe, yoe, doy, mp;

	z += 719468LL;
	era = (z >= 0 ? z : z - 146096LL) / 146097LL;
	doe = (unsigned)(z - era * 146097LL);
	yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
	*month = 0;
	*day = 0;
	{
		int y = (int)yoe + (int)era * 400;
		doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
		mp = (5u * doy + 2u) / 153u;
		*day = doy - (153u * mp + 2u) / 5u + 1u;
		*month = mp + (mp < 10u ? 3u : (unsigned)-9);
		y += (*month <= 2u);
		*year = y;
	}
}

static int epoch_from_tm(const struct tm *tm, time_t *out)
{
	long long days;
	long long sec;

	if (!tm || tm->tm_mon < 0 || tm->tm_mon > 11 ||
	    tm->tm_mday < 1 || tm->tm_mday > 31 ||
	    tm->tm_hour < 0 || tm->tm_hour > 23 ||
	    tm->tm_min < 0 || tm->tm_min > 59 ||
	    tm->tm_sec < 0 || tm->tm_sec > 59)
		return -1;
	if (tm->tm_year < -1900 || tm->tm_year > 8099)
		return -1;

	days = days_from_civil(tm->tm_year + 1900,
			       (unsigned)(tm->tm_mon + 1),
			       (unsigned)tm->tm_mday);
	sec = days * 86400LL + tm->tm_hour * 3600LL +
	      tm->tm_min * 60LL + tm->tm_sec;
	*out = (time_t)sec;
	return 0;
}

static int rtc_now(struct tm *tm)
{
	struct sys_datetime dt;

	if (u_rtc_get(&dt) != 0) {
		errno = EIO;
		return -1;
	}
	if (dt.year < 1970 || dt.year > 2069 ||
	    dt.month < 1 || dt.month > 12 ||
	    dt.day < 1 || dt.day > 31 ||
	    dt.hour > 23 || dt.minute > 59 || dt.second > 59) {
		errno = EINVAL;
		return -1;
	}

	tm->tm_sec = (int)dt.second;
	tm->tm_min = (int)dt.minute;
	tm->tm_hour = (int)dt.hour;
	tm->tm_mday = (int)dt.day;
	tm->tm_mon = (int)dt.month - 1;
	tm->tm_year = (int)dt.year - 1900;
	tm->tm_wday = (int)(dt.wday % 7u);
	tm->tm_yday = (int)(days_from_civil((int)dt.year, (unsigned)dt.month,
					   (unsigned)dt.day) -
			    days_from_civil((int)dt.year, 1u, 1u));
	tm->tm_isdst = 0;
	return 0;
}

time_t time(time_t *tloc)
{
	struct tm tm;
	time_t t;

	if (rtc_now(&tm) != 0)
		return (time_t)-1;
	if (epoch_from_tm(&tm, &t) != 0) {
		errno = EINVAL;
		return (time_t)-1;
	}
	if (tloc)
		*tloc = t;
	return t;
}

struct tm *gmtime(const time_t *timer)
{
	static struct tm tm;
	long long sec;
	long long days;
	unsigned sod;
	int y;
	unsigned m, d;

	if (!timer) {
		errno = EINVAL;
		return 0;
	}
	sec = (long long)*timer;
	if (sec >= 0) {
		days = sec / 86400LL;
		sod = (unsigned)(sec % 86400LL);
	} else {
		long long q = (-sec + 86399LL) / 86400LL;

		days = -q;
		sod = (unsigned)(sec - days * 86400LL);
	}
	civil_from_days(days, &y, &m, &d);

	tm.tm_year = y - 1900;
	tm.tm_mon = (int)m - 1;
	tm.tm_mday = (int)d;
	tm.tm_hour = (int)(sod / 3600u);
	tm.tm_min = (int)((sod / 60u) % 60u);
	tm.tm_sec = (int)(sod % 60u);
	tm.tm_wday = (int)((days + 4) % 7);
	if (tm.tm_wday < 0)
		tm.tm_wday += 7;
	tm.tm_yday = (int)(days - days_from_civil(y, 1u, 1u));
	tm.tm_isdst = 0;
	return &tm;
}

struct tm *localtime(const time_t *timer)
{
	/* No timezone database: the RTC is UTC. */
	return gmtime(timer);
}

time_t mktime(struct tm *tm)
{
	time_t t;

	if (epoch_from_tm(tm, &t) != 0) {
		errno = EINVAL;
		return (time_t)-1;
	}
	return t;
}

static int out_ch(char **pp, char *end, char c)
{
	if (*pp + 1 >= end)
		return -1;
	*(*pp)++ = c;
	return 0;
}

static int out_dec(char **pp, char *end, unsigned int v, int width)
{
	char tmp[12];
	int n = 0;
	int i;

	do {
		tmp[n++] = (char)('0' + v % 10u);
		v /= 10u;
	} while (v);
	while (n < width) {
		if (out_ch(pp, end, '0'))
			return -1;
		width--;
	}
	for (i = n - 1; i >= 0; i--) {
		if (out_ch(pp, end, tmp[i]))
			return -1;
	}
	return 0;
}

static int out_str(char **pp, char *end, const char *s)
{
	while (*s) {
		if (out_ch(pp, end, *s++))
			return -1;
	}
	return 0;
}

size_t strftime(char *s, size_t max, const char *format, const struct tm *tm)
{
	char *p = s;
	char *end = s + max;

	if (!s || !format || !tm || max == 0) {
		errno = EINVAL;
		return 0;
	}

	for (; *format; format++) {
		int c;

		if (*format != '%') {
			if (out_ch(&p, end, *format))
				return 0;
			continue;
		}
		format++;
		if (!*format) {
			errno = EINVAL;
			return 0;
		}
		c = *format;
		switch (c) {
		case 'Y':
			if (out_dec(&p, end, (unsigned)(tm->tm_year + 1900), 4))
				return 0;
			break;
		case 'y':
			if (out_dec(&p, end, (unsigned)(tm->tm_year % 100), 2))
				return 0;
			break;
		case 'm':
			if (out_dec(&p, end, (unsigned)(tm->tm_mon + 1), 2))
				return 0;
			break;
		case 'd':
			if (out_dec(&p, end, (unsigned)tm->tm_mday, 2))
				return 0;
			break;
		case 'e':
			if (out_dec(&p, end, (unsigned)tm->tm_mday, 2))
				return 0;
			if (p - 2 >= s)
				*(p - 2) = ' ';
			break;
		case 'H':
			if (out_dec(&p, end, (unsigned)tm->tm_hour, 2))
				return 0;
			break;
		case 'M':
			if (out_dec(&p, end, (unsigned)tm->tm_min, 2))
				return 0;
			break;
		case 'S':
			if (out_dec(&p, end, (unsigned)tm->tm_sec, 2))
				return 0;
			break;
		case 'j':
			if (out_dec(&p, end, (unsigned)(tm->tm_yday + 1), 3))
				return 0;
			break;
		case 'w':
			if (out_ch(&p, end, (char)('0' + (tm->tm_wday % 7))))
				return 0;
			break;
		case 'u':
			if (out_ch(&p, end, (char)('0' + (tm->tm_wday ? tm->tm_wday : 7))))
				return 0;
			break;
		case 'a':
			if (out_str(&p, end, wday_abbr[(unsigned)(tm->tm_wday % 7)]))
				return 0;
			break;
		case 'A':
			if (out_str(&p, end, wday_full[(unsigned)(tm->tm_wday % 7)]))
				return 0;
			break;
		case 'b':
			if (out_str(&p, end, mon_abbr[(unsigned)(tm->tm_mon % 12)]))
				return 0;
			break;
		case 'B':
			if (out_str(&p, end, mon_full[(unsigned)(tm->tm_mon % 12)]))
				return 0;
			break;
		case 'p':
			if (out_str(&p, end, tm->tm_hour < 12 ? "AM" : "PM"))
				return 0;
			break;
		case 'F':
			if (out_dec(&p, end, (unsigned)(tm->tm_year + 1900), 4) ||
			    out_ch(&p, end, '-') ||
			    out_dec(&p, end, (unsigned)(tm->tm_mon + 1), 2) ||
			    out_ch(&p, end, '-') ||
			    out_dec(&p, end, (unsigned)tm->tm_mday, 2))
				return 0;
			break;
		case 'T':
			if (out_dec(&p, end, (unsigned)tm->tm_hour, 2) ||
			    out_ch(&p, end, ':') ||
			    out_dec(&p, end, (unsigned)tm->tm_min, 2) ||
			    out_ch(&p, end, ':') ||
			    out_dec(&p, end, (unsigned)tm->tm_sec, 2))
				return 0;
			break;
		case 's': {
			time_t t;
			char tmp[32];
			char *q = tmp + sizeof(tmp);
			long long v;
			unsigned long long u;
			int neg;

			if (epoch_from_tm(tm, &t) != 0)
				return 0;
			v = (long long)t;
			neg = v < 0;
			u = neg ? (unsigned long long)(-v) : (unsigned long long)v;
			*--q = '\0';
			do {
				*--q = (char)('0' + u % 10u);
				u /= 10u;
			} while (u);
			if (neg)
				*--q = '-';
			if (out_str(&p, end, q))
				return 0;
			break;
		}
		case 'z':
		case 'Z':
			/* UTC / no timezone database. */
			break;
		case '%':
			if (out_ch(&p, end, '%'))
				return 0;
			break;
		default:
			if (out_ch(&p, end, '%') || out_ch(&p, end, c))
				return 0;
			break;
		}
	}
	if (p >= end)
		return 0;
	*p = '\0';
	return (size_t)(p - s);
}

char *asctime(const struct tm *tm)
{
	static char buf[32];
	char *p = buf;

	if (!tm) {
		errno = EINVAL;
		return 0;
	}
	for (const char *q = wday_abbr[tm->tm_wday % 7]; *q; q++) *p++ = *q;
	*p++ = ' ';
	for (const char *q = mon_abbr[tm->tm_mon % 12]; *q; q++) *p++ = *q;
	*p++ = ' ';
	*p++ = (char)('0' + (tm->tm_mday / 10) % 10);
	*p++ = (char)('0' + tm->tm_mday % 10);
	*p++ = ' ';
	*p++ = (char)('0' + (tm->tm_hour / 10) % 10);
	*p++ = (char)('0' + tm->tm_hour % 10);
	*p++ = ':';
	*p++ = (char)('0' + (tm->tm_min / 10) % 10);
	*p++ = (char)('0' + tm->tm_min % 10);
	*p++ = ':';
	*p++ = (char)('0' + (tm->tm_sec / 10) % 10);
	*p++ = (char)('0' + tm->tm_sec % 10);
	*p++ = ' ';
	{
		int y = tm->tm_year + 1900;
		*p++ = (char)('0' + (y / 1000) % 10);
		*p++ = (char)('0' + (y / 100) % 10);
		*p++ = (char)('0' + (y / 10) % 10);
		*p++ = (char)('0' + y % 10);
	}
	*p++ = '\n';
	*p = '\0';
	return buf;
}

char *ctime(const time_t *timer)
{
	return asctime(localtime(timer));
}

double difftime(time_t time1, time_t time0)
{
	return (double)time1 - (double)time0;
}

int gettimeofday(struct timeval *tv, void *tz)
{
	time_t t;
	(void)tz;

	if (!tv) {
		errno = EINVAL;
		return -1;
	}
	t = time(0);
	if (t == (time_t)-1)
		return -1;
	tv->tv_sec = t;
	tv->tv_usec = (suseconds_t)((u_ticks_ms() % 1000u) * 1000u);
	return 0;
}

int settimeofday(const struct timeval *tv, const void *tz)
{
	struct tm tm;
	struct sys_datetime dt;
	time_t t;
	(void)tz;

	if (!tv) {
		errno = EINVAL;
		return -1;
	}
	t = tv->tv_sec;
	{
		struct tm *p = gmtime(&t);

		if (!p) {
			errno = EINVAL;
			return -1;
		}
		tm = *p;
	}
	if (tm.tm_year + 1900 < 1970 || tm.tm_year + 1900 > 2069) {
		errno = EINVAL;
		return -1;
	}
	dt.year = (unsigned)(tm.tm_year + 1900);
	dt.month = (unsigned)(tm.tm_mon + 1);
	dt.day = (unsigned)tm.tm_mday;
	dt.hour = (unsigned)tm.tm_hour;
	dt.minute = (unsigned)tm.tm_min;
	dt.second = (unsigned)tm.tm_sec;
	dt.wday = (unsigned)(tm.tm_wday % 7);
	if (u_rtc_set(&dt) != 0) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

int clock_gettime(int clock_id, struct timespec *tp)
{
	if (!tp) {
		errno = EINVAL;
		return -1;
	}
	if (clock_id == CLOCK_REALTIME) {
		struct timeval tv;
		if (gettimeofday(&tv, 0) != 0)
			return -1;
		tp->tv_sec = tv.tv_sec;
		tp->tv_nsec = (long)tv.tv_usec * 1000L;
		return 0;
	}
	if (clock_id == CLOCK_MONOTONIC) {
		unsigned int ms = u_ticks_ms();
		tp->tv_sec = (time_t)(ms / 1000u);
		tp->tv_nsec = (long)(ms % 1000u) * 1000000L;
		return 0;
	}
	errno = EINVAL;
	return -1;
}

int clock_settime(int clock_id, const struct timespec *tp)
{
	if (clock_id != CLOCK_REALTIME || !tp) {
		errno = EINVAL;
		return -1;
	}
	{
		struct timeval tv;
		tv.tv_sec = tp->tv_sec;
		tv.tv_usec = tp->tv_nsec / 1000L;
		return settimeofday(&tv, 0);
	}
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
	unsigned long ms;

	if (!req || req->tv_sec < 0 || req->tv_nsec < 0 ||
	    req->tv_nsec >= 1000000000L) {
		errno = EINVAL;
		return -1;
	}
	ms = (unsigned long)req->tv_sec * 1000UL +
	     (unsigned long)(req->tv_nsec + 999999L) / 1000000L;
	if (ms > 0xFFFFFFFFu)
		ms = 0xFFFFFFFFu;
	u_sleep_ms((unsigned int)ms);
	if (rem) {
		rem->tv_sec = 0;
		rem->tv_nsec = 0;
	}
	return 0;
}
