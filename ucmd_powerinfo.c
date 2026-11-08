/* powerinfo: AC adapter and battery state from the ThinkPad H8 EC.
 *
 * The EC access lives in the kernel (SYS_POWER_INFO); this command only
 * formats the snapshot.  The same syscall feeds the WM's top-bar percentage.
 */
#include "ucmd_lib.h"

static void put_milli(unsigned int v, const char *unit)
{
	u_putdec(v / 1000u);
	u_putc('.');
	u_putc((char)('0' + (v % 1000u) / 100u));
	u_puts(unit);
}

static const char *cap_unit(const struct sys_battery *b)
{
	if (!b->unit_known)
		return "EC units";
	return b->unit_mwh ? "mWh" : "mAh";
}

static void show_battery(unsigned int index, const struct sys_battery *b)
{
	unsigned int health;

	u_puts("battery ");
	u_putdec(index);
	u_puts(": ");
	if (!b->present) {
		if (b->error)
			u_puts("read error\n");
		else
			u_puts("not present\n");
		return;
	}
	u_puts("present, ");
	if (b->charging)
		u_puts("charging");
	else if (b->discharging)
		u_puts("discharging");
	else if (b->percent_valid && b->percent >= 99)
		u_puts("full");
	else
		u_puts("idle");
	u_puts(" (status 0x");
	u_put_hex2(b->status);
	u_putc(')');
	if (b->error)
		u_puts(", read error");
	u_putc('\n');

	u_puts("  charge: ");
	if (b->percent_valid) {
		u_putdec(b->percent);
		u_putc('%');
	} else {
		u_puts("unknown");
	}
	u_putc('\n');

	if (b->full && b->percent_valid) {
		u_puts("  capacity: ");
		u_putdec(b->remaining);
		u_puts(" / ");
		u_putdec(b->full);
		u_putc(' ');
		u_puts(cap_unit(b));
		u_putc('\n');
		if (b->design) {
			u_puts("  design: ");
			u_putdec(b->design);
			u_putc(' ');
			u_puts(cap_unit(b));
			health = (b->full * 100u + b->design / 2u) / b->design;
			u_puts(", health ");
			u_putdec(health);
			u_putc('%');
			u_putc('\n');
		}
	}
	if (b->voltage_mv) {
		u_puts("  voltage: ");
		put_milli(b->voltage_mv, " V");
		if (b->rate) {
			unsigned int r = b->rate < 0 ?
				(unsigned int)(-b->rate) : (unsigned int)b->rate;

			u_puts(", rate: ");
			if (b->unit_known)
				put_milli(r, b->unit_mwh ? " W" : " A");
			else {
				u_putdec(r);
				u_puts(" EC units");
			}
		}
		u_putc('\n');
	}
	if (b->manufacturer[0] || b->model[0]) {
		u_puts("  ");
		if (b->model[0]) {
			u_puts("model ");
			u_puts(b->model);
		}
		if (b->manufacturer[0]) {
			if (b->model[0])
				u_puts(", ");
			u_puts("manufacturer ");
			u_puts(b->manufacturer);
		}
		u_putc('\n');
	}
}

void _start(void)
{
	struct sys_powerinfo p;
	unsigned int i;

	if (u_power_info(&p, SYS_POWER_F_DETAIL) != 0) {
		u_puts("powerinfo: unavailable\n");
		u_exit(1);
	}
	if (!p.present) {
		u_puts("no ThinkPad embedded controller detected\n");
		u_puts("battery/AC status unavailable on this platform\n");
		u_exit(0);
	}

	u_puts("thinkpad");
	if (p.vendor[0]) {
		u_putc(' ');
		u_puts(p.vendor);
	}
	if (p.product[0]) {
		u_putc(' ');
		u_puts(p.product);
	}
	if (p.version[0]) {
		u_putc(' ');
		u_puts(p.version);
	}
	u_puts(", EC rev ");
	u_putdec(p.ec_rev);
	u_putc('\n');

	u_puts("ac adapter: ");
	if (p.ac_online < 0)
		u_puts("unknown\n");
	else if (p.ac_online)
		u_puts("online\n");
	else
		u_puts("offline\n");

	for (i = 0; i < SYS_POWER_BATT_MAX; i++)
		show_battery(i, &p.battery[i]);

	u_exit(0);
}
