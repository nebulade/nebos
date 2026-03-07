/* VGA: VESA LFB (from boot). */

#include "vga.h"
#include "pci.h"
#include "ati.h"

static unsigned int vga_pci_id;  /* vendor(lo16) | device(hi16) */

/* Cached boot framebuffer info (set once when vga_fb_* first used). */
static unsigned int vga_fb_w, vga_fb_h, vga_fb_pitch_val, vga_fb_bpp_val;
static unsigned int vga_fb_vram_kb_val;
static volatile unsigned char *vga_fb_lfb;
static int vga_fb_cached;

/*
 * Force CRTC Start Address to 0 and Line Compare to max (0x3FF).
 * Fixes real hardware where VBE mode set leaves stale CRTC state,
 * causing the display to appear rotated (bottom portion at top).
 */
static void vga_reset_crtc(void)
{
	unsigned char v;

	/* Start Address High/Low = 0 */
	port_outb(0x3D4, 0x0C);
	port_outb(0x3D5, 0x00);
	port_outb(0x3D4, 0x0D);
	port_outb(0x3D5, 0x00);

	/* Unlock CRTC registers 0-7: clear bit 7 of register 0x11 */
	port_outb(0x3D4, 0x11);
	v = port_inb(0x3D5);
	port_outb(0x3D5, v & 0x7F);

	/* Line Compare bits 7:0 = 0xFF (register 0x18) */
	port_outb(0x3D4, 0x18);
	port_outb(0x3D5, 0xFF);

	/* Line Compare bit 8 = 1 (register 0x07, bit 4) */
	port_outb(0x3D4, 0x07);
	v = port_inb(0x3D5);
	port_outb(0x3D5, v | 0x10);

	/* Line Compare bit 9 = 1 (register 0x09, bit 6) */
	port_outb(0x3D4, 0x09);
	v = port_inb(0x3D5);
	port_outb(0x3D5, v | 0x40);
}

/* Find PCI VGA device and cache its ID (for non-ATI cards). */
static void vga_find_pci_vga(void)
{
	unsigned int bus, dev, id, class_rev;

	if (vga_pci_id)
		return;
	for (bus = 0; bus < 4; bus++) {
		for (dev = 0; dev < 32; dev++) {
			id = pci_cfg_read32(bus, dev, 0, 0x00);
			if (id == 0xFFFFFFFF || id == 0)
				continue;
			class_rev = pci_cfg_read32(bus, dev, 0, 0x08);
			if ((class_rev >> 24) == 0x03) {
				vga_pci_id = id;
				return;
			}
		}
	}
}

static volatile struct vga_boot_fb_info *vga_get_boot_fb(void)
{
	volatile struct vga_boot_fb_info *p = VGA_BOOT_FB;
	__asm__ volatile ("" : "+r"(p));
	return p;
}

static void vga_cache_boot_fb(void)
{
	volatile struct vga_boot_fb_info *info;

	if (vga_fb_cached)
		return;
	vga_fb_cached = 1;
	info = vga_get_boot_fb();
	if (!info->valid) {
		vga_fb_w = vga_fb_h = vga_fb_pitch_val = vga_fb_bpp_val = 0;
		vga_fb_lfb = 0;
		return;
	}
	vga_fb_w = info->width;
	vga_fb_h = info->height;
	vga_fb_pitch_val = info->pitch;
	vga_fb_bpp_val = info->bpp;
	/* Some BIOSes (e.g. 1024x768) report pitch in pixels or too small; enforce minimum. */
	{
		unsigned int min_pitch = vga_fb_w;
		if (vga_fb_bpp_val == 16)
			min_pitch *= 2;
		else if (vga_fb_bpp_val == 24)
			min_pitch *= 3;
		else if (vga_fb_bpp_val == 32)
			min_pitch *= 4;
		if (vga_fb_pitch_val < min_pitch)
			vga_fb_pitch_val = min_pitch;
	}
	vga_fb_lfb = (volatile unsigned char *)(unsigned long)info->lfb_base;
	vga_fb_vram_kb_val = (unsigned int)info->vram_64k * 64;
	/* Clamp height to what fits in VRAM (prevents wrap on small VRAM cards) */
	if (vga_fb_vram_kb_val > 0 && vga_fb_pitch_val > 0) {
		unsigned int vram_bytes = vga_fb_vram_kb_val * 1024;
		unsigned int max_h = vram_bytes / vga_fb_pitch_val;
		if (max_h < vga_fb_h)
			vga_fb_h = max_h;
	}
	vga_reset_crtc();
	ati_init();
	vga_find_pci_vga();
	if (ati_pci_id())
		vga_pci_id = ati_pci_id();
}

