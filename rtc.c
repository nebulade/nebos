/* RTC (CMOS) date/time read and write. */

#include "rtc.h"
#include "acpi.h"

#define CMOS_ADDR  0x70
#define CMOS_DATA  0x71

#define RTC_SEC    0x00
#define RTC_MIN    0x02
#define RTC_HOUR   0x04
#define RTC_WDAY   0x06
#define RTC_DAY    0x07
#define RTC_MON    0x08
#define RTC_YEAR   0x09
#define RTC_REGA   0x0A
#define RTC_REGB   0x0B

/* Last good value, used when a read straddles the update cycle and no retry
 * produces a complete, valid set.  Initialised to 2000-01-01 (a Saturday). */
static struct sys_datetime last_dt = { 2000, 1, 1, 0, 0, 0, 6 };

static inline unsigned char cmos_read(unsigned char reg)
{
	unsigned char v;
	/* Bit 7 on 0x70 disables NMI during access. */
	__asm__ volatile ("outb %b1, %w0" : : "Nd"(CMOS_ADDR), "a"((unsigned char)(reg | 0x80)));
	__asm__ volatile ("inb %w1, %0" : "=a"(v) : "Nd"(CMOS_DATA));
	return v;
}

static inline void cmos_write(unsigned char reg, unsigned char val)
{
	__asm__ volatile ("outb %b1, %w0" : : "Nd"(CMOS_ADDR), "a"((unsigned char)(reg | 0x80)));
	__asm__ volatile ("outb %b1, %w0" : : "Nd"(CMOS_DATA), "a"(val));
}

static int bcd_digits(unsigned char bcd)
{
	return (bcd & 0x0F) <= 9 && ((bcd >> 4) & 0x0F) <= 9;
}

static unsigned int rtc_decode(unsigned char v, int bcd)
{
	if (!bcd)
		return v;
	return (unsigned int)((v >> 4) * 10 + (v & 0x0F));
}

static unsigned char rtc_encode(unsigned int v, int bcd)
{
	if (!bcd)
		return (unsigned char)v;
	return (unsigned char)(((v / 10) << 4) | (v % 10));
}

static int digits_ok(unsigned char v, int bcd)
{
	return !bcd || bcd_digits(v);
}

/* The RTC's update-in-progress bit stays set for about 2 ms. */
static int rtc_wait_ready(void)
{
	unsigned int n = 0;

	while ((cmos_read(RTC_REGA) & 0x80) && ++n < 10000)
		;
	return (cmos_read(RTC_REGA) & 0x80) ? -1 : 0;
}

static unsigned int days_in_month(unsigned int y, unsigned int m)
{
	static const unsigned char dm[12] = {
		31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
	};

	if (m < 1 || m > 12)
		return 0;
	if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
		return 29;
	return dm[m - 1];
}

