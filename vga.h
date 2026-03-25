/* VGA: VESA LFB (from boot). */
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

/* LFB API. RGB 0..255. */
unsigned int vga_fb_width(void);
unsigned int vga_fb_height(void);
unsigned int vga_fb_pitch(void);
unsigned int vga_fb_bpp(void);
unsigned int vga_fb_vram_kb(void);
unsigned int vga_fb_pci_id(void);
void *vga_fb_ptr(void);
int vga_has_vesa(void);

void vga_put_pixel_rgb(unsigned int x, unsigned int y, unsigned int r, unsigned int g, unsigned int b);
void vga_clear_rgb(unsigned int r, unsigned int g, unsigned int b);

/* Tightly packed row-major: each row is w * pixel_bytes contiguous (pixel_bytes from bpp). */
void vga_fb_read_rect(unsigned int x, unsigned int y, unsigned int w, unsigned int h,
		unsigned char *buf);
void vga_fb_write_rect(unsigned int x, unsigned int y, unsigned int w, unsigned int h,
		const unsigned char *buf);

#endif
