/* API passed to loaded program at PROGRAM_API_ADDR. Used by run.c and prog.c. */

#ifndef PROG_API_H
#define PROG_API_H

#define PROGRAM_API_ADDR 0x20F00
#define IO_KEY_ESC       0x1B

/* Function pointer types for the program API. */
typedef void (*prog_draw_string_xy_t)(unsigned int x0, unsigned int y0,
	const char *str, unsigned int r_fg, unsigned int g_fg, unsigned int b_fg,
	int transparent_bg);
typedef unsigned int (*prog_get_cols_t)(void);
typedef unsigned int (*prog_get_rows_t)(void);
typedef int (*prog_poll_key_t)(void);

struct prog_api {
	prog_draw_string_xy_t draw_string_xy;
	prog_get_cols_t       get_cols;
	prog_get_rows_t       get_rows;
	prog_poll_key_t       poll_key;
};

#endif
