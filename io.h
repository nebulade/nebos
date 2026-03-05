/* I/O library: VGA text output and PS/2 keyboard. Usable by terminal and other programs. */
#ifndef IO_H
#define IO_H

#define IO_VGA_COLS       80
#define IO_VGA_ROWS       25
#define IO_HEADER_ROWS    1
#define IO_CONTENT_OFFSET (IO_HEADER_ROWS * IO_VGA_COLS)   /* row 1 start */
#define IO_CONTENT_ROWS   (IO_VGA_ROWS - IO_HEADER_ROWS)   /* 24 rows */
#define IO_CONTENT_SIZE   (IO_CONTENT_ROWS * IO_VGA_COLS)
/* Row 0: banner left, datetime right (19 chars "YYYY-MM-DD HH:MM:SS") */
#define IO_DATETIME_LEN   19
#define IO_DATETIME_POS   (IO_VGA_COLS - IO_DATETIME_LEN)

void io_init(void);
void io_clear_screen(void);
int io_did_scroll(void);  /* returns 1 once after a scroll, then 0 until next scroll */
void io_putchar(char c);
void io_putstr(const char *s);
unsigned int io_get_cursor(void);
void io_cursor_show_block(void);
void io_cursor_hide_block(void);
void io_vga_write(unsigned int pos, unsigned short value);
void io_drain_keyboard(void);

/* Poll keyboard; returns 0 or ASCII character. Shift applied. */
char io_poll_key(void);
unsigned char io_last_scancode(void);

#endif
