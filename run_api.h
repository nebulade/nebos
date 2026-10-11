/* API passed to loaded program at PROGRAM_API_ADDR. Used by run command and by programs run via run (e.g. prog, space). */

#ifndef RUN_API_H
#define RUN_API_H

/* Sits just below the program load area, inside the low 1 MB the frame
 * allocator reserves; keep in sync with run.c and user.ld. */
#define PROGRAM_API_ADDR 0x7FF00
#define IO_KEY_ESC       0x1B
#define IO_KEY_SPACE     0x20
#define IO_KEY_LEFT      0x100
#define IO_KEY_RIGHT     0x101

/* Function pointer types for the program API. */
typedef void (*prog_draw_string_xy_t)(unsigned int x0, unsigned int y0,
	const char *str, unsigned int r_fg, unsigned int g_fg, unsigned int b_fg,
	int transparent_bg);
typedef unsigned int (*prog_get_cols_t)(void);
typedef unsigned int (*prog_get_rows_t)(void);
typedef int (*prog_poll_key_t)(void);
typedef int (*prog_poll_key_with_hw_t)(void);
typedef unsigned int (*prog_get_fb_width_t)(void);
typedef unsigned int (*prog_get_fb_height_t)(void);
typedef void (*prog_clear_rgb_t)(unsigned int r, unsigned int g, unsigned int b);
typedef void (*prog_put_pixel_rgb_t)(unsigned int x, unsigned int y,
	unsigned int r, unsigned int g, unsigned int b);
typedef unsigned int (*prog_get_ticks_ms_t)(void);

struct prog_api {
	prog_draw_string_xy_t draw_string_xy;
	prog_get_cols_t       get_cols;
	prog_get_rows_t       get_rows;
	prog_poll_key_t       poll_key;
	prog_poll_key_with_hw_t poll_key_with_hw;
	prog_get_fb_width_t   get_fb_width;
	prog_get_fb_height_t  get_fb_height;
	prog_clear_rgb_t      clear_rgb;
	prog_put_pixel_rgb_t  put_pixel_rgb;
	prog_get_ticks_ms_t   get_ticks_ms;
};

#endif
