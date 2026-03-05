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

void rtc_read_datetime(char *buf)
{
	unsigned char sec, min, hour, day, month, year;
	unsigned int s, m, h, d, mo, y;

	/* Wait for RTC update to finish. Timeout to avoid hang. */
	{
		unsigned int n = 0;
		while ((cmos_read(0x0A) & 0x80) && ++n < 10000)
			;
	}

	sec   = cmos_read(0x00);
	min   = cmos_read(0x02);
	hour  = cmos_read(0x04);
	day   = cmos_read(0x07);
	month = cmos_read(0x08);
	year  = cmos_read(0x09);

	s  = bcd_to_bin(sec);
	m  = bcd_to_bin(min);
	h  = bcd_to_bin(hour);
	d  = bcd_to_bin(day);
	mo = bcd_to_bin(month);
	y  = bcd_to_bin(year);
	y += 2000;
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
