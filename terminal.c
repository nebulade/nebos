/* Terminal: prompt, datetime (top right), blinking cursor, echo loop, command dispatch.
   Commands are spawned as separate processes for preemptive multitasking. */

#include "io.h"
#include "dbg.h"
#include "kdbg.h"
#include "kexec.h"
#include "process.h"
#include "terminal.h"
#include "timer.h"

extern unsigned char _binary_hello_elf_start[];
extern unsigned char _binary_hello_elf_end[];
extern void program_reboot_main(void);
extern void program_poweroff_main(void);
extern void program_ls_main(const char *path);
extern void program_cat_main(const char *filename);
extern void program_run_main(const char *filename);
extern void program_save_main(const char *arg);
extern void program_rm_main(const char *arg);
extern void program_mkdir_main(const char *arg);
extern void program_rmdir_main(const char *arg);
extern void program_clock_main(void);
extern void program_colors_main(void);
extern void program_display_main(void);
extern void program_cpuinfo_main(void);
extern void program_mouseinfo_main(void);
extern void program_serial_loop_main(const char *arg);
extern void program_serial_recv_main(const char *arg);
extern void program_run_serial_main(const char *arg);
extern void program_run_embedded_main(const char *name);
extern void program_lsusb_main(void);
extern void program_storage_main(void);
extern void program_mem_main(void);
extern void program_format_main(const char *arg);
extern void program_lspci_main(const char *filter);
extern void program_peek_main(const char *arg);
extern void program_poke_main(const char *arg);
extern void program_netinfo_main(void);
extern void program_netrecv_main(void);
extern void program_netsend_main(void);
extern void program_netip_main(const char *arg);
extern void program_netlog_main(const char *arg);
extern void program_netconsole_main(const char *arg);
extern void program_dhcp_main(void);
extern void program_netregs_main(void);
extern void program_netloop_main(void);
extern void program_netreset_main(void);
extern void program_pong_main(void);
extern void program_ping_main(const char *arg);
extern void program_curl_main(const char *arg);
extern void program_netdns_main(const char *arg);
extern void program_host_main(const char *arg);

/* Print the prompt (with the current directory) and set line_start. */
static void term_print_prompt(struct term *t, int first)
{
	if (!first)
		io_putchar('\n');
	io_putchar('[');
	io_putstr(t->cwd);
	io_putstr("] > ");
	t->line_start = io_get_cursor();
}

static int is_cmd_hello(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 5 > line_len)
		return 0;
	if (line[i] != 'h' || line[i+1] != 'e' || line[i+2] != 'l' || line[i+3] != 'l' || line[i+4] != 'o')
		return 0;
	i += 5;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_reboot(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 6 > line_len)
		return 0;
	if (line[i] != 'r' || line[i+1] != 'e' || line[i+2] != 'b' || line[i+3] != 'o' || line[i+4] != 'o' || line[i+5] != 't')
		return 0;
	i += 6;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_poweroff(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 8 > line_len)
		return 0;
	if (line[i] != 'p' || line[i+1] != 'o' || line[i+2] != 'w' || line[i+3] != 'e' ||
	    line[i+4] != 'r' || line[i+5] != 'o' || line[i+6] != 'f' || line[i+7] != 'f')
		return 0;
	i += 8;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_ls(const char *line, unsigned int line_len, char *path_buf, unsigned int buf_size)
{
	unsigned int i = 0, j = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 2 > line_len)
		return 0;
	if (line[i] != 'l' || line[i+1] != 's')
		return 0;
	i += 2;
	if (i < line_len && line[i] != ' ')
		return 0;  /* "ls" must be a whole word, so reject "lsusb" etc. */
	while (i < line_len && line[i] == ' ')
		i++;
	if (i >= line_len) {
		if (buf_size > 0)
			path_buf[0] = '\0';
		return 1;
	}
	while (i < line_len && j + 1 < buf_size)
		path_buf[j++] = line[i++];
	path_buf[j] = '\0';
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_cd(const char *line, unsigned int line_len, char *path_buf, unsigned int buf_size)
{
	unsigned int i = 0, j = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 2 > line_len)
		return 0;
	if (line[i] != 'c' || line[i+1] != 'd')
		return 0;
	i += 2;
	if (i < line_len && line[i] != ' ')
		return 0;  /* "cd" must be a whole word */
	while (i < line_len && line[i] == ' ')
		i++;
	if (i >= line_len) {
		if (buf_size > 0)
			path_buf[0] = '\0';
		return 1;
	}
	while (i < line_len && j + 1 < buf_size)
		path_buf[j++] = line[i++];
	path_buf[j] = '\0';
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_clock(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 5 > line_len)
		return 0;
	if (line[i] != 'c' || line[i+1] != 'l' || line[i+2] != 'o' ||
	    line[i+3] != 'c' || line[i+4] != 'k')
		return 0;
	i += 5;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_colors(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 6 > line_len)
		return 0;
	if (line[i] != 'c' || line[i+1] != 'o' || line[i+2] != 'l' ||
	    line[i+3] != 'o' || line[i+4] != 'r' || line[i+5] != 's')
		return 0;
	i += 6;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_display(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 7 > line_len)
		return 0;
	if (line[i] != 'd' || line[i+1] != 'i' || line[i+2] != 's' || line[i+3] != 'p' ||
	    line[i+4] != 'l' || line[i+5] != 'a' || line[i+6] != 'y')
		return 0;
	i += 7;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_cpuinfo(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 7 > line_len)
		return 0;
	if (line[i] != 'c' || line[i+1] != 'p' || line[i+2] != 'u' || line[i+3] != 'i' ||
	    line[i+4] != 'n' || line[i+5] != 'f' || line[i+6] != 'o')
		return 0;
	i += 7;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_mouseinfo(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 9 > line_len)
		return 0;
	if (line[i] != 'm' || line[i+1] != 'o' || line[i+2] != 'u' ||
	    line[i+3] != 's' || line[i+4] != 'e' || line[i+5] != 'i' ||
	    line[i+6] != 'n' || line[i+7] != 'f' || line[i+8] != 'o')
		return 0;
	i += 9;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_lsusb(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 5 > line_len)
		return 0;
	if (line[i] != 'l' || line[i+1] != 's' || line[i+2] != 'u' ||
	    line[i+3] != 's' || line[i+4] != 'b')
		return 0;
	i += 5;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_lspci(const char *line, unsigned int line_len, char *arg_buf, unsigned int buf_size)
{
	unsigned int i = 0, j = 0;

	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 5 > line_len)
		return 0;
	if (line[i] != 'l' || line[i+1] != 's' || line[i+2] != 'p' ||
	    line[i+3] != 'c' || line[i+4] != 'i')
		return 0;
	i += 5;
	if (i < line_len && line[i] != ' ')
		return 0;  /* "lspci" must be a whole word */
	while (i < line_len && line[i] == ' ')
		i++;
	if (i >= line_len) {
		if (buf_size > 0)
			arg_buf[0] = '\0';
		return 1;
	}
	while (i < line_len && j + 1 < buf_size)
		arg_buf[j++] = line[i++];
	arg_buf[j] = '\0';
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_peek(const char *line, unsigned int line_len, char *arg_buf, unsigned int buf_size)
{
	unsigned int i = 0, j = 0;

	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 4 > line_len)
		return 0;
	if (line[i] != 'p' || line[i+1] != 'e' || line[i+2] != 'e' ||
	    line[i+3] != 'k')
		return 0;
	i += 4;
	if (i < line_len && line[i] != ' ')
		return 0;  /* "peek" must be a whole word */
	while (i < line_len && line[i] == ' ')
		i++;
	if (i >= line_len) {
		if (buf_size > 0)
			arg_buf[0] = '\0';
		return 1;
	}
	while (i < line_len && j + 1 < buf_size)
		arg_buf[j++] = line[i++];
	arg_buf[j] = '\0';
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