/* ---- LFB API ---- */

unsigned int vga_fb_width(void)
{
	vga_cache_boot_fb();
	return vga_fb_w;
}

unsigned int vga_fb_height(void)
{
	vga_cache_boot_fb();
	return vga_fb_h;
}

unsigned int vga_fb_pitch(void)
{
	vga_cache_boot_fb();
	return vga_fb_pitch_val;
}

unsigned int vga_fb_bpp(void)
{
	vga_cache_boot_fb();
	return vga_fb_bpp_val;
}

unsigned int vga_fb_vram_kb(void)
{
	vga_cache_boot_fb();
	return vga_fb_vram_kb_val;
}

unsigned int vga_fb_pci_id(void)
{
	vga_cache_boot_fb();
	return vga_pci_id;
}

void *vga_fb_ptr(void)
{
	vga_cache_boot_fb();
	return (void *)vga_fb_lfb;
}

int vga_has_vesa(void)
{
	vga_cache_boot_fb();
	return (vga_fb_w != 0 && vga_fb_lfb != 0);
}

/* 16bpp: RGB 5:6:5.  32bpp: BGRX byte order (standard VBE / QEMU). */
static void vga_lfb_put_pixel(unsigned int x, unsigned int y, unsigned int r, unsigned int g, unsigned int b)
{
	unsigned int pitch = vga_fb_pitch_val;
	unsigned int bpp = vga_fb_bpp_val;
	unsigned char *p;

	if (x >= vga_fb_w || y >= vga_fb_h || !vga_fb_lfb)
		return;
	p = (unsigned char *)vga_fb_lfb + (unsigned long)y * pitch;
	if (bpp == 16) {
		unsigned int rgb16 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
		unsigned int off = x * 2;
		p[off] = (unsigned char)(rgb16 & 0xFF);
		p[off + 1] = (unsigned char)(rgb16 >> 8);
	} else if (bpp == 24) {
		unsigned int off = x * 3;
		p[off]     = (unsigned char)b;
		p[off + 1] = (unsigned char)g;
		p[off + 2] = (unsigned char)r;
	} else if (bpp == 32) {
		unsigned int off = x * 4;
		p[off]     = (unsigned char)b;
		p[off + 1] = (unsigned char)g;
		p[off + 2] = (unsigned char)r;
		p[off + 3] = 0;
	}
}

void vga_put_pixel_rgb(unsigned int x, unsigned int y, unsigned int r, unsigned int g, unsigned int b)
{
	vga_cache_boot_fb();
	if (!vga_fb_lfb)
		return;
	vga_lfb_put_pixel(x, y, r, g, b);
}

void vga_clear_rgb(unsigned int r, unsigned int g, unsigned int b)
{
	unsigned int pitch = vga_fb_pitch_val;
	unsigned int bpp = vga_fb_bpp_val;

	vga_cache_boot_fb();
	if (!vga_fb_lfb)
		return;
	if (bpp == 16) {
		unsigned int rgb16 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
		unsigned short val = (unsigned short)((rgb16 & 0xFF) | ((rgb16 >> 8) << 8));
		volatile unsigned short *fb = (volatile unsigned short *)vga_fb_lfb;
		unsigned int n = (pitch / 2) * vga_fb_h;
		unsigned int i;
		for (i = 0; i < n; i++)
			fb[i] = val;
	} else if (bpp == 24) {
		volatile unsigned char *p = vga_fb_lfb;
		unsigned int row, x;
		for (row = 0; row < vga_fb_h; row++) {
			unsigned int row_off = row * pitch;
			for (x = 0; x < vga_fb_w; x++) {
				unsigned int off = row_off + x * 3;
				p[off]     = (unsigned char)b;
				p[off + 1] = (unsigned char)g;
				p[off + 2] = (unsigned char)r;
			}
		}
	} else if (bpp == 32) {
		volatile unsigned int *fb = (volatile unsigned int *)vga_fb_lfb;
		unsigned int pixel = ((unsigned int)r << 16) | ((unsigned int)g << 8) | b;
		unsigned int stride = pitch / 4;
		unsigned int row, x;
		for (row = 0; row < vga_fb_h; row++)
			for (x = 0; x < vga_fb_w; x++)
				fb[row * stride + x] = pixel;
	}
}
