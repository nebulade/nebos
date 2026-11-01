/* VGA: VESA LFB (from boot) with a shadow buffer and a damage-rect flush. */

#include "vga.h"
#include "blit.h"
#include "pci.h"
#include "gpu.h"
#include "ati.h"
#include "cpu.h"
#include "mtrr.h"
#include "nvidia.h"
#include "paging.h"
#include "process.h"
#include "serial.h"

/* Cached boot framebuffer info (set once when vga_fb_* first used). */
static unsigned int vga_fb_w, vga_fb_h, vga_fb_pitch_val, vga_fb_bpp_val;
static unsigned int vga_fb_vram_kb_val;
static volatile unsigned char *vga_fb_lfb;
static int vga_fb_cached;

/* Shadow buffer state. */
static unsigned char *vga_shadow_buf;
static unsigned int   vga_shadow_pitch;
static unsigned int   vga_shadow_size;

/* Damage rect list.  Each entry is a half-open [x0,x1) x [y0,y1) region of
 * the shadow buffer that has not been copied to VRAM yet.  Entries merge when
 * they overlap, touch or would barely waste space, so related marks (the
 * per-pixel writes of one glyph or cursor draw, the rectangles of one window
 * repaint) collapse into a few rects, while unrelated damage (a window and the
 * cursor on the other side of the screen) stays separate and the flush copies
 * each row only over the columns that changed.  A rect that is only partially
 * flushed keeps its remaining rows and moves behind the newer damage. */
#define DAMAGE_SLOTS 16
struct damage_rect {
	unsigned int x0, y0, x1, y1;
};
static struct damage_rect damage_rects[DAMAGE_SLOTS];
static unsigned int damage_count;

/* Set while a process is inside shadow_copy_damage(): the flush walks and
 * trims the shared damage list, so it must not be entered twice and the
 * scheduler must not switch away from it halfway.  Marks made while a flush
 * is active are deferred to one full-screen rect. */
static int present_busy;
static int damage_deferred;

static inline unsigned int irq_save_disable(void)
{
	unsigned int flags;

	__asm__ volatile ("pushfl; popl %0; cli" : "=r"(flags));
	return flags;
}

static inline void irq_restore(unsigned int flags)
{
	__asm__ volatile ("pushl %0; popfl" :: "r"(flags));
}

static void damage_add_locked(unsigned int x0, unsigned int y0,
			      unsigned int x1, unsigned int y1)
{
	unsigned int i;

	if (x1 > vga_fb_w)
		x1 = vga_fb_w;
	if (y1 > vga_fb_h)
		y1 = vga_fb_h;
	if (x0 >= x1 || y0 >= y1)
		return;

	/* Already covered?  Scanning backwards catches the repeated per-pixel
	   marks of a glyph or cursor draw in one step. */
	for (i = damage_count; i-- > 0; ) {
		struct damage_rect *d = &damage_rects[i];

		if (x0 >= d->x0 && y0 >= d->y0 && x1 <= d->x1 && y1 <= d->y1)
			return;
	}

	/* Merge with every rect this one touches; a merge can bring another
	   rect into reach, so restart the scan after each one. */
	for (i = 0; i < damage_count; ) {
		struct damage_rect *d = &damage_rects[i];
		unsigned int nx0 = (x0 < d->x0) ? x0 : d->x0;
		unsigned int ny0 = (y0 < d->y0) ? y0 : d->y0;
		unsigned int nx1 = (x1 > d->x1) ? x1 : d->x1;
		unsigned int ny1 = (y1 > d->y1) ? y1 : d->y1;
		unsigned int merged_area = (nx1 - nx0) * (ny1 - ny0);
		unsigned int area = (d->x1 - d->x0) * (d->y1 - d->y0) +
				    (x1 - x0) * (y1 - y0);

		if (nx0 <= d->x1 && d->x0 <= nx1 &&
		    ny0 <= d->y1 && d->y0 <= ny1 &&
		    merged_area <= 2 * area) {
			x0 = nx0;
			y0 = ny0;
			x1 = nx1;
			y1 = ny1;
			damage_rects[i] = damage_rects[--damage_count];
			i = 0;
			continue;
		}
		i++;
	}

	if (damage_count < DAMAGE_SLOTS) {
		damage_rects[damage_count].x0 = x0;
		damage_rects[damage_count].y0 = y0;
		damage_rects[damage_count].x1 = x1;
		damage_rects[damage_count].y1 = y1;
		damage_count++;
		return;
	}

	/* List full: extend whichever rect grows the least (correct, just a
	   few redundant pixels copied). */
	{
		unsigned int best = 0, best_grow = 0xFFFFFFFFu;

		for (i = 0; i < DAMAGE_SLOTS; i++) {
			struct damage_rect *d = &damage_rects[i];
			unsigned int nx0 = (x0 < d->x0) ? x0 : d->x0;
			unsigned int ny0 = (y0 < d->y0) ? y0 : d->y0;
			unsigned int nx1 = (x1 > d->x1) ? x1 : d->x1;
			unsigned int ny1 = (y1 > d->y1) ? y1 : d->y1;
			unsigned int grow = (nx1 - nx0) * (ny1 - ny0) -
					    (d->x1 - d->x0) * (d->y1 - d->y0);

			if (grow < best_grow) {
				best_grow = grow;
				best = i;
			}
		}
		{
			struct damage_rect *d = &damage_rects[best];

			if (x0 < d->x0) d->x0 = x0;
			if (y0 < d->y0) d->y0 = y0;
			if (x1 > d->x1) d->x1 = x1;
			if (y1 > d->y1) d->y1 = y1;
		}
	}
}

