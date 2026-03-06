/* cat command: print file contents to screen. */

#include "fat.h"
#include "io.h"

#define DUMP_BUF_SIZE 4096
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

void program_cat_main(const char *filename)
{
	static unsigned char buf[DUMP_BUF_SIZE];
	char name_8_3[NAME_BUF_SIZE];
	int n;
	unsigned int i;

	if (!filename || !filename[0]) {
		io_putstr("cat: usage cat <filename>\n");
		return;
	}
	to_8_3(filename, 64, name_8_3);
	if (fat_init() != 0) {
		io_putstr("cat: fat init failed\n");
		return;
	}
	n = fat_read_file(name_8_3, buf, DUMP_BUF_SIZE);
	if (n == -2) {
		io_putstr("cat: disk read error\n");
		return;
	}
	if (n < 0) {
		io_putstr("cat: file not found\n");
		return;
	}
	for (i = 0; i < (unsigned int)n; i++) {
		unsigned char c = buf[i];
		if (c >= 0x20 && c < 0x7F)
			io_putchar((char)c);
		else if (c == '\n')
			io_putchar('\n');
		else if (c == '\t')
			io_putchar('\t');
		else
			io_putchar('.');
	}
	if (n > 0 && buf[n - 1] != '\n')
		io_putstr("\n");
}
