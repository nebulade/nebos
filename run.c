/* run command: load raw binary from FAT floppy to 0x21000 and execute. */

#include "fat.h"
#include "fbtext.h"
#include "io.h"
#include "prog_api.h"

#define PROGRAM_LOAD_ADDR 0x21000
#define PROGRAM_LOAD_MAX  0x10000
#define NAME_BUF_SIZE     12

static void to_8_3(const char *in, unsigned int in_len, char *out)
{
	unsigned int i, j, dot;
	for (i = 0; i < in_len && in[i] == ' '; i++)
		;
	dot = i;
	while (dot < in_len && in[dot] != '.' && in[dot] != ' ')
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
	while (i < in_len && in[i] != ' ' && j < 11) {
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
		io_putstr("run: disk read error\n");
		return;
	}
	if (n != 0) {
		io_putstr("run: no FAT filesystem found\n");
		return;
	}
	n = fat_read_file(name_8_3, (void *)PROGRAM_LOAD_ADDR, PROGRAM_LOAD_MAX);
	if (n == -2) {
		io_putstr("run: disk read error\n");
		return;
	}
	if (n <= 0) {
		io_putstr("run: file not found\n");
		return;
	}
	{
		struct prog_api api = {
			.draw_string_xy = fbtext_draw_string_xy,
			.get_cols       = fbtext_get_cols,
			.get_rows       = fbtext_get_rows,
			.poll_key       = io_poll_key,
		};
		*(struct prog_api *)PROGRAM_API_ADDR = api;
	}
	((void (*)(void))PROGRAM_LOAD_ADDR)();
}
