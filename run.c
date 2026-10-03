/* run command: load raw binary from FAT floppy to 0x31000 and execute. */

#include "fat.h"
#include "fbtext.h"
#include "io.h"
#include "run_api.h"
#include "serial.h"
#include "timer.h"
#include "vga.h"

#define PROGRAM_LOAD_ADDR 0x31000
#define PROGRAM_LOAD_MAX  0x10000
#define NAME_BUF_SIZE     12

static void to_8_3(const char *in, unsigned int in_len, char *out)
{
	unsigned int i, j, dot;
	for (i = 0; i < in_len && in[i] == ' '; i++)
		;
	dot = i;
	while (dot < in_len && in[dot] != '\0' && in[dot] != '.' && in[dot] != ' ')
		dot++;
	j = 0;
	while (i < dot && j < 8) {
		out[j++] = (char)(in[i] >= 'a' && in[i] <= 'z' ? in[i] - 32 : in[i]);
		i++;
	}
	while (j < 8)
		out[j++] = ' ';
	if (i < in_len && in[i] == '.')
		i++;
	while (i < in_len && in[i] != '\0' && in[i] != ' ' && j < 11) {
		out[j++] = (char)(in[i] >= 'a' && in[i] <= 'z' ? in[i] - 32 : in[i]);
		i++;
	}
	while (j < 11)
		out[j++] = ' ';
	out[11] = '\0';
}

void program_run_main(const char *filename)
{
	char name_8_3[NAME_BUF_SIZE];
	int n;

	if (!filename || !filename[0]) {
		io_putstr("run: usage run <filename>\n");
		return;
	}
	to_8_3(filename, 64, name_8_3);
	n = fat_init();
	if (n == -2) {
		io_putstr("run: disk read error (see above)\n");
		return;
	}
	if (n != 0) {
		io_putstr("run: no FAT filesystem found\n");
		return;
	}
	n = fat_read_file(name_8_3, (void *)PROGRAM_LOAD_ADDR, PROGRAM_LOAD_MAX);
	if (n == -2) {
		io_putstr("run: disk read error (see above)\n");
		return;
	}
	if (n <= 0) {
		io_putstr("run: file not found\n");
		return;
	}
	io_drain_keyboard();
	{
		struct prog_api api = {
			.draw_string_xy = fbtext_draw_string_xy,
			.get_cols       = fbtext_get_cols,
			.get_rows       = fbtext_get_rows,
			.poll_key       = io_poll_key,
			.poll_key_with_hw = io_poll_key_with_hw,
			.get_fb_width   = vga_fb_width,
			.get_fb_height  = vga_fb_height,
			.clear_rgb      = vga_clear_rgb,
			.put_pixel_rgb  = vga_put_pixel_rgb,
			.get_ticks_ms   = get_ticks_ms,
		};
		*(struct prog_api *)PROGRAM_API_ADDR = api;
	}
	((void (*)(void))PROGRAM_LOAD_ADDR)();
}

/* run serial: execute the binary already loaded at 0x31000 via serial_recv. */

void program_run_serial_main(const char *arg)
{
	(void)arg;

	if (serial_recv_size == 0) {
		io_putstr("run serial: no binary received (use 'serial recv' first)\n");
		return;
	}

	io_drain_keyboard();
	{
		struct prog_api api = {
			.draw_string_xy = fbtext_draw_string_xy,
			.get_cols       = fbtext_get_cols,
			.get_rows       = fbtext_get_rows,
			.poll_key       = io_poll_key,
			.poll_key_with_hw = io_poll_key_with_hw,
			.get_fb_width   = vga_fb_width,
			.get_fb_height  = vga_fb_height,
			.clear_rgb      = vga_clear_rgb,
			.put_pixel_rgb  = vga_put_pixel_rgb,
			.get_ticks_ms   = get_ticks_ms,
		};
		*(struct prog_api *)PROGRAM_API_ADDR = api;
	}
	((void (*)(void))PROGRAM_LOAD_ADDR)();
}
