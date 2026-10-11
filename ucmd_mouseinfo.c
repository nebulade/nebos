/* mouseinfo: ring-3 command.  Mouse hardware/protocol info. */

#include "ucmd_lib.h"

static void put_hex8(unsigned int x)
{
	static const char hex[] = "0123456789ABCDEF";

	u_putc(hex[(x >> 4) & 0xF]);
	u_putc(hex[x & 0xF]);
}

void _start(const char *arg)
{
	struct sys_mouseinfo m;

	(void)arg;
	if (u_syscall1(SYS_MOUSEINFO, (int)&m) != 1) {
		u_puts("mouse present no\n");
		u_exit(0);
	}

	u_puts("mouse present yes\n");
	if (m.ps2) {
		u_puts("mouse ps/2 id 0x");
		put_hex8(m.ps2_id & 0xFFu);
		u_putc('\n');
		u_puts("mouse protocol ");
		if ((m.ps2_id & 0xFFu) == 0x03)
			u_puts("intellimouse (wheel)\n");
		else if ((m.ps2_id & 0xFFu) == 0x04)
			u_puts("intellimouse explorer (5-button)\n");
		else
			u_puts("standard ps/2\n");
		u_puts("mouse packet bytes ");
		u_putdec(m.ps2_packet_size);
		u_putc('\n');
		u_puts("mouse streaming ");
		u_puts(m.ps2_streaming ? "on\n" : "off\n");
	}

	if (m.usb) {
		u_puts("mouse usb ");
		u_puts(m.hcd);
		u_puts(" hid boot\n");
		u_puts("mouse usb ctrl ");
		u_putdec(m.usb_ctrl);
		u_puts(" addr ");
		u_putdec(m.usb_addr);
		u_puts(" ep 0x");
		put_hex8(0x80u | m.usb_ep);
		u_putc('\n');
		u_puts("mouse usb mps ");
		u_putdec(m.usb_mps);
		u_puts(m.usb_low ? " (low speed)\n" : " (full speed)\n");
	}

	u_puts("mouse usb keyboard ");
	u_puts(m.usb_keyboard ? "yes\n" : "no\n");
	u_puts("mouse cursor ");
	u_puts(m.cursor_visible ? "visible\n" : "hidden\n");
	u_puts("mouse cursor backend ");
	u_puts(m.backend);
	u_putc('\n');
	u_exit(0);
}