static void damage_add(unsigned int x0, unsigned int y0,
		       unsigned int x1, unsigned int y1)
{
	unsigned int flags = irq_save_disable();

	if (present_busy) {
		/* The flush owns the list right now.  Re-mark the whole screen
		 * once it finishes so the new pixels are not lost. */
		damage_deferred = 1;
		irq_restore(flags);
		return;
	}
	damage_add_locked(x0, y0, x1, y1);
	irq_restore(flags);
}

static int damage_any(void)
{
	return damage_count != 0;
}

static void damage_clear(void)
{
	damage_count = 0;
	damage_deferred = 0;
}

static void mark_dirty_rect_xy(unsigned int y, unsigned int h,
			       unsigned int x0, unsigned int x1)
{
	damage_add(x0, y, x1, y + h);
}

/* Full-width dirty rows (used where the x-extent isn't tracked). */
static void mark_dirty_rect(unsigned int y, unsigned int h)
{
	damage_add(0, y, vga_fb_w, y + h);
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

/* ---- Surfaces and the render target ---- */

/* The screen surface: the shadow buffer (or the LFB when there is none).
 * Kept in sync by vga_shadow_init() invalidating it. */
static struct surface screen_surf;

/* 1 when the flush writes VRAM through a write-combining alias. */
static int lfb_is_wc;

struct surface *vga_screen_surface(void)
{
	vga_cache_boot_fb();
	if (!screen_surf.px) {
		screen_surf.w = vga_fb_w;
		screen_surf.h = vga_fb_h;
		screen_surf.pitch = vga_shadow_buf ? vga_shadow_pitch
						   : vga_fb_pitch_val;
		screen_surf.bpp = vga_fb_bpp_val;
		screen_surf.alpha = 255;
		screen_surf.px = vga_shadow_buf ? vga_shadow_buf
						: (unsigned char *)vga_fb_lfb;
	}
	return &screen_surf;
}

struct surface *vga_target(void)
{
	struct surface *t = process_render_target();

	return t ? t : vga_screen_surface();
}

void vga_set_target(struct surface *s)
{
	process_set_render_target(s);
}

int vga_target_is_screen(void)
{
	return vga_target() == vga_screen_surface();
}

void vga_target_damage(unsigned int x, unsigned int y, unsigned int w,
		       unsigned int h)
{
	if (w == 0 || h == 0 || !vga_target_is_screen())
		return;
	mark_dirty_rect_xy(y, h, x, x + w);
}

/* 16bpp: RGB 5:6:5.  32bpp: BGRX byte order (standard VBE / QEMU). */
static void target_put_pixel(unsigned int x, unsigned int y, unsigned int r, unsigned int g, unsigned int b)
{
	struct surface *t = vga_target();
	unsigned int pitch, bpp;
	unsigned char *p;

	if (!t->px || x >= t->w || y >= t->h)
		return;
	pitch = t->pitch;
	bpp = t->bpp;
	p = t->px + (unsigned long)y * pitch;
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
		p[off + 3] = t->alpha;
	}
	vga_target_damage(x, y, 1, 1);
}

void vga_put_pixel_rgb(unsigned int x, unsigned int y, unsigned int r, unsigned int g, unsigned int b)
{
	vga_cache_boot_fb();
	target_put_pixel(x, y, r, g, b);
}

void vga_fill_rect(unsigned int x, unsigned int y, unsigned int w, unsigned int h,
		unsigned int r, unsigned int g, unsigned int b)
{
	vga_cache_boot_fb();
	if (!blit || !blit->fill)
		return;
	blit->fill(x, y, w, h, ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF));
}

static unsigned int vga_pixel_bytes(void)
{
	if (vga_fb_bpp_val == 16)
		return 2;
	if (vga_fb_bpp_val == 24)
		return 3;
	return 4;
}