/* poke <addr> <val>: write one dword to memory/MMIO. */
static int is_cmd_poke(const char *line, unsigned int line_len, char *arg_buf, unsigned int buf_size)
{
	unsigned int i = 0, j = 0;

	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 4 > line_len)
		return 0;
	if (line[i] != 'p' || line[i+1] != 'o' || line[i+2] != 'k' ||
	    line[i+3] != 'e')
		return 0;
	i += 4;
	if (i < line_len && line[i] != ' ')
		return 0;  /* "poke" must be a whole word */
	while (i < line_len && line[i] == ' ')
		i++;
	if (i >= line_len)
		return 0;  /* address and value required */
	while (i < line_len && j + 1 < buf_size)
		arg_buf[j++] = line[i++];
	arg_buf[j] = '\0';
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_prog(const char *line, unsigned int line_len){
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 4 > line_len)
		return 0;
	if (line[i] != 'p' || line[i+1] != 'r' || line[i+2] != 'o' ||
	    line[i+3] != 'g')
		return 0;
	i += 4;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_netinfo(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 7 > line_len)
		return 0;
	if (line[i] != 'n' || line[i+1] != 'e' || line[i+2] != 't' ||
	    line[i+3] != 'i' || line[i+4] != 'n' || line[i+5] != 'f' ||
	    line[i+6] != 'o')
		return 0;
	i += 7;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_netlog(const char *line, unsigned int line_len, char *arg_buf, unsigned int buf_size)
{
	unsigned int i = 0, j = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 6 > line_len)
		return 0;
	if (line[i] != 'n' || line[i+1] != 'e' || line[i+2] != 't' ||
	    line[i+3] != 'l' || line[i+4] != 'o' || line[i+5] != 'g')
		return 0;
	i += 6;
	if (i < line_len && line[i] != ' ')
		return 0;  /* "netlog" must be a whole word */
	while (i < line_len && line[i] == ' ')
		i++;
	if (i >= line_len) {
		if (buf_size > 0)
			arg_buf[0] = '\0';
		return 1;
	}
	while (i < line_len && j + 1 < buf_size)
		arg_buf[j++] = line[i++];
	arg_buf[j] = '\0';
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_netconsole(const char *line, unsigned int line_len, char *arg_buf, unsigned int buf_size)
{
	unsigned int i = 0, j = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 10 > line_len)
		return 0;
	if (line[i] != 'n' || line[i+1] != 'e' || line[i+2] != 't' ||
	    line[i+3] != 'c' || line[i+4] != 'o' || line[i+5] != 'n' ||
	    line[i+6] != 's' || line[i+7] != 'o' || line[i+8] != 'l' ||
	    line[i+9] != 'e')
		return 0;
	i += 10;
	if (i < line_len && line[i] != ' ')
		return 0;  /* "netconsole" must be a whole word */
	while (i < line_len && line[i] == ' ')
		i++;
	if (i >= line_len) {
		if (buf_size > 0)
			arg_buf[0] = '\0';
		return 1;
	}
	while (i < line_len && j + 1 < buf_size)
		arg_buf[j++] = line[i++];
	arg_buf[j] = '\0';
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_dhcp(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 4 > line_len)
		return 0;
	if (line[i] != 'd' || line[i+1] != 'h' || line[i+2] != 'c' ||
	    line[i+3] != 'p')
		return 0;
	i += 4;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_netregs(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 7 > line_len)
		return 0;
	if (line[i] != 'n' || line[i+1] != 'e' || line[i+2] != 't' ||
	    line[i+3] != 'r' || line[i+4] != 'e' || line[i+5] != 'g' ||
	    line[i+6] != 's')
		return 0;
	i += 7;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_netloop(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 7 > line_len)
		return 0;
	if (line[i] != 'n' || line[i+1] != 'e' || line[i+2] != 't' ||
	    line[i+3] != 'l' || line[i+4] != 'o' || line[i+5] != 'o' ||
	    line[i+6] != 'p')
		return 0;
	i += 7;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_netreset(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 8 > line_len)
		return 0;
	if (line[i] != 'n' || line[i+1] != 'e' || line[i+2] != 't' ||
	    line[i+3] != 'r' || line[i+4] != 'e' || line[i+5] != 's' ||
	    line[i+6] != 'e' || line[i+7] != 't')
		return 0;
	i += 8;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_netrecv(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 7 > line_len)
		return 0;
	if (line[i] != 'n' || line[i+1] != 'e' || line[i+2] != 't' ||
	    line[i+3] != 'r' || line[i+4] != 'e' || line[i+5] != 'c' ||
	    line[i+6] != 'v')
		return 0;
	i += 7;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_netsend(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 7 > line_len)
		return 0;
	if (line[i] != 'n' || line[i+1] != 'e' || line[i+2] != 't' ||
	    line[i+3] != 's' || line[i+4] != 'e' || line[i+5] != 'n' ||
	    line[i+6] != 'd')
		return 0;
	i += 7;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_netip(const char *line, unsigned int line_len, char *arg_buf, unsigned int buf_size)
{
	unsigned int i = 0, j = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 5 > line_len)
		return 0;
	if (line[i] != 'n' || line[i+1] != 'e' || line[i+2] != 't' ||
	    line[i+3] != 'i' || line[i+4] != 'p')
		return 0;
	i += 5;
	if (i < line_len && line[i] != ' ')
		return 0;  /* "netip" must be a whole word */
	while (i < line_len && line[i] == ' ')
		i++;
	if (i >= line_len) {
		if (buf_size > 0)
			arg_buf[0] = '\0';
		return 1;
	}
	while (i < line_len && j + 1 < buf_size)
		arg_buf[j++] = line[i++];
	arg_buf[j] = '\0';
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

/* One optional single-token argument (hostname or dotted IP). */
static int is_cmd_ping(const char *line, unsigned int line_len, char *arg_buf, unsigned int buf_size)
{
	unsigned int i = 0, j = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 4 > line_len)
		return 0;
	if (line[i] != 'p' || line[i+1] != 'i' || line[i+2] != 'n' ||
	    line[i+3] != 'g')
		return 0;
	i += 4;
	if (i < line_len && line[i] != ' ')
		return 0;  /* "ping" must be a whole word */
	while (i < line_len && line[i] == ' ')
		i++;
	if (i >= line_len) {
		if (buf_size > 0)
			arg_buf[0] = '\0';
		return 1;
	}
	while (i < line_len && j + 1 < buf_size)
		arg_buf[j++] = line[i++];
	arg_buf[j] = '\0';
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

