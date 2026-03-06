/* display command: print resolution, color depth, and other graphics info. */

#include "io.h"
#include "vga.h"
#include "fbtext.h"

static void put_uint(unsigned int n)
{
	char buf[12];
	char *p = buf + sizeof(buf) - 1;
	*p = '\0';
	if (n == 0) {
		io_putchar('0');
		return;
	}
	while (n) {
		*--p = (char)('0' + (n % 10));
		n /= 10;
	}
	while (*p)
		io_putchar(*p++);
}

static void put_hex(unsigned int n)
{
	static const char hex[] = "0123456789ABCDEF";
	io_putstr("0x");
	if (n == 0) {
		io_putchar('0');
		return;
	}
	{
		char buf[9];
		char *q = buf + 8;
		*q = '\0';
		while (n) {
			*--q = hex[n & 0xF];
			n >>= 4;
		}
		while (*q)
			io_putchar(*q++);
	}
}

void program_display_main(void)
{
	if (vga_has_vesa()) {
		io_putstr("resolution ");
		put_uint(vga_fb_width());
		io_putstr(" x ");
		put_uint(vga_fb_height());
		io_putchar('\n');
		io_putstr("color depth ");
		put_uint(vga_fb_bpp());
		io_putstr(" bpp\n");
		io_putstr("pitch ");
		put_uint(vga_fb_pitch());
		io_putstr(" bytes/line\n");
		io_putstr("LFB ");
		put_hex((unsigned int)(unsigned long)vga_fb_ptr());
		io_putchar('\n');
		if (fbtext_active()) {
			io_putstr("text ");
			put_uint(fbtext_get_cols());
			io_putstr(" x ");
			put_uint(fbtext_get_rows());
			io_putstr(" chars\n");
		}
	} else {
		io_putstr("VESA not available; VGA text ");
		put_uint(io_get_cols());
		io_putstr(" x ");
		put_uint(io_get_rows());
		io_putstr(" chars\n");
	}
}