/* The software cursor saves and restores pixels of the *screen*: these two
 * keep using the shadow buffer directly, never the render target. */
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
	if (pb == 4) {
		for (row = 0; row < h; row++) {
			const unsigned int *src = (const unsigned int *)
				(fb + (unsigned long)(y + row) * pitch + x * 4);
			unsigned int *dst = (unsigned int *)
				(buf + (unsigned long)row * w * 4);
			for (col = 0; col < w; col++)
				dst[col] = src[col];
		}
		return;
	}
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
	if (pb == 4) {
		for (row = 0; row < h; row++) {
			unsigned int *dst = (unsigned int *)
				(vga_shadow_buf + (unsigned long)(y + row) * pitch + x * 4);
			const unsigned int *src = (const unsigned int *)
				(buf + (unsigned long)row * w * 4);
			for (col = 0; col < w; col++)
				dst[col] = src[col];
		}
		goto mark;
	}
	for (row = 0; row < h; row++) {
		dst = vga_shadow_buf + (unsigned long)(y + row) * pitch + x * pb;
		for (col = 0; col < w * pb; col++)
			dst[col] = buf[row * w * pb + col];
	}
mark:
	{
		unsigned int xe = x + w;
		if (xe > vga_fb_w)
			xe = vga_fb_w;
		mark_dirty_rect_xy(y, h, x, xe);
	}
}

void vga_clear_rgb(unsigned int r, unsigned int g, unsigned int b)
{
	struct surface *t;

	vga_cache_boot_fb();
	t = vga_target();
	if (!t->px)
		return;
	vga_fill_rect(0, 0, t->w, t->h, r, g, b);
}

/* ---- Shadow buffer init and flush ---- */

void vga_shadow_init(void)
{
	vga_cache_boot_fb();
	if (!vga_fb_lfb || vga_fb_w == 0 || vga_fb_h == 0)
		return;

	/* The GPU probe (inside vga_cache_boot_fb) maps its BARs -- including the
	 * NVIDIA VRAM aperture, which covers the VBE LFB -- uncacheable, so the
	 * framebuffer mapping is re-applied once everything is up. */
	paging_lfb_refresh();

	/* On ION-class hardware the framebuffer is stolen DRAM and the VBE LFB
	 * is just a BAR3 window into it.  The BIOS leaves a 512 MB uncacheable
	 * MTRR over the PCIe apertures (and the lowest-numbered matching entry
	 * wins), so the aperture can never be write-combining: every dword is a
	 * separate transaction (~33 MB/s).  Write through the DRAM alias
	 * instead, with a WC MTRR over the stolen range -- and only switch once
	 * that entry is in place, otherwise the alias would be cached and the
	 * screen would not update. */
	if (gpu_vendor() == GPU_VENDOR_NVIDIA) {
		unsigned int alias = nvidia_lfb_in_vram();

		if (alias) {
			unsigned int wb, ws;

			if (mtrr_set_wc(nvidia_vram_base(),
					nvidia_vram_size(), &wb, &ws)) {
				vga_fb_lfb = (volatile unsigned char *)alias;
				lfb_is_wc = 1;
				serial_puts(SERIAL_COM1,
					"vga: LFB via stolen VRAM (write-combining)\n");
			} else {
				serial_puts(SERIAL_COM1,
					"vga: LFB alias found, no WC entry; staying on BAR3\n");
			}
		} else {
			serial_puts(SERIAL_COM1,
				"vga: LFB has no stolen-VRAM alias; using BAR3\n");
		}
	}

	vga_shadow_buf = (unsigned char *)VGA_SHADOW_BASE;
	vga_shadow_pitch = vga_fb_pitch_val;
	vga_shadow_size = vga_fb_h * vga_shadow_pitch;
	screen_surf.px = 0;       /* re-derive from the shadow buffer */
	damage_clear();

	serial_puts(SERIAL_COM1, "blit backend: ");
	serial_puts(SERIAL_COM1, blit ? blit->name : "none");
	serial_puts(SERIAL_COM1, "\r\n");
}

/* Copy damage rects from the shadow to VRAM, at most about `max_bytes` this
 * call (0 = no limit), and drop the rects that are done.  A rect that fits
 * the budget is copied whole even when that overshoots a little, so a
 * window-sized repaint is never split across iterations; only a rect larger
 * than the budget (a full-screen repaint) is chunked, and its remaining rows
 * move to the back of the list so newer, smaller damage is flushed first. */