/* One optional single-token argument (URL, name or dotted IP). */
static int is_cmd_curl(const char *line, unsigned int line_len, char *arg_buf, unsigned int buf_size)
{
	unsigned int i = 0, j = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 4 > line_len)
		return 0;
	if (line[i] != 'c' || line[i+1] != 'u' || line[i+2] != 'r' ||
	    line[i+3] != 'l')
		return 0;
	i += 4;
	if (i < line_len && line[i] != ' ')
		return 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i >= line_len) {
		if (buf_size > 0)
			arg_buf[0] = '\0';
		return 1;
	}
	while (i < line_len && j + 1 < buf_size)
		arg_buf[j++] = line[i++];
	arg_buf[j] = '\0';
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_netdns(const char *line, unsigned int line_len, char *arg_buf, unsigned int buf_size)
{
	unsigned int i = 0, j = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 6 > line_len)
		return 0;
	if (line[i] != 'n' || line[i+1] != 'e' || line[i+2] != 't' ||
	    line[i+3] != 'd' || line[i+4] != 'n' || line[i+5] != 's')
		return 0;
	i += 6;
	if (i < line_len && line[i] != ' ')
		return 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i >= line_len) {
		if (buf_size > 0)
			arg_buf[0] = '\0';
		return 1;
	}
	while (i < line_len && j + 1 < buf_size)
		arg_buf[j++] = line[i++];
	arg_buf[j] = '\0';
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

/* One optional single-token argument (DNS name). */
static int is_cmd_host(const char *line, unsigned int line_len, char *arg_buf, unsigned int buf_size)
{
	unsigned int i = 0, j = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 4 > line_len)
		return 0;
	if (line[i] != 'h' || line[i+1] != 'o' || line[i+2] != 's' ||
	    line[i+3] != 't')
		return 0;
	i += 4;
	if (i < line_len && line[i] != ' ')
		return 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i >= line_len) {
		if (buf_size > 0)
			arg_buf[0] = '\0';
		return 1;
	}
	while (i < line_len && j + 1 < buf_size)
		arg_buf[j++] = line[i++];
	arg_buf[j] = '\0';
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_pong(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 4 > line_len)
		return 0;
	if (line[i] != 'p' || line[i+1] != 'o' || line[i+2] != 'n' || line[i+3] != 'g')
		return 0;
	i += 4;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_space(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 5 > line_len)
		return 0;
	if (line[i] != 's' || line[i+1] != 'p' || line[i+2] != 'a' ||
	    line[i+3] != 'c' || line[i+4] != 'e')
		return 0;
	i += 5;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

#define NET_UDP_ARG_MAX 128
#define CAT_FNAME_MAX 64
static int is_cmd_cat(const char *line, unsigned int line_len, char *filename_buf, unsigned int buf_size)
{
	unsigned int i = 0, j;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 3 > line_len)
		return 0;
	if (line[i] != 'c' || line[i+1] != 'a' || line[i+2] != 't')
		return 0;
	i += 3;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i >= line_len)
		return 0;
	j = 0;
	while (i < line_len && line[i] != ' ' && j < buf_size - 1)
		filename_buf[j++] = line[i++];
	filename_buf[j] = '\0';
	return (j > 0);
}

static int is_cmd_run(const char *line, unsigned int line_len, char *filename_buf, unsigned int buf_size)
{
	unsigned int i = 0, j;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 3 > line_len)
		return 0;
	if (line[i] != 'r' || line[i+1] != 'u' || line[i+2] != 'n')
		return 0;
	i += 3;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i >= line_len)
		return 0;
	j = 0;
	while (i < line_len && line[i] != ' ' && j < buf_size - 1)
		filename_buf[j++] = line[i++];
	filename_buf[j] = '\0';
	return (j > 0);
}

#define SAVE_ARGS_MAX 128
#define RM_FNAME_MAX 64
#define LS_PATH_MAX 64
#define FORMAT_DEV_MAX 16

static int is_cmd_save(const char *line, unsigned int line_len, char *arg_buf, unsigned int buf_size)
{
	unsigned int i = 0, j = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 4 > line_len)
		return 0;
	if (line[i] != 's' || line[i+1] != 'a' || line[i+2] != 'v' || line[i+3] != 'e')
		return 0;
	i += 4;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i >= line_len)
		return 0;
	while (i < line_len && j + 1 < buf_size)
		arg_buf[j++] = line[i++];
	while (j > 0 && arg_buf[j - 1] == ' ')
		j--;
	arg_buf[j] = '\0';
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len && j > 0);
}

static int is_cmd_rm(const char *line, unsigned int line_len, char *filename_buf, unsigned int buf_size)
{
	unsigned int i = 0, j = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 2 > line_len)
		return 0;
	if (line[i] != 'r' || line[i+1] != 'm')
		return 0;
	i += 2;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i >= line_len)
		return 0;
	while (i < line_len && line[i] != ' ' && j < buf_size - 1)
		filename_buf[j++] = line[i++];
	filename_buf[j] = '\0';
	return (j > 0);
}

static int is_cmd_mkdir(const char *line, unsigned int line_len, char *path_buf, unsigned int buf_size)
{
	unsigned int i = 0, j = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 5 > line_len)
		return 0;
	if (line[i] != 'm' || line[i+1] != 'k' || line[i+2] != 'd' || line[i+3] != 'i' || line[i+4] != 'r')
		return 0;
	i += 5;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i >= line_len)
		return 0;
	while (i < line_len && line[i] != ' ' && j < buf_size - 1)
		path_buf[j++] = line[i++];
	path_buf[j] = '\0';
	return (j > 0);
}

static int is_cmd_rmdir(const char *line, unsigned int line_len, char *path_buf, unsigned int buf_size)
{
	unsigned int i = 0, j = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 5 > line_len)
		return 0;
	if (line[i] != 'r' || line[i+1] != 'm' || line[i+2] != 'd' || line[i+3] != 'i' || line[i+4] != 'r')
		return 0;
	i += 5;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i >= line_len)
		return 0;
	while (i < line_len && line[i] != ' ' && j < buf_size - 1)
		path_buf[j++] = line[i++];
	path_buf[j] = '\0';
	return (j > 0);
}

static int is_cmd_storage(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 7 > line_len)
		return 0;
	if (line[i] != 's' || line[i+1] != 't' || line[i+2] != 'o' ||
	    line[i+3] != 'r' || line[i+4] != 'a' || line[i+5] != 'g' || line[i+6] != 'e')
		return 0;
	i += 7;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_meminfo(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 7 > line_len)
		return 0;
	if (line[i] != 'm' || line[i+1] != 'e' || line[i+2] != 'm' ||
	    line[i+3] != 'i' || line[i+4] != 'n' || line[i+5] != 'f' ||
	    line[i+6] != 'o')
		return 0;
	i += 7;
	if (i < line_len && line[i] != ' ')
		return 0;  /* "meminfo" must be a whole word */
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_format(const char *line, unsigned int line_len, char *dev_buf, unsigned int buf_size)
{
	unsigned int i = 0, j;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 6 > line_len)
		return 0;
	if (line[i] != 'f' || line[i+1] != 'o' || line[i+2] != 'r' ||
	    line[i+3] != 'm' || line[i+4] != 'a' || line[i+5] != 't')
		return 0;
	i += 6;
	if (i < line_len && line[i] != ' ')
		return 0;  /* "format" must be a whole word */
	while (i < line_len && line[i] == ' ')
		i++;
	if (i >= line_len)
		return 0;
	j = 0;
	while (i < line_len && line[i] != ' ' && j < buf_size - 1)
		dev_buf[j++] = line[i++];
	dev_buf[j] = '\0';
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len && j > 0);
}

