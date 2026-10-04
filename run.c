/* run command: load raw binary from a FAT filesystem to 0x31000 and execute.
 * Usage: run /<device>/<file>. */

#include "fat.h"
#include "fbtext.h"
#include "io.h"
#include "run_api.h"
#include "serial.h"
#include "timer.h"
#include "vga.h"
#include "vfs.h"

#define PROGRAM_LOAD_ADDR 0x31000
#define PROGRAM_LOAD_MAX  0x10000

void program_run_main(const char *filename)
{
	fat_fs_t *fs;
	const char *fs_path;
	unsigned int dir_cluster;
	char name_8_3[12];
	int n;

	if (!filename || !filename[0]) {
		io_putstr("run: usage run /<device>/<file>\n");
		return;
	}

	if (vfs_resolve(filename, &fs, &fs_path) != 0) {
		io_putstr("run: unknown device or invalid path\n");
		return;
	}

	n = fat_resolve_path(fs, fs_path, &dir_cluster, name_8_3);
	if (n == -2) {
		io_putstr("run: disk read error (see above)\n");
		return;
	}
	if (n != 0) {
		io_putstr("run: file not found\n");
		return;
	}

	n = fat_read_file_in(fs, dir_cluster, name_8_3, (void *)PROGRAM_LOAD_ADDR, PROGRAM_LOAD_MAX);
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
