/* VGA graphics: mode 13h (320x200 256-color) and text mode 03h.
   Works on real VGA hardware and QEMU alike. */
#ifndef VGA_H
#define VGA_H

#define VGA_GFX_WIDTH   320
#define VGA_GFX_HEIGHT  200

void vga_set_mode_13(void);
void vga_set_mode_text(void);
void vga_put_pixel(int x, int y, unsigned char color);
void vga_clear(unsigned char color);

#endif
