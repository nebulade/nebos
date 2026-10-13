/* VGA: VESA LFB (from boot) with a shadow buffer and a damage-driven flush. */

#include "vga.h"
#include "pci.h"
#include "gpu.h"
#include "ati.h"

/* Cached boot framebuffer info (set once when vga_fb_* first used). */
static unsigned int vga_fb_w, vga_fb_h, vga_fb_pitch_val, vga_fb_bpp_val;
static unsigned int vga_fb_vram_kb_val;
static volatile unsigned char *vga_fb_lfb;
static int vga_fb_cached;

/* Shadow buffer state. */
static unsigned char *vga_shadow_buf;
static unsigned int   vga_shadow_pitch;
static unsigned int   vga_shadow_size;

/* Dirty row bitmap: one bit per row. Max 2048 rows = 256 bytes. */
#define MAX_DIRTY_ROWS 2048
static unsigned char vga_shadow_dirty_rows[MAX_DIRTY_ROWS / 8];

/* Horizontal span [x0, x1) of dirty pixels, so the flush copies only the
   changed columns instead of whole rows. */
static unsigned int vga_dirty_x0, vga_dirty_x1;

static void mark_dirty_row(unsigned int y)
{
	if (y < MAX_DIRTY_ROWS)
		vga_shadow_dirty_rows[y / 8] |= (1 << (y % 8));
}

static void expand_dirty_x(unsigned int x0, unsigned int x1)
{
	if (x0 > vga_fb_w)
		x0 = vga_fb_w;
	if (x1 > vga_fb_w)
		x1 = vga_fb_w;
	if (x0 < vga_dirty_x0)
		vga_dirty_x0 = x0;
	if (x1 > vga_dirty_x1)
		vga_dirty_x1 = x1;
}

static void mark_dirty_rect_xy(unsigned int y, unsigned int h,
			       unsigned int x0, unsigned int x1)
{
	unsigned int i;
	for (i = 0; i < h; i++)
		mark_dirty_row(y + i);
	expand_dirty_x(x0, x1);
}

/* Full-width dirty rows (used where the x-extent isn't tracked). */
static void mark_dirty_rect(unsigned int y, unsigned int h)
{
	mark_dirty_rect_xy(y, h, 0, vga_fb_w);
}

static int any_dirty_rows(void)
{
	unsigned int i;
	unsigned int max_row = vga_fb_h;
	if (max_row > MAX_DIRTY_ROWS)
		max_row = MAX_DIRTY_ROWS;
	for (i = 0; i < (max_row + 7) / 8; i++)
		if (vga_shadow_dirty_rows[i])
			return 1;
	return 0;
}

static void clear_dirty_rows(void)
{
	unsigned int i;
	unsigned int max_row = vga_fb_h;
	if (max_row > MAX_DIRTY_ROWS)
		max_row = MAX_DIRTY_ROWS;
	for (i = 0; i < (max_row + 7) / 8; i++)
		vga_shadow_dirty_rows[i] = 0;
	vga_dirty_x0 = vga_fb_w;
	vga_dirty_x1 = 0;
}

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
		gpu_init(0);
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
	gpu_init(info->lfb_base);
	if (gpu_vendor() == GPU_VENDOR_ATI && !ati_is_radeon())
		vga_reset_crtc();
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
	return gpu_pci_id();
}

void *vga_fb_ptr(void)
{
	vga_cache_boot_fb();
	if (vga_shadow_buf)
		return (void *)vga_shadow_buf;
	return (void *)vga_fb_lfb;
}

unsigned long vga_fb_lfb_addr(void)
{
	vga_cache_boot_fb();
	return (unsigned long)vga_fb_lfb;
}

int vga_has_vesa(void)
{
	vga_cache_boot_fb();
	return (vga_fb_w != 0 && vga_fb_lfb != 0);
}

/* 16bpp: RGB 5:6:5.  32bpp: BGRX byte order (standard VBE / QEMU). */
static void shadow_put_pixel(unsigned int x, unsigned int y, unsigned int r, unsigned int g, unsigned int b)
{
	unsigned int pitch = vga_shadow_pitch;
	unsigned int bpp = vga_fb_bpp_val;
	unsigned char *p;

	if (x >= vga_fb_w || y >= vga_fb_h || !vga_shadow_buf)
		return;
	p = vga_shadow_buf + (unsigned long)y * pitch;
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
	mark_dirty_rect_xy(y, 1, x, x + 1);
}

void vga_put_pixel_rgb(unsigned int x, unsigned int y, unsigned int r, unsigned int g, unsigned int b)
{
	vga_cache_boot_fb();
	if (!vga_shadow_buf)
		return;
	shadow_put_pixel(x, y, r, g, b);
}

