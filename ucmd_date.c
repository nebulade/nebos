/* date: print the RTC date and time in the classic Linux format.
 *
 * No timezone database exists in NebOS, so the default output has no zone
 * suffix; the CMOS RTC is the system's wall clock.  A small +FORMAT subset
 * is supported for scripts and quick checks. */

#include "ucmd_lib.h"

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

static void put2(unsigned int v)
{
	u_putc((char)('0' + (v / 10) % 10));
	u_putc((char)('0' + v % 10));
}

static void put_year(unsigned int y)
{
	u_putc((char)('0' + (y / 1000) % 10));
	u_putc((char)('0' + (y / 100) % 10));
	u_putc((char)('0' + (y / 10) % 10));
	u_putc((char)('0' + y % 10));
}

static void put_e(unsigned int d)
{
	if (d < 10)
		u_putc(' ');
	else
		u_putc((char)('0' + d / 10));
	u_putc((char)('0' + d % 10));
}

static void format(const char *fmt, const struct sys_datetime *dt)
{
	for (; *fmt; fmt++) {
		char c;

		if (*fmt != '%') {
			u_putc(*fmt);
			continue;
		}
		fmt++;
		c = *fmt;
		if (!c)
			return;
		switch (c) {
		case 'Y':
			put_year(dt->year);
			break;
		case 'y':
			put2(dt->year % 100);
			break;
		case 'm':
			put2(dt->month);
			break;
		case 'd':
			put2(dt->day);
			break;
		case 'e':
			put_e(dt->day);
			break;
		case 'H':
			put2(dt->hour);
			break;
		case 'M':
			put2(dt->minute);
			break;
		case 'S':
			put2(dt->second);
			break;
		case 'a':
			u_puts(wday_abbr[dt->wday % 7]);
			break;
		case 'A':
			u_puts(wday_full[dt->wday % 7]);
			break;
		case 'b':
			u_puts(mon_abbr[(dt->month - 1) % 12]);
			break;
		case 'B':
			u_puts(mon_full[(dt->month - 1) % 12]);
			break;
		case 'p':
			u_puts(dt->hour < 12 ? "AM" : "PM");
			break;
		case 'F':
			format("%Y-%m-%d", dt);
			break;
		case 'T':
			format("%H:%M:%S", dt);
			break;
		case '%':
			u_putc('%');
			break;
		default:
			u_putc('%');
			u_putc(c);
			break;
		}
	}
}

void _start(const char *arg)
{
	struct sys_datetime dt;
	const char *fmt = "%a %b %e %H:%M:%S %Y";

	if (u_rtc_get(&dt) != 0) {
		u_puts("date: cannot read the RTC\n");
		u_exit(1);
	}

	if (arg) {
		while (*arg == ' ' || *arg == '\t')
			arg++;
		if (*arg == '+') {
			fmt = arg + 1;
		} else if (*arg) {
			u_puts("usage: date [+FORMAT]\n");
			u_exit(1);
		}
	}

	format(fmt, &dt);
	u_putc('\n');
	u_exit(0);
}
