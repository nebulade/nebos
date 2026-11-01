/* sensors: CPU DTS + IT87xx Super I/O temperatures and fan speeds.
 *
 * The DTS TjMax of the Atom 330 is not published; Intel only specifies
 * TJ = 90 C and Tcase-max = 85.2 C, while Linux coretemp uses 125 C for this
 * model.  This command prints the raw DTS delta and the resulting absolute
 * temperature for each common TjMax, plus the Super I/O's independent diode
 * readings and the fan tachometers.
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

void _start(void)
{
	struct sys_cputemp ct;
	struct sys_sensors s;
	unsigned int i;

	u_puts("dts ");
	if (u_cpu_temp(&ct) == 0 && ct.temp_c >= 0) {
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
			u_puts("\nscale 90: ");
			put_scale(90, ct.delta_c);
			u_puts(", 100: ");
			put_scale(100, ct.delta_c);
			u_puts(", 125: ");
			put_scale(125, ct.delta_c);
			u_putc('\n');
		}
		u_puts("cores ");
		for (i = 0; i < ct.ncpus && i < SYS_CPU_MAX; i++) {
			put_temp(ct.cpu_temp_c[i]);
			u_puts(" C");
			if (i + 1 < ct.ncpus && i + 1 < SYS_CPU_MAX)
				u_puts(", ");
		}
		u_putc('\n');
	} else {
		u_puts("unavailable (no Intel DTS)\n");
	}

	if (u_sensors(&s) != 0 || !s.present) {
		u_puts("it87 not found\n");
		u_exit(0);
	}
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
	u_exit(0);
}
