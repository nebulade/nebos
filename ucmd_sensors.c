/* sensors: CPU DTS plus the board's hardware monitor.
 *
 * Two board sources exist: the IT87xx Super I/O (AT3IONT-I, three
 * temperatures and fan tachometers) and the ThinkPad embedded controller
 * (X201, temperatures at EC 0x78..0x7F / 0xC0..0xC7 plus the fan tachometer).
 *
 * The DTS TjMax of the Atom 330 is not published; Intel only specifies
 * TJ = 90 C and Tcase-max = 85.2 C, while Linux coretemp uses 125 C for this
 * model.  This command prints the raw DTS delta and the resulting absolute
 * temperature for each common TjMax, plus the board's independent readings.
 */
#include "ucmd_lib.h"

static void put_temp(int t)
{
	if (t <= -50) {
		u_puts("--");
		return;
	}
	if (t < 0) {
		u_putc('-');
		u_putdec((unsigned int)(-t));
	} else {
		u_putdec((unsigned int)t);
	}
}

static void put_hex16(unsigned int v)
{
	static const char hex[] = "0123456789ABCDEF";

	u_putc(hex[(v >> 12) & 0xF]);
	u_putc(hex[(v >> 8) & 0xF]);
	u_putc(hex[(v >> 4) & 0xF]);
	u_putc(hex[v & 0xF]);
}

static void put_scale(int tjmax, int delta)
{
	int t = tjmax - delta;

	if (t < -20 || t > 150)
		u_puts("--");
	else {
		u_putdec((unsigned int)t);
		u_puts(" C");
	}
}

static void show_dts(void)
{
	struct sys_cputemp ct;
	unsigned int i;

	u_puts("dts ");
	if (u_cpu_temp(&ct) != 0 || ct.temp_c < 0) {
		u_puts("unavailable (no Intel DTS)\n");
		return;
	}
	u_puts("max ");
	put_temp(ct.temp_c);
	u_puts(" C (tjmax ");
	u_putdec((unsigned int)ct.tjmax_c);
	u_puts(" C)\n");
	if (ct.delta_c >= 0) {
		u_puts("this core delta ");
		u_putdec((unsigned int)ct.delta_c);
		u_puts(", status 0x");
		u_puthex(ct.status);
		u_putc('\n');
		/* The scales are for CPUs whose TjMax is not published (the Atom
		 * 330); when the MSR reports a real value such as 105 C for
		 * Arrandale there is nothing to guess. */
		if (ct.tjmax_c == 90 || ct.tjmax_c == 100 ||
		    ct.tjmax_c == 125) {
			u_puts("scale 90: ");
			put_scale(90, ct.delta_c);
			u_puts(", 100: ");
			put_scale(100, ct.delta_c);
			u_puts(", 125: ");
			put_scale(125, ct.delta_c);
			u_putc('\n');
		}
	}
	u_puts("cores ");
	for (i = 0; i < ct.ncpus && i < SYS_CPU_MAX; i++) {
		put_temp(ct.cpu_temp_c[i]);
		u_puts(" C");
		if (i + 1 < ct.ncpus && i + 1 < SYS_CPU_MAX)
			u_puts(", ");
	}
	u_putc('\n');
}

static int show_it87(void)
{
	struct sys_sensors s;
	unsigned int i;

	if (u_sensors(&s) != 0 || !s.present)
		return 0;
	u_puts("it87 chip 0x");
	put_hex16(s.chip_id);
	u_puts(" base 0x");
	put_hex16(s.base);
	u_puts(" enable 0x");
	u_put_hex2(s.temp_enable);
	u_puts(" extra 0x");
	u_put_hex2(s.temp_extra);
	u_putc('\n');
	for (i = 0; i < 3; i++) {
		int enabled = (s.temp_enable & (1u << i)) ||
			      (s.temp_enable & (1u << (i + 3))) ||
			      (i == 2 && (s.temp_enable & 0xC0) == 0xC0);

		u_puts("temp");
		u_putdec(i + 1);
		u_puts(" ");
		if (enabled)
			put_temp(s.temp_c[i]);
		else
			u_puts("--");
		u_puts(" C");
		if (i < 2)
			u_puts(", ");
	}
	u_putc('\n');
	for (i = 0; i < 3; i++) {
		u_puts("fan");
		u_putdec(i + 1);
		u_puts(" ");
		u_putdec(s.fan_rpm[i]);
		u_puts(" RPM");
		if (i < 2)
			u_puts(", ");
	}
	u_putc('\n');
	return 1;
}

/* EC 0x2F: bit 7 = automatic, bit 6 = disengaged/full speed (takes
 * precedence), bits 2:0 = manual level (0 stops the fan, 7 is the usual max).
 */
static void put_fan_level(unsigned int level)
{
	u_puts(" (0x");
	u_put_hex2(level);
	if (level & 0x40)
		u_puts(", full speed");
	else if (level & 0x80)
		u_puts(", auto");
	else if ((level & 0x3F) == 0)
		u_puts(", stopped");
	else {
		u_puts(", level ");
		u_putdec(level & 0x3F);
	}
	u_puts(")");
}

static int show_tpec(void)
{
	struct sys_tpec_sensors s;
	unsigned int i, shown = 0;

	if (u_tpec_sensors(&s) != 0 || !s.present)
		return 0;
	u_puts("thinkpad");
	if (s.vendor[0]) {
		u_puts(" ");
		u_puts(s.vendor);
	}
	if (s.product[0]) {
		u_puts(" ");
		u_puts(s.product);
	}
	if (s.version[0]) {
		u_puts(" ");
		u_puts(s.version);
	}
	u_putc('\n');
	for (i = 0; i < SYS_TPEC_TEMP_MAX; i++) {
		unsigned int reg = i < 8 ? 0x78 + i : 0xC0 + i - 8;

		if (s.temp_c[i] <= -50)
			continue;
		if (shown)
			u_puts(", ");
		u_puts("temp");
		u_putdec(i + 1);
		u_puts(" (0x");
		u_put_hex2(reg);
		u_puts(") ");
		put_temp(s.temp_c[i]);
		u_puts(" C");
		shown++;
	}
	if (!shown)
		u_puts("no EC temperature sensors");
	u_putc('\n');
	u_puts("fan ");
	u_putdec(s.fan_rpm);
	u_puts(" RPM");
	put_fan_level(s.fan_level);
	u_putc('\n');
	return 1;
}

void _start(void)
{
	int any;

	show_dts();
	any = show_it87();
	any |= show_tpec();
	if (!any)
		u_puts("no board sensors found\n");
	u_exit(0);
}
