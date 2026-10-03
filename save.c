/* save command: write a text string to a file on the FAT12 floppy.
 * Usage: save <file> <text...>  (text may contain spaces). */

#include "fat.h"
#include "io.h"

#define NAME_BUF_SIZE 12

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

void program_save_main(const char *arg)
{
	char name_8_3[NAME_BUF_SIZE];
	const char *text;
	unsigned int i, fn_len, text_len;
	int r;

	if (!arg || !arg[0]) {
		io_putstr("save: usage save <file> <text>\n");
		return;
	}

	/* filename = first token */
	i = 0;
	while (arg[i] == ' ')
		i++;
	fn_len = 0;
	while (arg[i + fn_len] && arg[i + fn_len] != ' ')
		fn_len++;
	if (fn_len == 0) {
		io_putstr("save: usage save <file> <text>\n");
		return;
	}
	to_8_3(arg + i, fn_len, name_8_3);

	/* text = the rest (trimmed) */
	i += fn_len;
	while (arg[i] == ' ')
		i++;
	text = arg + i;
	text_len = 0;
	while (text[text_len])
		text_len++;

	r = fat_init();
	if (r == -2) {
		io_putstr("save: disk read error\n");
		return;
	}
	if (r != 0) {
		io_putstr("save: no FAT filesystem found\n");
		return;
	}
	r = fat_write_file(name_8_3, text, text_len);
	if (r == -2) {
		io_putstr("save: disk write error\n");
		return;
	}
	if (r != 0) {
		io_putstr("save: write failed\n");
		return;
	}
	io_putstr("saved\n");
}