/* Help listing: printed as two 32-column columns so it fits the 64-column
   terminal without scrolling. */
static const char *const help_items[] = {
	"cd [path]", "ls [path]", "cat <path>", "save <path> <text>",
	"rm <path>", "mkdir <path>", "rmdir <path>", "hello", "reboot",
	"poweroff", "run <name>|<file>", "space", "prog", "clock", "colors",
	"display", "cpuinfo", "lsusb", "mouseinfo", "storage", "meminfo",
	"format <device>", "lspci [id]", "peek <addr> [len]", "poke <addr> <val>",
	"netinfo",
	"netrecv", "netsend", "netip [a.b.c.d]", "ping <host|a.b.c.d>",
	"netlog <text>", "netconsole [on|off|host <ip>]", "dhcp", "netregs",
	"netloop", "netreset", "pong", "ps", "kill <pid>", "kexec", "dbg",
	"serial test|recv", "clear", "curl <url>", "netdns [a.b.c.d]",
	"host <name>",
};

static void help_puts(const char *s)
{
	while (*s)
		io_putchar(*s++);
}

static void help_item_pad(const char *s)
{
	unsigned int n = 0;

	while (s[n]) {
		io_putchar(s[n]);
		n++;
	}
	while (n < 32) {
		io_putchar(' ');
		n++;
	}
}

static void help_print(void)
{
	unsigned int nitems = sizeof(help_items) / sizeof(help_items[0]);
	unsigned int half = (nitems + 1) / 2;
	unsigned int k;

	for (k = 0; k < half; k++) {
		if (k + half < nitems) {
			help_item_pad(help_items[k]);
			help_puts(help_items[k + half]);
		} else {
			help_puts(help_items[k]);
		}
		io_putchar('\n');
	}
}

static int is_cmd_help(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 4 > line_len)
		return 0;
	if (line[i] != 'h' || line[i+1] != 'e' || line[i+2] != 'l' || line[i+3] != 'p')
		return 0;
	i += 4;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_clear(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 5 > line_len)
		return 0;
	if (line[i] != 'c' || line[i+1] != 'l' || line[i+2] != 'e' ||
	    line[i+3] != 'a' || line[i+4] != 'r')
		return 0;
	i += 5;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_ps(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 2 > line_len)
		return 0;
	if (line[i] != 'p' || line[i+1] != 's')
		return 0;
	i += 2;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_kill(const char *line, unsigned int line_len, unsigned int *pid_out)
{
	unsigned int i = 0, num = 0;
	int has_digit = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 4 > line_len)
		return 0;
	if (line[i] != 'k' || line[i+1] != 'i' || line[i+2] != 'l' || line[i+3] != 'l')
		return 0;
	i += 4;
	while (i < line_len && line[i] == ' ')
		i++;
	while (i < line_len && line[i] >= '0' && line[i] <= '9') {
		num = num * 10 + (unsigned int)(line[i] - '0');
		has_digit = 1;
		i++;
	}
	if (!has_digit)
		return 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i != line_len)
		return 0;
	*pid_out = num;
	return 1;
}

static int is_cmd_kexec(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 5 > line_len)
		return 0;
	if (line[i] != 'k' || line[i+1] != 'e' || line[i+2] != 'x' || line[i+3] != 'e' || line[i+4] != 'c')
		return 0;
	i += 5;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_dbg(const char *line, unsigned int line_len)
{
	unsigned int i = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 3 > line_len)
		return 0;
	if (line[i] != 'd' || line[i+1] != 'b' || line[i+2] != 'g')
		return 0;
	i += 3;
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

static int is_cmd_serial(const char *line, unsigned int line_len, char *arg_buf, unsigned int buf_size)
{
	unsigned int i = 0, j = 0;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i + 6 > line_len)
		return 0;
	if (line[i] != 's' || line[i+1] != 'e' || line[i+2] != 'r' ||
	    line[i+3] != 'i' || line[i+4] != 'a' || line[i+5] != 'l')
		return 0;
	i += 6;
	while (i < line_len && line[i] == ' ')
		i++;
	if (i >= line_len) {
		if (buf_size > 0)
			arg_buf[0] = '\0';
		return 1;
	}
	while (i < line_len && j + 1 < buf_size)
		arg_buf[j++] = line[i++];
	while (j > 0 && arg_buf[j - 1] == ' ')
		j--;
	arg_buf[j] = '\0';
	while (i < line_len && line[i] == ' ')
		i++;
	return (i == line_len);
}

/* Redraw line from index `from` to end, clear one cell after, set cursor to line_pos. */
static void redraw_line_from(const char *line, unsigned int line_len,
	unsigned int line_pos, unsigned int line_start, unsigned int from)
{
	unsigned int i;
	io_set_cursor(line_start + from);
	for (i = from; i < line_len; i++)
		io_putchar(line[i]);
	io_putchar(' ');
	io_set_cursor(line_start + line_pos);
}

/* Redraw entire line and clear through clear_through (so leftover chars are erased). */
static void redraw_line_clear(const char *line, unsigned int line_len,
	unsigned int line_pos, unsigned int line_start, unsigned int clear_through)
{
	unsigned int i;
	io_set_cursor(line_start);
	for (i = 0; i < line_len; i++)
		io_putchar(line[i]);
	for (i = line_len; i <= clear_through; i++)
		io_putchar(' ');
	io_set_cursor(line_start + line_pos);
}

/* Copy history entry (1 = newest) into t->line, redraw from start. */
static void history_load_into_line(struct term *t, unsigned int browse)
{
	unsigned int slot, len, old_len;
	const char *src;
	if (browse < 1 || browse > t->history_count)
		return;
	old_len = t->line_len;
	slot = (t->history_next + HISTORY_SIZE - browse) % HISTORY_SIZE;
	src = t->history[slot];
	for (len = 0; len < LINE_MAX - 1 && src[len]; len++)
		t->line[len] = src[len];
	t->line[len] = '\0';
	t->line_len = len;
	t->line_pos = len;
	redraw_line_clear(t->line, len, len, t->line_start, old_len > len ? old_len : len);
}

/* ---- Tab completion ---- */

/* Command names come from the help table ("ls [path]" -> "ls"). */
static unsigned int help_name_len(const char *s)
{
	unsigned int n = 0;
	while (s[n] && s[n] != ' ' && s[n] != '[' && s[n] != '<' && s[n] != '|')
		n++;
	return n;
}

