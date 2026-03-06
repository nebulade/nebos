/* VGA: VESA LFB (from boot) or legacy mode 13h / text mode 03h. */
#ifndef VGA_H
#define VGA_H

/* Legacy mode 13h (320x200 256-color); used only when no VESA. */
#define VGA_GFX_WIDTH   320
#define VGA_GFX_HEIGHT  200

/* Boot video info at 0x600 (written by boot.S in safe conventional memory).
   Layout: valid(1) width(2) height(2) pitch(2) bpp(1) lfb_base(4) = 12 bytes. */
struct vga_boot_fb_info {
	unsigned char  valid;
	unsigned short width;
	unsigned short height;
	unsigned short pitch;
	unsigned char  bpp;
	unsigned int   lfb_base;
} __attribute__((packed));

#define VGA_BOOT_FB ((volatile struct vga_boot_fb_info *)0x600)

void vga_set_mode_13(void);
void vga_set_mode_text(void);
void vga_put_pixel(int x, int y, unsigned char color);
void vga_clear(unsigned char color);

/* LFB API (use when VESA valid). RGB 0..255. */
unsigned int vga_fb_width(void);
unsigned int vga_fb_height(void);
unsigned int vga_fb_pitch(void);
unsigned int vga_fb_bpp(void);
void *vga_fb_ptr(void);
int vga_has_vesa(void);

void vga_put_pixel_rgb(unsigned int x, unsigned int y, unsigned int r, unsigned int g, unsigned int b);
void vga_clear_rgb(unsigned int r, unsigned int g, unsigned int b);

#endif
