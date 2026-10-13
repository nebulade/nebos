/* RTC (CMOS) date/time read. */

#include "rtc.h"

#define CMOS_ADDR  0x70
#define CMOS_DATA  0x71

static inline unsigned char cmos_read(unsigned char reg)
{
	unsigned char v;
	/* Bit 7 on 0x70 disables NMI during access. */
	__asm__ volatile ("outb %b1, %w0" : : "Nd"(CMOS_ADDR), "a"((unsigned char)(reg | 0x80)));
	__asm__ volatile ("inb %w1, %0" : "=a"(v) : "Nd"(CMOS_DATA));
	return v;
}

static unsigned int bcd_to_bin(unsigned char bcd)
{
	return (unsigned int)((bcd >> 4) * 10 + (bcd & 0x0F));
}

static int bcd_digits(unsigned char bcd)
{
	return (bcd & 0x0F) <= 9 && ((bcd >> 4) & 0x0F) <= 9;
}

/* Read hour/minute/second out of the CMOS.  A read that straddles the RTC's
 * update cycle can tear (invalid BCD, or a value out of range), which used to
 * feed nonsense into callers - retry a few times and fall back to the last
 * values that validated.  Handles the 12-hour PM flag (hour bit 7). */
static void rtc_read_time(unsigned int *hour, unsigned int *minute,
			  unsigned int *second)
{
	static unsigned int last_h, last_m, last_s;
	unsigned int tries;

	for (tries = 0; tries < 4; tries++) {
		unsigned int n = 0;
		unsigned char s, m, h;
		unsigned int sv, mv, hv;

		while ((cmos_read(0x0A) & 0x80) && ++n < 10000)
			;

		s = cmos_read(0x00);
		m = cmos_read(0x02);
		h = (unsigned char)(cmos_read(0x04) & 0x7F);   /* drop the PM flag */

		if (!bcd_digits(s) || !bcd_digits(m) || !bcd_digits(h))
			continue;

		sv = bcd_to_bin(s);
		mv = bcd_to_bin(m);
		hv = bcd_to_bin(h);
		if (sv > 59 || mv > 59 || hv > 23)
			continue;

		last_h = hv;
		last_m = mv;
		last_s = sv;
		break;
	}

	*hour = last_h;
	*minute = last_m;
	*second = last_s;
}

void rtc_read_datetime(char *buf)
{
	static unsigned int last_d = 1, last_mo = 1, last_y = 2000;
	unsigned int s, m, h, d, mo, y;
	unsigned char dd, mm, yy;

	rtc_read_time(&h, &m, &s);

	/* Date registers (a torn read keeps the last good values). */
	{
		unsigned int n = 0;

		while ((cmos_read(0x0A) & 0x80) && ++n < 10000)
			;
	}
	dd = cmos_read(0x07);
	mm = cmos_read(0x08);
	yy = cmos_read(0x09);
	if (bcd_digits(dd) && bcd_to_bin(dd) >= 1 && bcd_to_bin(dd) <= 31)
		last_d = bcd_to_bin(dd);
	if (bcd_digits(mm) && bcd_to_bin(mm) >= 1 && bcd_to_bin(mm) <= 12)
		last_mo = bcd_to_bin(mm);
	if (bcd_digits(yy))
		last_y = 2000 + bcd_to_bin(yy);

	d = last_d;
	mo = last_mo;
	y = last_y;
	if (y < 2000)
		y += 100;

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
	rtc_read_time(hour, minute, second);
}