static char lower_ch(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int prefix_ci(const char *s, const char *prefix, unsigned int plen)
{
	unsigned int i;
	for (i = 0; i < plen; i++)
		if (lower_ch(s[i]) != lower_ch(prefix[i]))
			return 0;
	return 1;
}

/* "HELLO   TXT" -> "hello.txt" (a completion-ready 8.3 display name). */
static void tab_display_name(const char *n, char *out)
{
	unsigned int bl = 0, el = 0, i, j = 0;

	for (i = 0; i < 8 && n[i] && n[i] != ' '; i++)
		bl = i + 1;
	for (i = 8; i < 11 && n[i] && n[i] != ' '; i++)
		el = i - 8 + 1;
	for (i = 0; i < bl; i++)
		out[j++] = lower_ch(n[i]);
	if (el) {
		out[j++] = '.';
		for (i = 0; i < el; i++)
			out[j++] = lower_ch(n[8 + i]);
	}
	out[j] = '\0';
}

/* Shared completion state (fat_list_dir takes no context pointer). */
static char tab_prefix[16];
static unsigned int tab_prefix_len;
static unsigned int tab_count;
static unsigned int tab_lcp_len;
static char tab_lcp[16];
static int tab_single_dir;
static fat_fs_t *tab_fs;
static unsigned int tab_dir_cluster;
static unsigned int tab_col;
static unsigned int tab_width;

static void tab_collect_cb(const char *name, int is_dir, unsigned int size)
{
	char disp[16];
	unsigned int i, n;

	(void)size;
	if (name[0] == '.')                 /* "." / ".." */
		return;
	tab_display_name(name, disp);
	if (!prefix_ci(disp, tab_prefix, tab_prefix_len))
		return;
	n = 0;
	while (disp[n])
		n++;
	if (tab_count == 0) {
		for (i = 0; i < n; i++)
			tab_lcp[i] = disp[i];
		tab_lcp_len = n;
		tab_single_dir = is_dir;
	} else {
		i = 0;
		while (i < tab_lcp_len && i < n && tab_lcp[i] == disp[i])
			i++;
		tab_lcp_len = i;
	}
	tab_count++;
}

static void tab_print_one(const char *name, int is_dir)
{
	unsigned int n = 0, w;

	while (name[n])
		n++;
	w = n + (is_dir ? 1 : 0);
	if (tab_col > 0 && tab_col + 2 + w > tab_width) {
		io_putchar('\n');
		tab_col = 0;
	}
	if (tab_col > 0) {
		io_putstr("  ");
		tab_col += 2;
	}
	io_putstr(name);
	if (is_dir)
		io_putchar('/');
	tab_col += w;
}

static void tab_print_cb(const char *name, int is_dir, unsigned int size)
{
	char disp[16];

	(void)size;
	if (name[0] == '.')
		return;
	tab_display_name(name, disp);
	if (!prefix_ci(disp, tab_prefix, tab_prefix_len))
		return;
	tab_print_one(disp, is_dir);
}

static int tab_scan_dir(const char *abs)
{
	fat_fs_t *fs;
	const char *fs_path;

	if (vfs_resolve(abs, &fs, &fs_path) != 0)
		return 0;
	if (fat_resolve_dir(fs, fs_path, &tab_dir_cluster) != 0)
		return 0;
	tab_fs = fs;
	tab_count = 0;
	tab_lcp_len = 0;
	fat_list_dir(fs, tab_dir_cluster, tab_collect_cb);
	return (int)tab_count;
}

static int tab_scan_mounts(void)
{
	unsigned int i;

	tab_count = 0;
	tab_lcp_len = 0;
	for (i = 0; i < vfs_mount_count; i++) {
		const char *name = vfs_mounts[i].name;
		unsigned int n = 0, j;

		while (name[n])
			n++;
		if (!prefix_ci(name, tab_prefix, tab_prefix_len))
			continue;
		if (tab_count == 0) {
			for (j = 0; j < n; j++)
				tab_lcp[j] = name[j];
			tab_lcp_len = n;
		} else {
			j = 0;
			while (j < tab_lcp_len && j < n && tab_lcp[j] == name[j])
				j++;
			tab_lcp_len = j;
		}
		tab_count++;
	}
	return (int)tab_count;
}

/* Replace line[start,end) with ins, keep the tail and redraw. */
static void term_replace(struct term *t, unsigned int start, unsigned int end,
			 const char *ins)
{
	unsigned int inslen = 0, tail, i, old_len = t->line_len;

	while (ins[inslen])
		inslen++;
	tail = old_len - end;
	if (start + inslen + tail >= LINE_MAX)
		return;
	for (i = 0; i < tail; i++)
		t->line[start + inslen + i] = t->line[end + i];
	for (i = 0; i < inslen; i++)
		t->line[start + i] = ins[i];
	t->line_len = start + inslen + tail;
	t->line[t->line_len] = '\0';
	t->line_pos = start + inslen;
	redraw_line_clear(t->line, t->line_len, t->line_pos, t->line_start,
			  old_len > t->line_len ? old_len : t->line_len);
}

/* List the ambiguous matches below the line, then redraw prompt + line. */
static void term_tab_ambiguous(struct term *t, int mounts)
{
	unsigned int i;

	io_putchar('\n');
	tab_col = 0;
	tab_width = t->screen.cols;
	if (mounts) {
		for (i = 0; i < vfs_mount_count; i++) {
			const char *name = vfs_mounts[i].name;
			if (prefix_ci(name, tab_prefix, tab_prefix_len))
				tab_print_one(name, 1);
		}
	} else {
		fat_list_dir(tab_fs, tab_dir_cluster, tab_print_cb);
	}
	io_putchar('\n');
	term_print_prompt(t, 1);
	redraw_line_clear(t->line, t->line_len, t->line_pos, t->line_start, t->line_len);
}

/* Complete the token at the cursor: command name in the first token, a VFS
 * path afterwards.  One match completes, several extend to the common
 * prefix or are listed below the line. */
static void term_complete(struct term *t)
{
	char token[LINE_MAX + 1];
	char ins[LINE_MAX + 1];
	char dir[VFS_PATH_MAX];
	unsigned int start = t->line_pos, end, tl, i, j;

	while (start > 0 && t->line[start - 1] != ' ')
		start--;
	end = t->line_pos;
	while (end < t->line_len && t->line[end] != ' ')
		end++;
	tl = end - start;
	for (i = 0; i < tl; i++)
		token[i] = t->line[start + i];
	token[tl] = '\0';

	/* First token -> command name. */
	{
		int first = 1;
		for (i = 0; i < start; i++)
			if (t->line[i] != ' ') {
				first = 0;
				break;
			}
		if (first) {
			unsigned int nitems = sizeof(help_items) / sizeof(help_items[0]);
			unsigned int count = 0, lcp_len = 0, best = 0;
			char lcp[16];

			for (i = 0; i < nitems; i++) {
				unsigned int nl = help_name_len(help_items[i]);

				if (nl < tl || !prefix_ci(help_items[i], token, tl))
					continue;
				if (count == 0) {
					for (j = 0; j < nl; j++)
						lcp[j] = help_items[i][j];
					lcp_len = nl;
					best = i;
				} else {
					for (j = 0; j < lcp_len && j < nl &&
						     lcp[j] == help_items[i][j]; j++)
						;
					lcp_len = j;
				}
				count++;
			}
			if (count == 0)
				return;
			if (count == 1) {
				unsigned int nl = help_name_len(help_items[best]);
				for (j = 0; j < nl; j++)
					ins[j] = help_items[best][j];
				ins[nl] = ' ';
				ins[nl + 1] = '\0';
				term_replace(t, start, end, ins);
				return;
			}
			if (lcp_len > tl) {
				for (j = 0; j < lcp_len; j++)
					ins[j] = lcp[j];
				ins[lcp_len] = '\0';
				term_replace(t, start, end, ins);
				return;
			}
			io_putchar('\n');
			tab_col = 0;
			tab_width = t->screen.cols;
			for (i = 0; i < nitems; i++) {
				unsigned int nl = help_name_len(help_items[i]);
				char name[16];
				if (nl < tl || !prefix_ci(help_items[i], token, tl))
					continue;
				for (j = 0; j < nl && j < 15; j++)
					name[j] = help_items[i][j];
				name[j] = '\0';
				tab_print_one(name, 0);
			}
			io_putchar('\n');
			term_print_prompt(t, 1);
			redraw_line_clear(t->line, t->line_len, t->line_pos,
					  t->line_start, t->line_len);
			return;
		}
	}

	/* Argument token -> VFS path. */
	{
		char dirpart[LINE_MAX + 1];
		const char *last = 0, *p;
		unsigned int dp, n;
		int mounts = 0;

		for (p = token; *p; p++)
			if (*p == '/')
				last = p;
		if (last) {
			dp = (unsigned int)(last - token) + 1;
			for (i = 0; i < dp; i++)
				dirpart[i] = token[i];
			dirpart[dp] = '\0';
			p = last + 1;
		} else {
			dp = 0;
			dirpart[0] = '\0';
			p = token;
		}
		{
			unsigned int k = 0;
			while (*p && k < 15)
				tab_prefix[k++] = *p++;
			tab_prefix[k] = '\0';
			tab_prefix_len = k;
		}

		if (dirpart[0] == '/' && dirpart[1] == '\0')
			mounts = 1;
		if (dirpart[0] == '/') {
			if (dp >= VFS_PATH_MAX)
				return;
			for (i = 0; dirpart[i]; i++)
				dir[i] = dirpart[i];
			dir[i] = '\0';
		} else {
			if (vfs_join_path(t->cwd, dirpart, dir, sizeof(dir)) != 0)
				return;
		}

		n = mounts ? (unsigned int)tab_scan_mounts()
			   : (unsigned int)tab_scan_dir(dir);
		if (n == 0)
			return;
		if (dp + tab_lcp_len + 2 > LINE_MAX)
			return;
		if (n == 1) {
			for (i = 0; i < dp; i++)
				ins[i] = dirpart[i];
			for (i = 0; i < tab_lcp_len; i++)
				ins[dp + i] = tab_lcp[i];
			ins[dp + tab_lcp_len] = (mounts || tab_single_dir) ? '/' : ' ';
			ins[dp + tab_lcp_len + 1] = '\0';
			term_replace(t, start, end, ins);
			return;
		}
		if (tab_lcp_len > tab_prefix_len) {
			for (i = 0; i < dp; i++)
				ins[i] = dirpart[i];
			for (i = 0; i < tab_lcp_len; i++)
				ins[dp + i] = tab_lcp[i];
			ins[dp + tab_lcp_len] = '\0';
			term_replace(t, start, end, ins);
			return;
		}
		term_tab_ambiguous(t, mounts);
	}
}

/* Push current line to history if non-empty (and not duplicate of newest). */
static void history_push(struct term *t, const char *line, unsigned int line_len)
{
	unsigned int i, slot;
	int all_spaces = 1;
	if (line_len == 0)
		return;
	for (i = 0; i < line_len; i++) {
		if (line[i] != ' ') {
			all_spaces = 0;
			break;
		}
	}
	if (all_spaces)
		return;
	if (t->history_count > 0) {
		slot = (t->history_next + HISTORY_SIZE - 1) % HISTORY_SIZE;
		for (i = 0; i < line_len && i < LINE_MAX - 1 && t->history[slot][i]; i++) {
			if (t->history[slot][i] != line[i])
				break;
		}
		if (i == line_len && t->history[slot][i] == '\0')
			return;  /* duplicate of newest */
	}
	slot = t->history_next;
	for (i = 0; i < line_len && i < LINE_MAX - 1; i++)
		t->history[slot][i] = line[i];
	t->history[slot][i] = '\0';
	t->history_next = (t->history_next + 1) % HISTORY_SIZE;
	if (t->history_count < HISTORY_SIZE)
		t->history_count++;
}

/* Change directory: resolve `arg` (relative to t->cwd, or absolute) and
 * validate it is an existing directory. Updates t->cwd on success. */
static void term_cd(struct term *t, const char *arg)
{
	char abs[VFS_PATH_MAX];
	fat_fs_t *fs;
	const char *fs_path;
	unsigned int dir;
	int r;
	unsigned int i;

	if (arg[0] == '\0') {
		t->cwd[0] = '/';
		t->cwd[1] = '\0';
		return;
	}

	if (vfs_join_path(t->cwd, arg, abs, sizeof(abs)) != 0) {
		io_putstr("cd: path too long\n");
		return;
	}

	if (abs[0] == '/' && abs[1] == '\0') {
		t->cwd[0] = '/';
		t->cwd[1] = '\0';
		return;
	}

	if (vfs_resolve(abs, &fs, &fs_path) != 0) {
		io_putstr("cd: no such directory\n");
		return;
	}
	r = fat_resolve_dir(fs, fs_path, &dir);
	if (r == -2) {
		io_putstr("cd: disk error\n");
		return;
	}
	if (r != 0) {
		io_putstr("cd: no such directory\n");
		return;
	}

	for (i = 0; abs[i] && i < sizeof(t->cwd) - 1; i++)
		t->cwd[i] = abs[i];
	t->cwd[i] = '\0';
}

/* Resolve the first token (path) of `args` against t->cwd, rebuilding
 * "abs-path rest-of-args" into out. Used by `save`. */
static void term_resolve_first(struct term *t, const char *args,
			       char *out, unsigned int out_size)
{
	char path[VFS_PATH_MAX];
	char abs[VFS_PATH_MAX];
	unsigned int i = 0, j = 0;

	while (args[i] == ' ')
		i++;
	while (args[i] && args[i] != ' ' && j < sizeof(path) - 1)
		path[j++] = args[i++];
	path[j] = '\0';

	if (vfs_join_path(t->cwd, path, abs, sizeof(abs)) != 0) {
		abs[0] = '/';
		abs[1] = '\0';
	}
	while (args[i] == ' ')
		i++;

	j = 0;
	{
		unsigned int k = 0;
		while (abs[k] && j + 1 < out_size)
			out[j++] = abs[k++];
	}
	if (args[i]) {
		if (j + 1 < out_size)
			out[j++] = ' ';
		while (args[i] && j + 1 < out_size)
			out[j++] = args[i++];
	}
	out[j] = '\0';
}

/* Spawn a foreground command process; the caller (term_run's wait loop)
 * waits for it to finish and handles Ctrl+C. */
static void term_spawn(struct term *t, const char *name, void (*entry)(void))
{
	int pid = process_create(name, entry);
	if (pid < 0)
		io_putstr("error: too many processes\n");
	else
		t->fg_pid = pid;
}

static void term_spawn_arg(struct term *t, const char *name,
			   void (*entry)(const char *), const char *arg)
{
	int pid = process_create_arg(name, entry, arg);
	if (pid < 0)
		io_putstr("error: too many processes\n");
	else
		t->fg_pid = pid;
}

void term_run(struct term *t)
{
	t->line_len         = 0;
	t->line_pos         = 0;
	t->line[0]          = '\0';
	t->history_browse   = 0;
	t->fg_pid           = -1;

	io_clear_screen();
	term_print_prompt(t, 1);

	for (;;) {
		if (t->fg_pid >= 0) {
			/* Waiting on a foreground child: poll keys for Ctrl+C
			 * and watch for the child to exit. */
			int k = term_key_pop(t);
			if (k == IO_KEY_CTRL_C) {
				int killed = (process_kill((unsigned int)t->fg_pid) == 0);
				t->fg_pid = -1;
				if (killed)
					io_putstr("^C\n");
				term_print_prompt(t, 0);
			} else if (!process_is_alive((unsigned int)t->fg_pid)) {
				t->fg_pid = -1;
				term_print_prompt(t, 0);
			} else {
				process_sleep_ms(10);
			}
			continue;
		}
		{
			int k = term_key_pop(t);
			if (k) {
				kdbg_term_count++;
				kdbg_term_last = (unsigned int)k;
				kdbg_status();
				if (k == IO_KEY_CTRL_C) {
					/* Ctrl+C at the prompt: discard the line. */
					io_putstr("^C\n");
					t->line_len = 0;
					t->line_pos = 0;
					t->line[0]  = '\0';
					term_print_prompt(t, 0);
				} else if (k == IO_KEY_LEFT) {
					if (t->line_pos > 0) {
						t->line_pos--;
						io_set_cursor(t->line_start + t->line_pos);
					}
				} else if (k == IO_KEY_RIGHT) {
					if (t->line_pos < t->line_len) {
						t->line_pos++;
						io_set_cursor(t->line_start + t->line_pos);
					}
				} else if (k == IO_KEY_UP) {
					if (t->history_count > 0) {
						if (t->history_browse < t->history_count)
							t->history_browse++;
						history_load_into_line(t, t->history_browse);
					}
				} else if (k == IO_KEY_DOWN) {
					if (t->history_browse > 0) {
						t->history_browse--;
						if (t->history_browse == 0) {
							unsigned int clear_len = t->line_len;
							t->line_len = 0;
							t->line_pos = 0;
							t->line[0]  = '\0';
							redraw_line_clear(t->line, 0, 0, t->line_start, clear_len);
						} else {
							history_load_into_line(t, t->history_browse);
						}
					}
				} else if (k == '\b') {
					if (t->line_pos > 0) {
						unsigned int i;
						t->line_pos--;
						for (i = t->line_pos; i < t->line_len; i++)
							t->line[i] = t->line[i + 1];
						t->line_len--;
						t->line[t->line_len] = '\0';
						redraw_line_from(t->line, t->line_len, t->line_pos, t->line_start, t->line_pos);
					}
				} else if (k == '\t') {
					term_complete(t);
				} else if (k == '\n') {
					char cat_fname[CAT_FNAME_MAX];
					char save_args[SAVE_ARGS_MAX];
					char rm_fname[RM_FNAME_MAX];
					char ls_path[LS_PATH_MAX];
					char format_dev[FORMAT_DEV_MAX];
					char netip_arg[16];
					char ping_arg[64];
					char netlog_arg[NET_UDP_ARG_MAX];
					char netconsole_arg[NET_UDP_ARG_MAX];
					char curl_arg[128];
					char netdns_arg[32];
					char host_arg[64];
					char lspci_arg[16];
					char peek_arg[32];
					char resolved[VFS_PATH_MAX];
					char save_resolved[SAVE_ARGS_MAX + VFS_PATH_MAX];
					unsigned int kill_pid;
					int skip_prompt = 0;
					io_putchar('\n');
					history_push(t, t->line, t->line_len);
					t->history_browse = 0;
					if (is_cmd_hello(t->line, t->line_len)) {
						int pid = process_create_user("hello",
							_binary_hello_elf_start,
							(unsigned int)(_binary_hello_elf_end -
								_binary_hello_elf_start));
						if (pid < 0)
							io_putstr("error: too many processes\n");
						else
							t->fg_pid = pid;
					} else if (is_cmd_reboot(t->line, t->line_len)) {
						program_reboot_main();
					} else if (is_cmd_poweroff(t->line, t->line_len)) {
						program_poweroff_main();
					} else if (is_cmd_cd(t->line, t->line_len, ls_path, LS_PATH_MAX)) {
						term_cd(t, ls_path);
					} else if (is_cmd_ls(t->line, t->line_len, ls_path, LS_PATH_MAX)) {
						if (vfs_join_path(t->cwd, ls_path, resolved, sizeof(resolved)) == 0) {
							term_spawn_arg(t, "ls", program_ls_main, resolved);
						} else {
							io_putstr("ls: path too long\n");
						}
					} else if (is_cmd_lsusb(t->line, t->line_len)) {
						term_spawn(t, "lsusb", program_lsusb_main);
					} else if (is_cmd_lspci(t->line, t->line_len, lspci_arg, sizeof(lspci_arg))) {
						term_spawn_arg(t, "lspci", program_lspci_main, lspci_arg);
					} else if (is_cmd_peek(t->line, t->line_len, peek_arg, sizeof(peek_arg))) {
						term_spawn_arg(t, "peek", program_peek_main, peek_arg);
					} else if (is_cmd_poke(t->line, t->line_len, peek_arg, sizeof(peek_arg))) {
						term_spawn_arg(t, "poke", program_poke_main, peek_arg);
					} else if (is_cmd_netinfo(t->line, t->line_len)) {
						term_spawn(t, "netinfo", program_netinfo_main);
					} else if (is_cmd_netrecv(t->line, t->line_len)) {
						term_spawn(t, "netrecv", program_netrecv_main);
					} else if (is_cmd_netsend(t->line, t->line_len)) {
						term_spawn(t, "netsend", program_netsend_main);
					} else if (is_cmd_netip(t->line, t->line_len, netip_arg, sizeof(netip_arg))) {
						term_spawn_arg(t, "netip", program_netip_main, netip_arg);
					} else if (is_cmd_ping(t->line, t->line_len, ping_arg, sizeof(ping_arg))) {
						term_spawn_arg(t, "ping", program_ping_main, ping_arg);
					} else if (is_cmd_netlog(t->line, t->line_len, netlog_arg, sizeof(netlog_arg))) {
						term_spawn_arg(t, "netlog", program_netlog_main, netlog_arg);
					} else if (is_cmd_netconsole(t->line, t->line_len, netconsole_arg, sizeof(netconsole_arg))) {
						term_spawn_arg(t, "netconsole", program_netconsole_main, netconsole_arg);
					} else if (is_cmd_dhcp(t->line, t->line_len)) {
						term_spawn(t, "dhcp", program_dhcp_main);
					} else if (is_cmd_netregs(t->line, t->line_len)) {
						term_spawn(t, "netregs", program_netregs_main);
					} else if (is_cmd_netloop(t->line, t->line_len)) {
						term_spawn(t, "netloop", program_netloop_main);
					} else if (is_cmd_netreset(t->line, t->line_len)) {
						term_spawn(t, "netreset", program_netreset_main);
					} else if (is_cmd_netdns(t->line, t->line_len, netdns_arg, sizeof(netdns_arg))) {
						term_spawn_arg(t, "netdns", program_netdns_main, netdns_arg);
					} else if (is_cmd_curl(t->line, t->line_len, curl_arg, sizeof(curl_arg))) {
						term_spawn_arg(t, "curl", program_curl_main, curl_arg);
					} else if (is_cmd_host(t->line, t->line_len, host_arg, sizeof(host_arg))) {
						term_spawn_arg(t, "host", program_host_main, host_arg);
					} else if (is_cmd_pong(t->line, t->line_len)) {
						term_spawn(t, "pong", program_pong_main);
					} else if (is_cmd_storage(t->line, t->line_len)) {
						term_spawn(t, "storage", program_storage_main);
					} else if (is_cmd_meminfo(t->line, t->line_len)) {
						term_spawn(t, "meminfo", program_mem_main);
					} else if (is_cmd_clock(t->line, t->line_len)) {
						io_putstr("clock: not available in windowed mode yet\n");
					} else if (is_cmd_colors(t->line, t->line_len)) {
						io_putstr("colors: not available in windowed mode yet\n");
					} else if (is_cmd_cat(t->line, t->line_len, cat_fname, CAT_FNAME_MAX)) {
						if (vfs_join_path(t->cwd, cat_fname, resolved, sizeof(resolved)) == 0) {
							term_spawn_arg(t, "cat", program_cat_main, resolved);
						} else {
							io_putstr("cat: path too long\n");
						}
					} else if (is_cmd_run(t->line, t->line_len, cat_fname, CAT_FNAME_MAX)) {
						if (cat_fname[0] == 's' && cat_fname[1] == 'e' &&
						    cat_fname[2] == 'r' && cat_fname[3] == 'i' &&
						    cat_fname[4] == 'a' && cat_fname[5] == 'l' &&
						    cat_fname[6] == '\0') {
							if (process_create_arg("run_serial", program_run_serial_main, cat_fname) < 0)
								io_putstr("error: too many processes\n");
						} else {
							io_putstr("run: not available in windowed mode yet\n");
						}
					} else if (is_cmd_save(t->line, t->line_len, save_args, SAVE_ARGS_MAX)) {
						term_resolve_first(t, save_args, save_resolved, sizeof(save_resolved));
						term_spawn_arg(t, "save", program_save_main, save_resolved);
					} else if (is_cmd_mkdir(t->line, t->line_len, rm_fname, RM_FNAME_MAX)) {
						if (vfs_join_path(t->cwd, rm_fname, resolved, sizeof(resolved)) == 0) {
							term_spawn_arg(t, "mkdir", program_mkdir_main, resolved);
						} else {
							io_putstr("mkdir: path too long\n");
						}
					} else if (is_cmd_rmdir(t->line, t->line_len, rm_fname, RM_FNAME_MAX)) {
						if (vfs_join_path(t->cwd, rm_fname, resolved, sizeof(resolved)) == 0) {
							term_spawn_arg(t, "rmdir", program_rmdir_main, resolved);
						} else {
							io_putstr("rmdir: path too long\n");
						}
					} else if (is_cmd_rm(t->line, t->line_len, rm_fname, RM_FNAME_MAX)) {
						if (vfs_join_path(t->cwd, rm_fname, resolved, sizeof(resolved)) == 0) {
							term_spawn_arg(t, "rm", program_rm_main, resolved);
						} else {
							io_putstr("rm: path too long\n");
						}
					} else if (is_cmd_space(t->line, t->line_len)) {
						term_spawn_arg(t, "space", program_run_embedded_main, "space");
					} else if (is_cmd_prog(t->line, t->line_len)) {
						term_spawn_arg(t, "prog", program_run_embedded_main, "prog");
					} else if (is_cmd_display(t->line, t->line_len)) {
						term_spawn(t, "display", program_display_main);
					} else if (is_cmd_cpuinfo(t->line, t->line_len)) {
						term_spawn(t, "cpuinfo", program_cpuinfo_main);
					} else if (is_cmd_mouseinfo(t->line, t->line_len)) {
						term_spawn(t, "mouseinfo", program_mouseinfo_main);
					} else if (is_cmd_format(t->line, t->line_len, format_dev, FORMAT_DEV_MAX)) {
						program_format_main(format_dev);
					} else if (is_cmd_ps(t->line, t->line_len)) {
						process_ps();
					} else if (is_cmd_kill(t->line, t->line_len, &kill_pid)) {
						if (process_kill(kill_pid) < 0)
							io_putstr("kill: no such process\n");
						else
							io_putstr("killed\n");
					} else if (is_cmd_kexec(t->line, t->line_len)) {
						if (kexec_busy) {
							io_putstr("kexec: already receiving\n");
						} else {
							kexec_busy = 1;
							if (process_create("kexec", program_kexec_main) < 0) {
								kexec_busy = 0;
								io_putstr("error: too many processes\n");
							}
						}
					} else if (is_cmd_dbg(t->line, t->line_len)) {
						program_dbg_main();
					} else if (is_cmd_help(t->line, t->line_len)) {
						help_print();
				} else if (is_cmd_clear(t->line, t->line_len)) {
					io_clear_screen();
					term_print_prompt(t, 1);
					skip_prompt = 1;
				} else {
					char serial_arg[8];
					if (is_cmd_serial(t->line, t->line_len, serial_arg, sizeof(serial_arg))) {
						if (serial_arg[0] == 't' && serial_arg[1] == 'e' &&
						    serial_arg[2] == 's' && serial_arg[3] == 't' &&
						    serial_arg[4] == '\0') {
							if (process_create_arg("serial_loop", program_serial_loop_main, serial_arg) < 0)
								io_putstr("error: too many processes\n");
						} else if (serial_arg[0] == 'r' && serial_arg[1] == 'e' &&
						           serial_arg[2] == 'c' && serial_arg[3] == 'v' &&
						           serial_arg[4] == '\0') {
							program_serial_recv_main(serial_arg);
						} else {
							io_putstr("serial: usage serial test|recv\n");
						}
					} else {
						unsigned int i = 0;
						while (i < t->line_len && t->line[i] == ' ')
							i++;
						if (i < t->line_len)
							io_putstr("command not found\n");
					}
				}
				t->line_len = 0;
				t->line_pos = 0;
				t->line[0]  = '\0';
				if (!skip_prompt && t->fg_pid < 0) {
					term_print_prompt(t, 0);
				}
			} else {
				unsigned char u = (unsigned char)k;
				if (u >= 0x20 && u < 0x7F && t->line_len < LINE_MAX - 1) {
					unsigned int i;
					t->history_browse = 0;
					for (i = t->line_len; i > t->line_pos; i--)
						t->line[i] = t->line[i - 1];
					t->line[t->line_pos] = (char)k;
					t->line_len++;
					t->line[t->line_len] = '\0';
					t->line_pos++;
					redraw_line_from(t->line, t->line_len, t->line_pos, t->line_start, t->line_pos - 1);
				}
			}
		} else {
			process_sleep_ms(10);
		}
	}
}
}

void term_entry(void *arg)
{
	struct term *t = (struct term *)arg;
	process_set_screen(&t->screen);
	term_run(t);
}
