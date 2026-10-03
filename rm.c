/* rm command: delete a file from the FAT12 floppy. */

#include "fat.h"
#include "io.h"

#define NAME_BUF_SIZE 12

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

void program_rm_main(const char *arg)
{
	char name_8_3[NAME_BUF_SIZE];
	unsigned int i, len;
	int r;

	if (!arg || !arg[0]) {
		io_putstr("rm: usage rm <file>\n");
		return;
	}
	i = 0;
	while (arg[i] == ' ')
		i++;
	len = 0;
	while (arg[i + len])
		len++;
	if (len == 0) {
		io_putstr("rm: usage rm <file>\n");
		return;
	}
	to_8_3(arg + i, len, name_8_3);

	r = fat_init();
	if (r == -2) {
		io_putstr("rm: disk read error\n");
		return;
	}
	if (r != 0) {
		io_putstr("rm: no FAT filesystem found\n");
		return;
	}
	r = fat_delete_file(name_8_3);
	if (r == -2) {
		io_putstr("rm: disk error\n");
		return;
	}
	if (r != 0) {
		io_putstr("rm: file not found\n");
		return;
	}
	io_putstr("removed\n");
}
