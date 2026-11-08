/* settime: Date & Time editor.
 *
 * A small ngui canvas window that reads the CMOS RTC through SYS_RTC_GET and
 * writes it through SYS_RTC_SET.  The RTC stores a two-digit year, so the
 * kernel expands it with a 1970-2069 window; this app accepts the same range
 * so a dead-battery default such as 1980 or 1990 can be corrected. */

#include "ucmd_lib.h"
#include "ngui.h"

#define ID_DATE 1
#define ID_TIME 2
#define ID_SET  3
#define ID_NOW  4

#define ACT_SET (1u << 0)
#define ACT_NOW (1u << 1)

static struct ngui ui;
static struct ngui_input date_in;
static struct ngui_input time_in;
static struct sys_datetime current;
static char status[64];
static int status_error;
static unsigned int status_until;
static unsigned int last_second;

static unsigned int s_len(const char *s)
{
	unsigned int n = 0;

	while (s && s[n])
		n++;
	return n;
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

static char *put_2(char *p, unsigned int v)
{
	*p++ = (char)('0' + (v / 10) % 10);
	*p++ = (char)('0' + v % 10);
	return p;
}

static char *put_date(char *p, const struct sys_datetime *dt)
{
	*p++ = (char)('0' + (dt->year / 1000) % 10);
	*p++ = (char)('0' + (dt->year / 100) % 10);
	*p++ = (char)('0' + (dt->year / 10) % 10);
	*p++ = (char)('0' + dt->year % 10);
	*p++ = '-';
	p = put_2(p, dt->month);
	*p++ = '-';
	p = put_2(p, dt->day);
	return p;
}

static char *put_time(char *p, const struct sys_datetime *dt)
{
	p = put_2(p, dt->hour);
	*p++ = ':';
	p = put_2(p, dt->minute);
	*p++ = ':';
	p = put_2(p, dt->second);
	return p;
}

static void set_status(const char *msg, int err)
{
	unsigned int i;

	for (i = 0; msg[i] && i + 1 < sizeof(status); i++)
		status[i] = msg[i];
	status[i] = '\0';
	status_error = err;
	status_until = u_ticks_ms() + 4000;
	ngui_invalidate(&ui);
}

static void load_current(int fill_fields)
{
	struct sys_datetime dt;
	char b[24];
	char *p;

	if (u_rtc_get(&dt) != 0) {
		set_status("Cannot read the RTC", 1);
		return;
	}
	current = dt;
	if (!fill_fields)
		return;
	p = put_date(b, &dt);
	*p = '\0';
	ngui_input_set(&date_in, b);
	p = put_time(b, &dt);
	*p = '\0';
	ngui_input_set(&time_in, b);
}

static int parse_num(const char *s, unsigned int n, unsigned int *out)
{
	unsigned int v = 0, i;

	if (!n)
		return -1;
	for (i = 0; i < n; i++) {
		if (s[i] < '0' || s[i] > '9')
			return -1;
		v = v * 10 + (unsigned int)(s[i] - '0');
	}
	*out = v;
	return 0;
}

static int parse_date(const char *s, unsigned int *y, unsigned int *mo,
		      unsigned int *d)
{
	if (s_len(s) != 10)
		return -1;
	if ((s[4] != '-' && s[4] != '/') ||
	    (s[7] != '-' && s[7] != '/'))
		return -1;
	if (parse_num(s, 4, y) != 0 ||
	    parse_num(s + 5, 2, mo) != 0 ||
	    parse_num(s + 8, 2, d) != 0)
		return -1;
	return 0;
}

static int parse_time(const char *s, unsigned int *h, unsigned int *mi,
		      unsigned int *sec)
{
	unsigned int len = s_len(s);

	if (len == 5) {
		*sec = 0;
	} else if (len == 8) {
		if (s[5] != ':' || parse_num(s + 6, 2, sec) != 0)
			return -1;
	} else {
		return -1;
	}
	if (s[2] != ':' ||
	    parse_num(s, 2, h) != 0 ||
	    parse_num(s + 3, 2, mi) != 0)
		return -1;
	return 0;
}

static void apply_now(void)
{
	struct sys_datetime dt;
	unsigned int y, mo, d, h, mi, s;

	if (parse_date(date_in.text, &y, &mo, &d) != 0) {
		set_status("Date must be YYYY-MM-DD", 1);
		return;
	}
	if (parse_time(time_in.text, &h, &mi, &s) != 0) {
		set_status("Time must be HH:MM:SS", 1);
		return;
	}
	if (y < 1970 || y > 2069) {
		set_status("Year must be 1970-2069", 1);
		return;
	}
	if (mo < 1 || mo > 12) {
		set_status("Month must be 1-12", 1);
		return;
	}
	if (d < 1 || d > days_in_month(y, mo)) {
		set_status("Day is not valid for that month", 1);
		return;
	}
	if (h > 23 || mi > 59 || s > 59) {
		set_status("Time is out of range", 1);
		return;
	}

	dt.year = y;
	dt.month = mo;
	dt.day = d;
	dt.hour = h;
	dt.minute = mi;
	dt.second = s;
	dt.wday = 0;             /* the kernel recomputes the weekday */
	if (u_rtc_set(&dt) != 0) {
		set_status("RTC write failed", 1);
		return;
	}
	load_current(1);
	set_status("Date and time updated", 0);
}

static unsigned int draw(void)
{
	struct ngui_rect body, r, date_r, time_r, set_r, now_r;
	unsigned int action = 0;
	char line[64];
	char *p;

	body.x = 0;
	body.y = 0;
	body.w = ui.w;
	body.h = ui.h;
	ngui_fill_rect(&ui, body, ui.theme.bg);
	ngui_inset(&body, 16);

	r = ngui_cut_top(&body, 24);
	ngui_text(&ui, r.x, r.y, "System date and time", ui.theme.text);

	p = line;
	{
		static const char prefix[] = "Current: ";
		unsigned int i;

		for (i = 0; prefix[i]; i++)
			*p++ = prefix[i];
	}
	p = put_date(p, &current);
	*p++ = ' ';
	p = put_time(p, &current);
	*p = '\0';
	r = ngui_cut_top(&body, 24);
	ngui_text(&ui, r.x, r.y, line, ui.theme.text_dim);

	r = ngui_cut_top(&body, 18);
	ngui_text(&ui, r.x, r.y, "Date (YYYY-MM-DD)", ui.theme.text_dim);
	date_r = ngui_cut_top(&body, 32);
	ngui_cut_top(&body, 8);
	r = ngui_cut_top(&body, 18);
	ngui_text(&ui, r.x, r.y, "Time (HH:MM:SS)", ui.theme.text_dim);
	time_r = ngui_cut_top(&body, 32);
	ngui_cut_top(&body, 8);

	if (ngui_text_input(&ui, ID_DATE, date_r, &date_in) &
	    NGUI_INPUT_SUBMIT)
		action |= ACT_SET;
	if (ngui_text_input(&ui, ID_TIME, time_r, &time_in) &
	    NGUI_INPUT_SUBMIT)
		action |= ACT_SET;

	r = ngui_cut_top(&body, 40);
	set_r = r;
	set_r.w = 96;
	now_r = r;
	now_r.x = r.x + 104;
	now_r.w = 96;
	if (ngui_button(&ui, ID_SET, set_r, "Set"))
		action |= ACT_SET;
	if (ngui_button(&ui, ID_NOW, now_r, "Now"))
		action |= ACT_NOW;

	r = ngui_cut_top(&body, 24);
	if (status[0] && status_until &&
	    (int)(u_ticks_ms() - status_until) < 0)
		ngui_text(&ui, r.x, r.y, status,
			  status_error ? ui.theme.error : ui.theme.accent);

	r = ngui_cut_top(&body, 20);
	ngui_text(&ui, r.x, r.y,
		  "RTC year range: 1970-2069.  Enter applies the change.",
		  ui.theme.text_dim);

	return action;
}

void _start(const char *arg)
{
	struct win_info wi;
	int quit = 0;

	(void)arg;
	if (u_win_create(460, 320, "Date & Time", &wi) != 0) {
		u_puts("settime: no graphics window\n");
		u_exit(1);
	}
	ngui_init(&ui, (unsigned short *)wi.fb, (int)wi.width, (int)wi.height);
	ngui_font_attach(&ui);
	date_in.placeholder = "YYYY-MM-DD";
	time_in.placeholder = "HH:MM:SS";

	if (u_rtc_get(&current) == 0) {
		char b[24];
		char *p;

		p = put_date(b, &current);
		*p = '\0';
		ngui_input_set(&date_in, b);
		p = put_time(b, &current);
		*p = '\0';
		ngui_input_set(&time_in, b);
	} else {
		set_status("Cannot read the RTC", 1);
	}

	for (;;) {
		unsigned int now = u_ticks_ms();
		unsigned int action = 0;

		if (status_until && (int)(now - status_until) >= 0) {
			status[0] = '\0';
			status_until = 0;
			status_error = 0;
			ngui_invalidate(&ui);
		}
		if (now - last_second >= 1000) {
			last_second = now;
			load_current(0);
			ngui_invalidate(&ui);
		}

		if (!ngui_begin(&ui)) {
			u_sleep_ms(10);
			continue;
		}
		action = draw();
		if (ui.focus == 0 && ngui_key_peek(&ui) == NGUI_KEY_ESC) {
			ngui_key(&ui);
			quit = 1;
		}
		ngui_end(&ui);
		if (action & ACT_SET)
			apply_now();
		if (action & ACT_NOW)
			load_current(1);
		if (quit)
			break;
		u_sleep_ms(10);
	}
	u_win_close();
	u_exit(0);
}
