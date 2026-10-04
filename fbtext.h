/* Framebuffer text: draw characters into VESA LFB. 8x16 font, VGA attribute colors. */
#ifndef FBTEXT_H
#define FBTEXT_H

void fbtext_init(void);
int fbtext_active(void);	/* 1 if using LFB, 0 if VGA text path */

unsigned int fbtext_get_cols(void);
unsigned int fbtext_get_rows(void);

void fbtext_clear(void);
void fbtext_put_char_at(unsigned int col, unsigned int row, unsigned char c, unsigned char attr);

/* Blit a cell buffer (cells of (attr<<8)|ch) into the shadow buffer at pixel
   origin (x0, y0). 8 px per column, 10 px per row. */
void fbtext_draw_cells(const unsigned short *cells, unsigned int cols, unsigned int rows,
		unsigned int x0, unsigned int y0);
void fbtext_scroll_up_one_line(void);
void fbtext_set_cursor(unsigned int col, unsigned int row);
void fbtext_show_cursor(void);
/* Hide cursor and redraw cell with (c, attr) so the character is restored. */
void fbtext_hide_cursor(unsigned char c, unsigned char attr);

/* Draw string at pixel (x0, y0) with fg RGB. transparent_bg 1 = only fg pixels (overlays). */
void fbtext_draw_string_xy(unsigned int x0, unsigned int y0, const char *str,
		unsigned int r_fg, unsigned int g_fg, unsigned int b_fg, int transparent_bg);

/* Draw a rainbow gradient line just below the header row (between header and content). No-op if not LFB. */
void fbtext_draw_rainbow_separator(void);

#endif
