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
void io_puthex(unsigned int n);  /* print n as 8 hex digits, no 0x prefix */
unsigned int io_get_cursor(void);
void io_set_cursor(unsigned int pos);
void io_cursor_show_block(void);
void io_cursor_hide_block(void);
void io_vga_write(unsigned int pos, unsigned short value);
void io_drain_keyboard(void);

/* Poll keyboard; returns 0 = no key, 1..255 = ASCII, or an IO_KEY_* code. */
#define IO_KEY_CTRL_C 0x03  /* Ctrl+C (ASCII ETX), kills the foreground job */
#define IO_KEY_LEFT      0x100
#define IO_KEY_RIGHT     0x101
#define IO_KEY_UP        0x102
#define IO_KEY_DOWN      0x103
#define IO_KEY_PAGE_UP   0x104
#define IO_KEY_PAGE_DOWN 0x105
#define IO_KEY_HOME      0x106
#define IO_KEY_END       0x107
#define IO_KEY_DELETE    0x108
#define IO_KEY_CTRL_LEFT  0x109
#define IO_KEY_CTRL_RIGHT 0x10A
#define IO_KEY_CTRL_DELETE 0x10B
#define IO_KEY_CTRL_BACKSPACE 0x10C
int io_poll_key(void);
/* Same as io_poll_key but also reads port 0x60 when buffer empty (for full-screen programs). */
int io_poll_key_with_hw(void);
/* 1 when the IRQ1 storm guard masked the keyboard; the WM then polls the
   controller directly (every few ms) instead of relying on interrupts. */
int io_keyboard_irq_masked(void);
/* Queue a PS/2 set-1 scancode from a non-IRQ source (USB keyboard backend);
   it is then decoded by the normal io_poll_key path. */
void io_inject_scancode(unsigned char sc);

/* While set, the WM stops draining the keyboard ring: an inline command
   (e.g. `format`'s confirmation) is polling for keys itself.  The WM still
   runs and the USB backend still queues scancodes. */
void io_set_raw_keyboard(int on);
int io_keyboard_raw(void);

#endif
