/* I/O library: VGA text output and PS/2 keyboard. Usable by terminal and other programs. */
#ifndef IO_H
#define IO_H

/* Content area: rows 0..(IO_DEBUG_ROW-1). Row IO_DEBUG_ROW is below content. */
#define IO_VGA_COLS      80
#define IO_VGA_ROWS      25
#define IO_DEBUG_ROW     24
#define IO_CONTENT_ROWS  IO_DEBUG_ROW
#define IO_CONTENT_SIZE  (IO_CONTENT_ROWS * IO_VGA_COLS)

void io_init(void);
void io_clear_screen(void);
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
