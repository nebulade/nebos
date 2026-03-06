/* Framebuffer text: draw characters into VESA LFB. 8x16 font, VGA attribute colors. */
#ifndef FBTEXT_H
#define FBTEXT_H

void fbtext_init(void);
int fbtext_active(void);	/* 1 if using LFB, 0 if VGA text path */

unsigned int fbtext_get_cols(void);
unsigned int fbtext_get_rows(void);

void fbtext_clear(void);
void fbtext_put_char_at(unsigned int col, unsigned int row, unsigned char c, unsigned char attr);
void fbtext_scroll_up_one_line(void);
void fbtext_set_cursor(unsigned int col, unsigned int row);
void fbtext_show_cursor(void);
/* Hide cursor and redraw cell with (c, attr) so the character is restored. */
void fbtext_hide_cursor(unsigned char c, unsigned char attr);

#endif
