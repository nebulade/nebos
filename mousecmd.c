/* mouse command: poll PS/2 aux (mouse) for 10s, print packets (relative dx/dy + accumulators). */

#include "io.h"
#include "timer.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

#define STATUS_OUT_FULL 0x01
#define STATUS_IN_FULL  0x02
#define STATUS_AUX_DATA 0x20

#define MOUSE_ACK     0xFA
#define MOUSE_RESEND  0xFE

static inline unsigned char inb(unsigned short port)
{
	unsigned char v;
	__asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
	return v;
}

static inline void outb(unsigned short port, unsigned char v)
{
	__asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}

static void ps2_wait_input_empty(void)
{
	unsigned spin = 100000;
	while (spin-- && (inb(PS2_STATUS) & STATUS_IN_FULL))
		;
}

static void mouse_cmd_write(unsigned char val)
{
	ps2_wait_input_empty();
	outb(PS2_CMD, 0xD4);
	ps2_wait_input_empty();
	outb(PS2_DATA, val);
}

/* Read next byte from aux port (trackpoint/touchpad), with timeout in timer ticks (~ms). */
static int mouse_read_aux(unsigned char *out, unsigned int timeout_ticks)
{
	unsigned int t0 = get_ticks_ms();
	while (get_ticks_ms() - t0 < timeout_ticks) {
		unsigned char st = inb(PS2_STATUS);
		if ((st & (STATUS_OUT_FULL | STATUS_AUX_DATA)) ==
		    (STATUS_OUT_FULL | STATUS_AUX_DATA)) {
			*out = inb(PS2_DATA);
			return 0;
		}
	}
	return -1;
}

static int mouse_expect_ack(void)
{
	unsigned char b;
	if (mouse_read_aux(&b, 500) != 0)
		return -1;
	if (b == MOUSE_RESEND) {
		/* one retry path left to caller */
		return -2;
	}
	if (b != MOUSE_ACK)
		return -1;
	return 0;
}

static void enable_aux_port(void)
{
	ps2_wait_input_empty();
	outb(PS2_CMD, 0xA8);
}

static int mouse_enable_streaming(void)
{
	int r;
	enable_aux_port();
	mouse_cmd_write(0xF4);
	r = mouse_expect_ack();
	if (r == -2) {
		mouse_cmd_write(0xF4);
		r = mouse_expect_ack();
	}
	return r;
}

static void mouse_disable_streaming(void)
{
	mouse_cmd_write(0xF5);
	(void)mouse_expect_ack();
}

static void put_uint(unsigned int n)
{
	char buf[12];
	int i = 10;
	buf[11] = '\0';
	if (n == 0) {
		io_putchar('0');
		return;
	}
	while (n && i >= 0) {
		buf[i--] = (char)('0' + (n % 10u));
		n /= 10u;
	}
	io_putstr(buf + i + 1);
}

static void put_sdec(int v)
{
	if (v < 0) {
		io_putchar('-');
		put_uint(0u - (unsigned int)v);
	} else
		put_uint((unsigned int)v);
}

void program_mouse_main(void)
{
	enum { RUN_MS = 10000 };
	unsigned int t_start;
	int acc_x = 0, acc_y = 0;
	unsigned char pkt[3];
	unsigned int pkt_idx = 0;

	io_putstr("PS/2 mouse: 10s (aux poll, 3-byte packets)\n");

	if (mouse_enable_streaming() != 0) {
		io_putstr("mouse: enable failed (no ACK?)\n");
		return;
	}

	t_start = get_ticks_ms();
	while (get_ticks_ms() - t_start < RUN_MS) {
		unsigned char st = inb(PS2_STATUS);
		if ((st & (STATUS_OUT_FULL | STATUS_AUX_DATA)) ==
		    (STATUS_OUT_FULL | STATUS_AUX_DATA)) {
			unsigned char b = inb(PS2_DATA);
			pkt[pkt_idx++] = b;
			if (pkt_idx == 3) {
				unsigned char b0 = pkt[0];
				int dx = (int)(signed char)pkt[1];
				int dy = (int)(signed char)pkt[2];
				if (b0 & 0x10)
					dx |= ~0xFF;
				if (b0 & 0x20)
					dy |= ~0xFF;
				/* overflow bits in b0: do not add to accumulators */
				if (!(b0 & 0xC0)) {
					acc_x += dx;
					acc_y += dy;
				}
				put_uint(get_ticks_ms() - t_start);
				io_putchar(' ');
				put_uint(b0 & 7);
				io_putchar(' ');
				put_sdec(dx);
				io_putchar(' ');
				put_sdec(dy);
				io_putchar(' ');
				put_sdec(acc_x);
				io_putchar(' ');
				put_sdec(acc_y);
				io_putchar('\n');
				pkt_idx = 0;
			}
		}
	}

	mouse_disable_streaming();
	io_putstr("PS/2 mouse: done\n");
}