void vga_fill_rect(unsigned int x, unsigned int y, unsigned int w, unsigned int h,
		unsigned int r, unsigned int g, unsigned int b)
{
	unsigned int row, x1, y1;
	unsigned int pitch;
	unsigned int bpp;
	unsigned char *fb;

	vga_cache_boot_fb();
	pitch = vga_shadow_pitch;
	bpp = vga_fb_bpp_val;
	fb = vga_shadow_buf;

	if (!fb || w == 0 || h == 0)
		return;
	if (x >= vga_fb_w || y >= vga_fb_h)
		return;
	x1 = x + w;
	y1 = y + h;
	if (x1 > vga_fb_w) x1 = vga_fb_w;
	if (y1 > vga_fb_h) y1 = vga_fb_h;

	if (bpp == 16) {
		unsigned short val = (unsigned short)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
		for (row = y; row < y1; row++) {
			unsigned short *p = (unsigned short *)(fb + (unsigned long)row * pitch + x * 2);
			unsigned int xx;
			for (xx = x; xx < x1; xx++)
				*p++ = val;
		}
	} else if (bpp == 24) {
		for (row = y; row < y1; row++) {
			unsigned char *p = fb + (unsigned long)row * pitch + x * 3;
			unsigned int xx;
			for (xx = x; xx < x1; xx++) {
				p[0] = (unsigned char)b;
				p[1] = (unsigned char)g;
				p[2] = (unsigned char)r;
				p += 3;
			}
		}
	} else {
		unsigned int val = ((unsigned int)r << 16) | ((unsigned int)g << 8) | b;
		for (row = y; row < y1; row++) {
			unsigned int *p = (unsigned int *)(fb + (unsigned long)row * pitch + x * 4);
			unsigned int xx;
			for (xx = x; xx < x1; xx++)
				*p++ = val;
		}
	}
	mark_dirty_rect_xy(y, y1 - y, x, x1);
}

static unsigned int vga_pixel_bytes(void)
{
	if (vga_fb_bpp_val == 16)
		return 2;
	if (vga_fb_bpp_val == 24)
		return 3;
	return 4;
}

void vga_fb_read_rect(unsigned int x, unsigned int y, unsigned int w, unsigned int h,
		unsigned char *buf)
{
	unsigned int pitch, pb, row, col;
	unsigned char *fb;
	const unsigned char *src;

	vga_cache_boot_fb();
	if (!vga_shadow_buf || vga_fb_w == 0 || w == 0 || h == 0)
		return;
	pitch = vga_shadow_pitch;
	pb = vga_pixel_bytes();
	fb = vga_shadow_buf;
	for (row = 0; row < h; row++) {
		src = fb + (unsigned long)(y + row) * pitch + x * pb;
		for (col = 0; col < w * pb; col++)
			buf[row * w * pb + col] = src[col];
	}
}

void vga_fb_write_rect(unsigned int x, unsigned int y, unsigned int w, unsigned int h,
		const unsigned char *buf)
{
	unsigned int pitch, pb, row, col;
	unsigned char *dst;

	vga_cache_boot_fb();
	if (!vga_shadow_buf || vga_fb_w == 0 || w == 0 || h == 0)
		return;
	pitch = vga_shadow_pitch;
	pb = vga_pixel_bytes();
	for (row = 0; row < h; row++) {
		dst = vga_shadow_buf + (unsigned long)(y + row) * pitch + x * pb;
		for (col = 0; col < w * pb; col++)
			dst[col] = buf[row * w * pb + col];
	}
	{
		unsigned int xe = x + w;
		if (xe > vga_fb_w)
			xe = vga_fb_w;
		mark_dirty_rect_xy(y, h, x, xe);
	}
}

void vga_clear_rgb(unsigned int r, unsigned int g, unsigned int b)
{
	unsigned int pitch = vga_shadow_pitch;
	unsigned int bpp = vga_fb_bpp_val;

	vga_cache_boot_fb();
	if (!vga_shadow_buf)
		return;
	if (bpp == 16) {
		unsigned int rgb16 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
		unsigned short val = (unsigned short)((rgb16 & 0xFF) | ((rgb16 >> 8) << 8));
		unsigned short *fb = (unsigned short *)vga_shadow_buf;
		unsigned int n = (pitch / 2) * vga_fb_h;
		unsigned int i;
		for (i = 0; i < n; i++)
			fb[i] = val;
	} else if (bpp == 24) {
		unsigned char *p = vga_shadow_buf;
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
		unsigned int *fb = (unsigned int *)vga_shadow_buf;
		unsigned int pixel = ((unsigned int)r << 16) | ((unsigned int)g << 8) | b;
		unsigned int stride = pitch / 4;
		unsigned int row, x;
		for (row = 0; row < vga_fb_h; row++)
			for (x = 0; x < vga_fb_w; x++)
				fb[row * stride + x] = pixel;
	}
	mark_dirty_rect(0, vga_fb_h);
}

