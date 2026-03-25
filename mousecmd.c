/* mouseinfo command: print mouse hardware/protocol info and exit. */

#include "io.h"
#include "mouse.h"

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
	unsigned int id = mouse_device_id();

	io_putstr("mouse present ");
	io_putstr(mouse_is_present() ? "yes\n" : "no\n");
	if (!mouse_is_present())
		return;

	io_putstr("mouse id 0x");
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
	put_uint((id == 0x03 || id == 0x04) ? 4u : 3u);
	io_putchar('\n');
	io_putstr("mouse streaming ");
	io_putstr(mouse_streaming_enabled() ? "on\n" : "off\n");
	io_putstr("mouse cursor ");
	io_putstr(mouse_cursor_visible() ? "visible\n" : "hidden\n");
}
