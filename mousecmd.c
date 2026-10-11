/* mouseinfo command: print mouse hardware/protocol info and exit. */

#include "io.h"
#include "mouse.h"
#include "usb.h"
#include "usbhid.h"

static void put_uint(unsigned int n)
{
	char buf[12];
	int i = 10;
	buf[11] = '\0';
	if (n == 0) {
		io_putchar('0');
		return;
	}
	while (n && i >= 0) {
		buf[i--] = (char)('0' + (n % 10u));
		n /= 10u;
	}
	io_putstr(buf + i + 1);
}

static void put_hex8(unsigned int x)
{
	static const char hex[] = "0123456789ABCDEF";
	io_putchar(hex[(x >> 4) & 0xF]);
	io_putchar(hex[x & 0xF]);
}

void program_mouseinfo_main(void)
{
	io_putstr("mouse present ");
	io_putstr(mouse_is_present() ? "yes\n" : "no\n");
	if (!mouse_is_present())
		return;

	if (mouse_ps2_present()) {
		unsigned int id = mouse_device_id();

		io_putstr("mouse ps/2 id 0x");
		put_hex8(id & 0xFFu);
		io_putchar('\n');
		io_putstr("mouse protocol ");
		if (id == 0x03)
			io_putstr("intellimouse (wheel)\n");
		else if (id == 0x04)
			io_putstr("intellimouse explorer (5-button)\n");
		else
			io_putstr("standard ps/2\n");
		io_putstr("mouse packet bytes ");
		put_uint(mouse_packet_size());
		io_putchar('\n');
		io_putstr("mouse streaming ");
		io_putstr(mouse_streaming_enabled() ? "on\n" : "off\n");
	}

	if (usbhid_mouse_attached()) {
		unsigned int ctrl, addr, ep, mps;
		int low;

		usbhid_get_info(&ctrl, &addr, &ep, &mps, &low);
		io_putstr("mouse usb ");
		io_putstr(usb_hcd_name());
		io_putstr(" hid boot\n");
		io_putstr("mouse usb ctrl ");
		put_uint(ctrl);
		io_putstr(" addr ");
		put_uint(addr);
		io_putstr(" ep 0x");
		put_hex8(0x80u | ep);
		io_putchar('\n');
		io_putstr("mouse usb mps ");
		put_uint(mps);
		io_putstr(low ? " (low speed)\n" : " (full speed)\n");
	}

	io_putstr("mouse usb keyboard ");
	io_putstr(usbhid_keyboard_attached() ? "yes\n" : "no\n");

	io_putstr("mouse cursor ");
	io_putstr(mouse_cursor_visible() ? "visible\n" : "hidden\n");
}
