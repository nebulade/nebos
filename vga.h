/* VGA: VESA LFB (from boot). Shadow buffer with 30Hz USWC flush. */
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

/* LFB API. RGB 0..255. All drawing goes into the shadow buffer; flush copies to VRAM at 30Hz. */
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

/* Copy shadow buffer to VRAM. Called by 30Hz timer or manually after init. */
void vga_fb_flush(void);

/* Flag set by PIT IRQ when 30Hz flush is due. Main loop checks and clears. */
extern int vga_shadow_flush_pending;

/* Called from main loop: if pending, copies shadow to VRAM and draws debug square. */
void vga_fb_flush_pending_check(void);

/* Returns 1 if shadow buffer is active, 0 if rendering directly to VRAM. */
int vga_shadow_is_active(void);

/* Mark rows dirty for direct buffer manipulators (e.g. fbtext_scroll). */
void vga_fb_mark_dirty_rect(unsigned int y, unsigned int h);

/* Mark rows dirty, limited to the horizontal span [x0, x1). */
void vga_fb_mark_dirty_rect_xy(unsigned int y, unsigned int h,
			       unsigned int x0, unsigned int x1);

/* Clear the dirty-row bitmap (so only explicitly re-marked rows get flushed). */
void vga_fb_clear_dirty(void);

#endif
