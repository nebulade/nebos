/* VGA: VESA LFB (from boot). Shadow buffer; the main loop copies pending
 * damage rectangles to VRAM on every iteration (byte budget, no fixed frame
 * rate). */
#ifndef VGA_H
#define VGA_H

/* Boot video info at 0x600 (written by boot.S in safe conventional memory).
   Layout: valid(1) width(2) height(2) pitch(2) bpp(1) lfb_base(4) vram_64k(2) = 14 bytes. */
struct vga_boot_fb_info {
	unsigned char  valid;
	unsigned short width;
	unsigned short height;
	unsigned short pitch;
	unsigned char  bpp;
	unsigned int   lfb_base;
	unsigned short vram_64k;   /* total VRAM in 64KB blocks (from VBE) */
} __attribute__((packed));

#define VGA_BOOT_FB ((volatile struct vga_boot_fb_info *)0x600)

/* Shadow buffer lives at 1MB — safely above kernel, stacks, DMA, and program load area. */
#define VGA_SHADOW_BASE 0x100000

/* LFB API. RGB 0..255. All drawing goes into the shadow buffer; the main loop
 * flushes dirty areas to VRAM on every iteration (byte-budgeted).  These
 * describe the *screen*, not the current render target (see below). */
unsigned int vga_fb_width(void);
unsigned int vga_fb_height(void);
unsigned int vga_fb_pitch(void);
unsigned int vga_fb_bpp(void);
unsigned int vga_fb_vram_kb(void);
unsigned int vga_fb_pci_id(void);
void *vga_fb_ptr(void);
/* Physical address of the linear framebuffer (for off-screen VRAM access). */
unsigned long vga_fb_lfb_addr(void);
int vga_has_vesa(void);

/* A drawing surface: the screen's shadow buffer, a window layer or a canvas.
 * Drawing helpers paint into the *current render target*, which is the screen
 * unless process_set_render_target() pointed this process at a surface. */
struct surface {
	unsigned char *px;
	unsigned int w, h;
	unsigned int pitch;
	unsigned int bpp;             /* 16, 24 or 32 */
	/* Alpha written into the high byte of 32 bpp pixels.  The screen uses
	 * 255 (the byte is padding there); a window layer sets what its
	 * background fill should composite with, while glyph foregrounds are
	 * always written opaque. */
	unsigned char alpha;
};

/* The screen surface (shadow buffer, or the LFB when there is no shadow). */
struct surface *vga_screen_surface(void);

/* Current render target for this process: the screen by default. */
struct surface *vga_target(void);
void vga_set_target(struct surface *s);
int vga_target_is_screen(void);

/* Mark a rectangle of the screen dirty.  A no-op while the target is not the
 * screen: drawing into a layer must not touch the screen's damage list, the
 * compositor marks the rects it copies out. */
void vga_target_damage(unsigned int x, unsigned int y, unsigned int w,
		       unsigned int h);

/* The software presenter used by the blit backend: copies up to max_bytes of
 * the pending damage rects from the shadow buffer to VRAM. */
void vga_present_shadow(unsigned int max_bytes);

void vga_put_pixel_rgb(unsigned int x, unsigned int y, unsigned int r, unsigned int g, unsigned int b);
void vga_fill_rect(unsigned int x, unsigned int y, unsigned int w, unsigned int h,
		unsigned int r, unsigned int g, unsigned int b);
void vga_clear_rgb(unsigned int r, unsigned int g, unsigned int b);

/* Tightly packed row-major: each row is w * pixel_bytes contiguous (pixel_bytes from bpp). */
void vga_fb_read_rect(unsigned int x, unsigned int y, unsigned int w, unsigned int h,
		unsigned char *buf);
void vga_fb_write_rect(unsigned int x, unsigned int y, unsigned int w, unsigned int h,
		const unsigned char *buf);

/* Initialize shadow buffer (called once from kernel_main after vga_cache_boot_fb). */
void vga_shadow_init(void);

/* Copy shadow buffer to VRAM (everything dirty, no budget). Called after
 * blocking redraws and at boot. */
void vga_fb_flush(void);

/* Copy up to max_bytes of the pending damage rects to VRAM (0 = no limit).
 * Called by the main loop every iteration: small updates reach the screen
 * within one loop, large ones are chunked so they cannot stall input. */
void vga_fb_flush_budget(unsigned int max_bytes);

/* Per-iteration flush budget: large enough that a window-sized repaint
 * (~0.7 MB) is flushed as one frame, small enough that a full-screen repaint
 * is chunked instead of stalling input behind it. */
#define VGA_FLUSH_BUDGET_BYTES (1024u * 1024u)

/* Returns 1 if shadow buffer is active, 0 if rendering directly to VRAM. */
int vga_shadow_is_active(void);

/* Mark rows dirty for direct buffer manipulators (e.g. fbtext_scroll). */
void vga_fb_mark_dirty_rect(unsigned int y, unsigned int h);

/* Mark a rectangle dirty, limited to the horizontal span [x0, x1). */
void vga_fb_mark_dirty_rect_xy(unsigned int y, unsigned int h,
			       unsigned int x0, unsigned int x1);

/* Drop all pending damage (so only explicitly re-marked areas get flushed). */
void vga_fb_clear_dirty(void);

#endif