/* ---- Shadow buffer init and flush ---- */

/* Assembly row copy: rep movsl optimized for P5 USWC write-combining to UC memory. */
extern void vga_fb_copy_row(void *dst, void *src, unsigned int dwords);

void vga_shadow_init(void)
{
	vga_cache_boot_fb();
	if (!vga_fb_lfb || vga_fb_w == 0 || vga_fb_h == 0)
		return;

	vga_shadow_buf = (unsigned char *)VGA_SHADOW_BASE;
	vga_shadow_pitch = vga_fb_pitch_val;
	vga_shadow_size = vga_fb_h * vga_shadow_pitch;
	clear_dirty_rows();
}

/* Copy dirty rows from shadow to VRAM, at most about `max_bytes` this call
 * (0 = no limit).  A row is cleared as it is copied, so the next call resumes
 * with whatever is left: a large repaint is spread over several main-loop
 * iterations instead of stalling input behind it. */
static unsigned int shadow_copy_dirty_rows(unsigned int max_bytes)
{
	unsigned int row, bytes = 0;
	unsigned int pb = vga_pixel_bytes();
	unsigned int align = (pb == 2) ? 2 : (pb == 3) ? 4 : 1;
	unsigned int x0 = vga_dirty_x0;
	unsigned int x1 = vga_dirty_x1;
	unsigned int dwords;
	unsigned char *src = vga_shadow_buf;
	unsigned char *dst = (unsigned char *)vga_fb_lfb;
	unsigned int max_row = vga_fb_h;
	if (max_row > MAX_DIRTY_ROWS)
		max_row = MAX_DIRTY_ROWS;

	/* Clamp and dword-align the horizontal span so each row copy is an
	   integral number of dwords starting on a dword boundary. */
	if (x1 > vga_fb_w)
		x1 = vga_fb_w;
	x0 &= ~(align - 1);
	x1 = (x1 + align - 1) & ~(align - 1);
	if (x1 > vga_fb_w)
		x1 = vga_fb_w;
	if (x0 >= x1)
		return 0;
	dwords = ((x1 - x0) * pb) / 4;

	for (row = 0; row < max_row; row++) {
		if (vga_shadow_dirty_rows[row / 8] & (1 << (row % 8))) {
			vga_shadow_dirty_rows[row / 8] &= ~(1 << (row % 8));
			vga_fb_copy_row(
				(void *)(dst + (unsigned long)row * vga_fb_pitch_val + x0 * pb),
				(void *)(src + (unsigned long)row * vga_shadow_pitch + x0 * pb),
				dwords);
			bytes += dwords * 4;
			if (max_bytes && bytes >= max_bytes)
				break;
		}
	}
	return bytes;
}

/* Copy up to `max_bytes` of the dirty shadow rows to VRAM.  Called from the
 * main loop on every iteration: small updates (a cursor move is ~1 KB) reach
 * the screen within one loop instead of waiting for a fixed 30 Hz tick, while
 * a full-screen repaint is chunked so it cannot stall input. */
void vga_fb_flush_budget(unsigned int max_bytes)
{
	vga_cache_boot_fb();
	if (!vga_shadow_buf || !vga_fb_lfb)
		return;
	if (!any_dirty_rows())
		return;
	if (shadow_copy_dirty_rows(max_bytes) == 0) {
		clear_dirty_rows();          /* degenerate span: drop the marks */
		return;
	}
	if (!any_dirty_rows())
		clear_dirty_rows();          /* also resets the dirty x-span */
}

void vga_fb_flush(void)
{
	vga_fb_flush_budget(0);
}

int vga_shadow_is_active(void)
{
	return vga_shadow_buf != 0;
}

void vga_fb_mark_dirty_rect(unsigned int y, unsigned int h)
{
	mark_dirty_rect(y, h);
}

/* Mark rows dirty, limited to the horizontal span [x0, x1). */
void vga_fb_mark_dirty_rect_xy(unsigned int y, unsigned int h,
			       unsigned int x0, unsigned int x1)
{
	mark_dirty_rect_xy(y, h, x0, x1);
}

void vga_fb_clear_dirty(void)
{
	clear_dirty_rows();
}
