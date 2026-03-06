/* I/O library: VGA text output and PS/2 keyboard. Usable by terminal and other programs. */
#ifndef IO_H
#define IO_H

#define IO_HEADER_ROWS    1
/* Row 0: banner left, datetime right (19 chars "YYYY-MM-DD HH:MM:SS") */
#define IO_DATETIME_LEN   19

unsigned int io_get_cols(void);
unsigned int io_get_rows(void);
#define IO_VGA_COLS       (io_get_cols())
#define IO_VGA_ROWS       (io_get_rows())
#define IO_CONTENT_OFFSET (IO_HEADER_ROWS * io_get_cols())
#define IO_CONTENT_ROWS   (io_get_rows() - IO_HEADER_ROWS)
#define IO_CONTENT_SIZE   (IO_CONTENT_ROWS * io_get_cols())
#define IO_DATETIME_POS   (io_get_cols() - IO_DATETIME_LEN)

void io_init(void);
void io_clear_screen(void);
int io_did_scroll(void);  /* returns 1 once after a scroll, then 0 until next scroll */
void io_putchar(char c);
void io_putstr(const char *s);
unsigned int io_get_cursor(void);
void io_set_cursor(unsigned int pos);
void io_cursor_show_block(void);
void io_cursor_hide_block(void);
void io_vga_write(unsigned int pos, unsigned short value);
void io_drain_keyboard(void);

/* Poll keyboard; returns 0 = no key, 1..255 = ASCII, or IO_KEY_LEFT/IO_KEY_RIGHT. */
#define IO_KEY_LEFT  0x100
#define IO_KEY_RIGHT 0x101
int io_poll_key(void);
unsigned char io_last_scancode(void);

#endif