static unsigned int shadow_copy_damage(unsigned int max_bytes)
{
	unsigned int i = 0, bytes = 0;
	int use_sse2 = cpu_has_sse2();
	unsigned int pb = vga_pixel_bytes();
	unsigned int align = (pb == 2) ? 2 : (pb == 3) ? 4 : 1;
	unsigned char *src = vga_shadow_buf;
	unsigned char *dst = (unsigned char *)vga_fb_lfb;

	while (i < damage_count) {
		struct damage_rect *d = &damage_rects[i];
		unsigned int rx0 = d->x0 & ~(align - 1);
		unsigned int rx1 = (d->x1 + align - 1) & ~(align - 1);
		unsigned int row_bytes, rect_bytes, dwords, tail, y;

		if (rx1 > vga_fb_w)
			rx1 = vga_fb_w;
		if (rx0 >= rx1) {
			damage_rects[i] = damage_rects[--damage_count];
			continue;
		}
		row_bytes = (rx1 - rx0) * pb;
		rect_bytes = row_bytes * (d->y1 - d->y0);
		dwords = row_bytes / 4;
		tail = row_bytes % 4;

		/* The budget is spent and this rect is bigger than a whole
		   call: leave it (rows already copied keep their progress). */
		if (max_bytes && bytes > 0 && rect_bytes > max_bytes)
			break;

		for (y = d->y0; y < d->y1; y++) {
			unsigned char *s = src + (unsigned long)y * vga_shadow_pitch + rx0 * pb;
			unsigned char *t = dst + (unsigned long)y * vga_fb_pitch_val + rx0 * pb;

			/* Streaming stores: VRAM is WC (or UC on machines where the
			 * MTRR cannot cover it) and rep movsl does not combine
			 * there, while movntdq does. */
			if (use_sse2)
				vga_fb_copy_row_nt(t, s, dwords);
			else
				vga_fb_copy_row(t, s, dwords);
			if (tail) {
				unsigned int k;
				for (k = dwords * 4; k < row_bytes; k++)
					t[k] = s[k];
			}
			bytes += row_bytes;
			if (max_bytes && rect_bytes > max_bytes && bytes >= max_bytes) {
				d->y0 = y + 1;
				break;
			}
		}
		if (y < d->y1) {
			/* Chunked: keep the remaining rows, behind the rest. */
			struct damage_rect rest = *d;

			damage_rects[i] = damage_rects[--damage_count];
			damage_rects[damage_count++] = rest;
			return bytes;
		}
		damage_rects[i] = damage_rects[--damage_count];
		if (use_sse2)
			__asm__ volatile ("sfence" ::: "memory");
		if (max_bytes && bytes >= max_bytes)
			break;
	}
	return bytes;
}

/* Bytes copied by the presenter so far (see vga.h). */
unsigned int vga_present_copied;

int vga_present_busy(void)
{
	return present_busy;
}

/* Copy up to `max_bytes` of the pending damage to VRAM.  Called by the main
 * loop on every iteration: small updates (a cursor move is ~1 KB) reach the
 * screen within one loop instead of waiting for a fixed 30 Hz tick, while a
 * full-screen repaint is chunked so it cannot stall input. */
void vga_present_shadow(unsigned int max_bytes)
{
	unsigned int flags;

	vga_cache_boot_fb();
	if (!vga_shadow_buf || !vga_fb_lfb)
		return;
	if (!damage_any())
		return;
	flags = irq_save_disable();
	if (present_busy) {
		/* Another process is mid-flush; its remaining damage stays
		 * queued and is copied by whoever flushes next. */
		irq_restore(flags);
		return;
	}
	present_busy = 1;
	irq_restore(flags);
	vga_present_copied += shadow_copy_damage(max_bytes);
	flags = irq_save_disable();
	present_busy = 0;
	if (damage_deferred) {
		damage_deferred = 0;
		damage_add_locked(0, 0, vga_fb_w, vga_fb_h);
	}
	irq_restore(flags);
}

/* Make drawn content visible: the backend's present() (the software one copies
 * the pending damage rects to VRAM). */
void vga_fb_flush_budget(unsigned int max_bytes)
{
	vga_cache_boot_fb();
	if (!blit || !blit->present)
		return;
	blit->present(max_bytes);
}

void vga_fb_flush(void)
{
	vga_fb_flush_budget(0);
}

int vga_fb_lfb_wc(void)
{
	return lfb_is_wc;
}

int vga_shadow_is_active(void)
{
	return vga_shadow_buf != 0;
}

void vga_fb_mark_dirty_rect(unsigned int y, unsigned int h)
{
	vga_cache_boot_fb();
	mark_dirty_rect(y, h);
}

/* Mark a rectangle dirty, limited to the horizontal span [x0, x1). */
void vga_fb_mark_dirty_rect_xy(unsigned int y, unsigned int h,
			       unsigned int x0, unsigned int x1)
{
	vga_cache_boot_fb();
	mark_dirty_rect_xy(y, h, x0, x1);
}

void vga_fb_clear_dirty(void)
{
	damage_clear();
}
