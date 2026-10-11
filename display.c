/* display command: print resolution, color depth, and other graphics info. */

#include "io.h"
#include "vga.h"
#include "gpu.h"
#include "ati.h"
#include "nvidia.h"
#include "radeon.h"
#include "mouse.h"
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
	io_putstr("VRAM ");
	put_uint(vga_fb_vram_kb());
	io_putstr(" KB\n");
	io_putstr("LFB ");
	put_hex((unsigned int)(unsigned long)vga_fb_ptr());
	io_putchar('\n');
	io_putstr("LFB phys ");
	put_hex((unsigned int)vga_fb_lfb_addr());
	io_putchar('\n');
	io_putstr("rendering: ");
	io_putstr(vga_shadow_is_active() ? "shadow buffer" : "direct");
	io_putchar('\n');
	{
		unsigned int pci_id = vga_fb_pci_id();
		if (pci_id) {
			io_putstr("GPU ");
			io_putstr(gpu_vendor_name());
			io_putstr(" PCI ");
			put_hex(pci_id & 0xFFFF);
			io_putchar(':');
			put_hex(pci_id >> 16);
			io_putchar('\n');
		}
	}
	if (gpu_vendor() == GPU_VENDOR_ATI) {
		unsigned int method = ati_mmio_method();
		if (method) {
			io_putstr("ATI CRTC ");
			put_hex(ati_off_pitch_before());
			io_putstr(" -> ");
			put_hex(ati_off_pitch_after());
			io_putstr(" via ");
			if (method == 1)
				io_putstr("BAR2");
			else if (method == 2)
				io_putstr("LFB+8M");
			else if (method == 3)
				io_putstr("LFB+4M");
			else if (method == 4)
				io_putstr("IO");
			io_putchar('\n');
		} else if (ati_is_radeon()) {
			io_putstr("ATI Radeon (R200/RV280)");
			if (radeon_available())
				io_putstr(" hardware cursor");
			io_putchar('\n');
		}
	}
	if (gpu_vendor() == GPU_VENDOR_NVIDIA) {
		unsigned int lfb = (unsigned int)vga_fb_lfb_addr();
		unsigned int bar3 = nvidia_bar3();

		io_putstr("NVIDIA ");
		io_putstr(nvidia_chip_name());
		io_putstr(" chipset ");
		put_hex(nvidia_chipset());
		io_putstr(" regs ");
		put_hex(nvidia_bar0());
		io_putstr(" bar3 ");
		put_hex(bar3);
		if (bar3 && lfb >= bar3) {
			/* VRAM offset of the raster (BAR3 maps VRAM 1:1):
			   the base for offscreen VRAM (cursor image etc.). */
			io_putstr(" FB ofs ");
			put_hex(lfb - bar3);
		}
		io_putchar('\n');
	}
	io_putstr("cursor backend ");
	io_putstr(mouse_cursor_backend_name());
	io_putchar('\n');
	io_putstr("text ");
	put_uint(fbtext_get_cols());
	io_putstr(" x ");
	put_uint(fbtext_get_rows());
	io_putstr(" chars\n");
}
