/* run command: load raw binary from a FAT filesystem to 0x43000 and execute.
 * Usage: run /<device>/<file>. */

#include "fat.h"
#include "fbtext.h"
#include "io.h"
#include "run_api.h"
#include "serial.h"
#include "timer.h"
#include "vga.h"
#include "vfs.h"

#define PROGRAM_LOAD_ADDR 0x43000
#define PROGRAM_LOAD_MAX  0x10000

/* User programs embedded in the kernel image (like hello.elf): they travel
 * with `make dev`/`make net-dev`, so a board with no readable filesystem can
 * still run them.  Rebuilt from prog.c/space.c by the Makefile. */
extern unsigned char _binary_prog_bin_start[], _binary_prog_bin_end[];
extern unsigned char _binary_space_bin_start[], _binary_space_bin_end[];

struct embedded_prog {
	const char *name;
	const unsigned char *start;
	const unsigned char *end;
};

static const struct embedded_prog embedded_progs[] = {
	{ "prog",  _binary_prog_bin_start,  _binary_prog_bin_end },
	{ "space", _binary_space_bin_start, _binary_space_bin_end },
};

static int name_eq(const char *a, const char *b)
{
	unsigned int i = 0;

	while (a[i] && b[i] && a[i] == b[i])
		i++;
	return a[i] == '\0' && b[i] == '\0';
}

/* Install the program API and hand control to the image at PROGRAM_LOAD_ADDR. */
static void jump_to_program(void)
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

	io_drain_keyboard();
	*(struct prog_api *)PROGRAM_API_ADDR = api;
	((void (*)(void))PROGRAM_LOAD_ADDR)();
}

/* Load the named embedded program and run it (0 = name not found). */
static int run_embedded(const char *name)
{
	unsigned int i;

	for (i = 0; i < sizeof(embedded_progs) / sizeof(embedded_progs[0]); i++) {
		const struct embedded_prog *p = &embedded_progs[i];
		unsigned int size;
		unsigned char *dst = (unsigned char *)PROGRAM_LOAD_ADDR;
		const unsigned char *src;

		if (!name_eq(name, p->name))
			continue;
		size = (unsigned int)(p->end - p->start);
		if (size == 0 || size > PROGRAM_LOAD_MAX) {
			io_putstr("run: embedded image size out of range\n");
			return 1;
		}
		for (src = p->start; src < p->end; )
			*dst++ = *src++;
		jump_to_program();
		return 1;
	}
	return 0;
}

void program_run_main(const char *filename)
{
	fat_fs_t *fs;
	const char *fs_path;
	unsigned int dir_cluster;
	char name_8_3[12];
	int n;

	if (!filename || !filename[0]) {
		io_putstr("run: usage run <name>|/<device>/<file>\n");
		return;
	}

	/* Names of embedded programs first: `run prog`, `run space`. */
	if (run_embedded(filename))
		return;

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

/* run serial: execute the binary already loaded at 0x43000 via serial_recv. */

void program_run_serial_main(const char *arg)
{
	(void)arg;

	if (serial_recv_size == 0) {
		io_putstr("run serial: no binary received (use 'serial recv' first)\n");
		return;
	}
	jump_to_program();
}

/* Command entry for the embedded programs (`prog`, `space`). */
void program_run_embedded_main(const char *name)
{
	if (!run_embedded(name))
		io_putstr("run: no such embedded program\n");
}