/* Sakamoto's algorithm; 0 = Sunday. */
static unsigned int weekday_of(unsigned int y, unsigned int m, unsigned int d)
{
	static const unsigned char t[12] = {
		0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4
	};

	if (m < 3)
		y -= 1;
	return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

/* The RTC only stores two digits.  A dead CMOS battery commonly leaves 1980
 * or 1990, so decode with the usual 1970-2069 window instead of assuming
 * 20xx: 70..99 -> 19xx, 00..69 -> 20xx.  This keeps pre-2000 defaults in the
 * past and still maps the current date correctly. */
static unsigned int expand_year(unsigned int yy)
{
	return (yy >= 70) ? 1900 + yy : 2000 + yy;
}

struct rtc_raw {
	unsigned char sec, min, hour, day, mon, year;
};

static void rtc_read_raw(struct rtc_raw *r)
{
	r->sec  = cmos_read(RTC_SEC);
	r->min  = cmos_read(RTC_MIN);
	r->hour = cmos_read(RTC_HOUR);
	r->day  = cmos_read(RTC_DAY);
	r->mon  = cmos_read(RTC_MON);
	r->year = cmos_read(RTC_YEAR);
}

/* Read all fields twice and require the raw bytes to agree, so a read that
 * straddles an update is retried rather than decoded as a torn value.
 * Register B selects BCD/binary and 12/24-hour mode; both are honoured. */
static int rtc_read_fields(struct sys_datetime *out)
{
	struct rtc_raw a, b;
	unsigned char regb;
	unsigned int tries;

	regb = cmos_read(RTC_REGB);
	for (tries = 0; tries < 4; tries++) {
		int is_bcd = !(regb & 0x04);
		/* Register B bit 1 is the 24-hour select: 1 = 24-hour,
		 * 0 = 12-hour with bit 7 of the hour as the PM flag. */
		int is_12h = (regb & 0x02) == 0;
		unsigned int sec, min, hour, day, mon, yy, year;
		unsigned int pm;

		if (rtc_wait_ready() != 0)
			continue;
		rtc_read_raw(&a);
		if (rtc_wait_ready() != 0)
			continue;
		rtc_read_raw(&b);
		if (a.sec != b.sec || a.min != b.min || a.hour != b.hour ||
		    a.day != b.day || a.mon != b.mon || a.year != b.year)
			continue;

		if (!digits_ok(a.sec, is_bcd) ||
		    !digits_ok(a.min, is_bcd) ||
		    !digits_ok((unsigned char)(a.hour & 0x7F), is_bcd) ||
		    !digits_ok(a.day, is_bcd) ||
		    !digits_ok(a.mon, is_bcd) ||
		    !digits_ok(a.year, is_bcd))
			continue;

		sec = rtc_decode(a.sec, is_bcd);
		min = rtc_decode(a.min, is_bcd);
		pm = (is_12h && (a.hour & 0x80)) ? 1 : 0;
		hour = rtc_decode((unsigned char)(a.hour & 0x7F), is_bcd);
		if (is_12h) {
			if (hour == 12)
				hour = pm ? 12 : 0;
			else if (pm)
				hour += 12;
		}
		day = rtc_decode(a.day, is_bcd);
		mon = rtc_decode(a.mon, is_bcd);
		yy = rtc_decode(a.year, is_bcd);
		year = expand_year(yy);

		if (sec > 59 || min > 59 || hour > 23 ||
		    mon < 1 || mon > 12 || yy > 99 ||
		    day < 1 || day > days_in_month(year, mon))
			continue;

		out->year = year;
		out->month = mon;
		out->day = day;
		out->hour = hour;
		out->minute = min;
		out->second = sec;
		out->wday = weekday_of(year, mon, day);
		return 0;
	}
	return -1;
}

void rtc_read_datetime_full(struct sys_datetime *out)
{
	if (!out)
		return;
	if (rtc_read_fields(out) == 0) {
		last_dt = *out;
		return;
	}
	*out = last_dt;
}

void rtc_read_datetime(char *buf)
{
	struct sys_datetime dt;
	unsigned int y, mo, d, h, m, s;

	rtc_read_datetime_full(&dt);
	y = dt.year;
	mo = dt.month;
	d = dt.day;
	h = dt.hour;
	m = dt.minute;
	s = dt.second;

	buf[0]  = (char)('0' + (y / 1000));
	buf[1]  = (char)('0' + ((y / 100) % 10));
	buf[2]  = (char)('0' + ((y / 10) % 10));
	buf[3]  = (char)('0' + (y % 10));
	buf[4]  = '-';
	buf[5]  = (char)('0' + (mo / 10));
	buf[6]  = (char)('0' + (mo % 10));
	buf[7]  = '-';
	buf[8]  = (char)('0' + (d / 10));
	buf[9]  = (char)('0' + (d % 10));
	buf[10] = ' ';
	buf[11] = (char)('0' + (h / 10));
	buf[12] = (char)('0' + (h % 10));
	buf[13] = ':';
	buf[14] = (char)('0' + (m / 10));
	buf[15] = (char)('0' + (m % 10));
	buf[16] = ':';
	buf[17] = (char)('0' + (s / 10));
	buf[18] = (char)('0' + (s % 10));
	buf[19] = '\0';
}

void rtc_read_hms(unsigned int *hour, unsigned int *minute, unsigned int *second)
{
	struct sys_datetime dt;

	rtc_read_datetime_full(&dt);
	if (hour)
		*hour = dt.hour;
	if (minute)
		*minute = dt.minute;
	if (second)
		*second = dt.second;
}

int rtc_set_datetime(const struct sys_datetime *in)
{
	unsigned int year, mon, day, hour, min, sec, wday, yy, century_reg;
	unsigned char regb, hour_raw;
	int is_bcd, is_12h;

	if (!in)
		return -1;
	year = in->year;
	mon = in->month;
	day = in->day;
	hour = in->hour;
	min = in->minute;
	sec = in->second;

	if (year < 1970 || year > 2069 ||
	    mon < 1 || mon > 12 ||
	    day < 1 || day > days_in_month(year, mon) ||
	    hour > 23 || min > 59 || sec > 59)
		return -1;

	/* Read register B before the SET sequence so the RTC's BCD/binary and
	 * 12/24-hour mode survive the write. */
	regb = cmos_read(RTC_REGB);
	if (rtc_wait_ready() != 0)
		return -1;
	is_bcd = !(regb & 0x04);
	is_12h = (regb & 0x02) == 0;

	if (is_12h) {
		unsigned int h12, pm = 0;

		if (hour == 0) {
			h12 = 12;
		} else if (hour < 12) {
			h12 = hour;
		} else if (hour == 12) {
			h12 = 12;
			pm = 1;
		} else {
			h12 = hour - 12;
			pm = 1;
		}
		hour_raw = (unsigned char)(rtc_encode(h12, is_bcd) |
					   (pm ? 0x80 : 0));
	} else {
		hour_raw = rtc_encode(hour, is_bcd);
	}

	wday = weekday_of(year, mon, day);
	yy = year % 100;
	/* The FADT points at the RTC century byte on ACPI machines.  Writing it
	 * keeps the two-digit year in the intended century for the firmware and
	 * any other OS; old non-ACPI boards do not get an arbitrary NVRAM write,
	 * and the kernel's 1970-2069 read window still gives 19xx years. */
	century_reg = acpi_rtc_century_reg();

	cmos_write(RTC_REGB, (unsigned char)(regb | 0x80));
	cmos_write(RTC_SEC, rtc_encode(sec, is_bcd));
	cmos_write(RTC_MIN, rtc_encode(min, is_bcd));
	cmos_write(RTC_HOUR, hour_raw);
	cmos_write(RTC_WDAY, rtc_encode(wday + 1, is_bcd));
	cmos_write(RTC_DAY, rtc_encode(day, is_bcd));
	cmos_write(RTC_MON, rtc_encode(mon, is_bcd));
	cmos_write(RTC_YEAR, rtc_encode(yy, is_bcd));
	if (century_reg)
		cmos_write((unsigned char)century_reg,
			   rtc_encode(year / 100, is_bcd));
	cmos_write(RTC_REGB, (unsigned char)(regb & 0x7F));

	last_dt.year = year;
	last_dt.month = mon;
	last_dt.day = day;
	last_dt.hour = hour;
	last_dt.minute = min;
	last_dt.second = sec;
	last_dt.wday = wday;
	return 0;
}
