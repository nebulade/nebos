#include "mouse.h"
#include "timer.h"
#include "vga.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

#define STATUS_OUT_FULL 0x01
#define STATUS_IN_FULL  0x02
#define STATUS_AUX_DATA 0x20

#define MOUSE_ACK     0xFA
#define MOUSE_RESEND  0xFE

#define MOUSE_CUR_W 16
#define MOUSE_CUR_H 16

static const unsigned short mouse_pointer_bits[MOUSE_CUR_H] = {
	0x0000,
	0x8000, 0xC000, 0xE000, 0xF000, 0xF800, 0xFC00, 0xFE00, 0xFF00,
	0xFF80, 0x9780, 0x0380, 0x0180, 0x0180, 0x0000, 0x0000
};

static int mouse_present_flag;
static int mouse_streaming_flag;
static unsigned int mouse_id;
static int mouse_cursor_flag;

static unsigned int fbw, fbh;
static unsigned int cur_x, cur_y;
static unsigned char cur_saved[MOUSE_CUR_W * MOUSE_CUR_H * 4];
static unsigned char pkt[3];
static unsigned int pkt_idx;

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

static void enable_aux_port(void)
{
	ps2_wait_input_empty();
	outb(PS2_CMD, 0xA8);
}

static void mouse_cmd_write(unsigned char val)
{
	ps2_wait_input_empty();
	outb(PS2_CMD, 0xD4);
	ps2_wait_input_empty();
	outb(PS2_DATA, val);
}

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
	if (b == MOUSE_RESEND)
		return -2;
	if (b != MOUSE_ACK)
		return -1;
	return 0;
}

static int mouse_send_cmd_ack(unsigned char cmd)
{
	int r;
	mouse_cmd_write(cmd);
	r = mouse_expect_ack();
	if (r == -2) {
		mouse_cmd_write(cmd);
		r = mouse_expect_ack();
	}
	return r;
}

static int mouse_bit_at(int r, int c)
{
	if (r < 0 || c < 0 || r >= (int)MOUSE_CUR_H || c >= (int)MOUSE_CUR_W)
		return 0;
	return (mouse_pointer_bits[(unsigned int)r] & (0x8000u >> c)) != 0;
}

static void mouse_draw_cursor(unsigned int x0, unsigned int y0)
{
	unsigned int r, c;

	for (r = 0; r < MOUSE_CUR_H; r++) {
		for (c = 0; c < MOUSE_CUR_W; c++) {
			if (mouse_bit_at((int)r, (int)c))
				vga_put_pixel_rgb(x0 + c, y0 + r, 255, 255, 255);
		}
	}
	for (r = 0; r < MOUSE_CUR_H; r++) {
		for (c = 0; c < MOUSE_CUR_W; c++) {
			if (!mouse_bit_at((int)r, (int)c)) {
				if (mouse_bit_at((int)r - 1, (int)c) ||
				    mouse_bit_at((int)r + 1, (int)c) ||
				    mouse_bit_at((int)r, (int)c - 1) ||
				    mouse_bit_at((int)r, (int)c + 1))
					vga_put_pixel_rgb(x0 + c, y0 + r, 0, 0, 0);
			}
		}
	}
}

static void mouse_clamp_ptr(long *nx, long *ny)
{
	long maxx = (long)fbw - (long)MOUSE_CUR_W;
	long maxy = (long)fbh - (long)MOUSE_CUR_H;

	if (maxx < 0)
		maxx = 0;
	if (maxy < 0)
		maxy = 0;
	if (*nx < 0)
		*nx = 0;
	else if (*nx > maxx)
		*nx = maxx;
	if (*ny < 0)
		*ny = 0;
	else if (*ny > maxy)
		*ny = maxy;
}

static void mouse_cursor_setup(void)
{
	mouse_cursor_flag = 0;
	if (!vga_has_vesa())
		return;
	fbw = vga_fb_width();
	fbh = vga_fb_height();
	if (fbw < MOUSE_CUR_W || fbh < MOUSE_CUR_H)
		return;
	/* Start cursor at a fixed visible spot (clamped to the framebuffer).
	   Using a constant offset avoids any overlap with the terminal header/prompt. */
	cur_x = 100;
	cur_y = 100;
	{
		long nx = (long)cur_x;
		long ny = (long)cur_y;
		mouse_clamp_ptr(&nx, &ny);
		cur_x = (unsigned int)nx;
		cur_y = (unsigned int)ny;
	}
	vga_fb_read_rect(cur_x, cur_y, MOUSE_CUR_W, MOUSE_CUR_H, cur_saved);
	mouse_draw_cursor(cur_x, cur_y);
	mouse_cursor_flag = 1;
}

void mouse_init(void)
{
	unsigned char id = 0;

	mouse_present_flag = 0;
	mouse_streaming_flag = 0;
	mouse_id = 0;
	mouse_cursor_flag = 0;
	pkt_idx = 0;

	enable_aux_port();

	if (mouse_send_cmd_ack(0xF5) != 0)
		return;

	mouse_cmd_write(0xF2);
	if (mouse_expect_ack() != 0)
		return;
	if (mouse_read_aux(&id, 500) != 0)
		return;

	mouse_present_flag = 1;
	mouse_id = id;
	mouse_cursor_setup();

	if (mouse_send_cmd_ack(0xF4) != 0)
		return;
	mouse_streaming_flag = 1;
}

void mouse_poll_update(void)
{
	/* If video mode info wasn't ready at mouse_init time, try again. */
	if (mouse_present_flag && !mouse_cursor_flag)
		mouse_cursor_setup();
	if (!mouse_streaming_flag)
		return;

	for (;;) {
		unsigned char st = inb(PS2_STATUS);
		unsigned char b0;
		int dx, dy;

		if ((st & (STATUS_OUT_FULL | STATUS_AUX_DATA)) !=
		    (STATUS_OUT_FULL | STATUS_AUX_DATA))
			return;

		pkt[pkt_idx++] = inb(PS2_DATA);
		if (pkt_idx < 3)
			continue;
		pkt_idx = 0;

		b0 = pkt[0];
		dx = (int)(signed char)pkt[1];
		dy = (int)(signed char)pkt[2];
		if (b0 & 0x10)
			dx |= ~0xFF;
		if (b0 & 0x20)
			dy |= ~0xFF;
		if ((b0 & 0xC0) != 0)
			continue;

		if (mouse_cursor_flag) {
			long nx = (long)cur_x + dx;
			long ny = (long)cur_y - dy;
			mouse_clamp_ptr(&nx, &ny);
			if ((unsigned int)nx != cur_x || (unsigned int)ny != cur_y) {
				vga_fb_write_rect(cur_x, cur_y, MOUSE_CUR_W, MOUSE_CUR_H,
					cur_saved);
				cur_x = (unsigned int)nx;
				cur_y = (unsigned int)ny;
				vga_fb_read_rect(cur_x, cur_y, MOUSE_CUR_W, MOUSE_CUR_H,
					cur_saved);
				mouse_draw_cursor(cur_x, cur_y);
			}
		}
	}
}

int mouse_is_present(void)
{
	return mouse_present_flag;
}

int mouse_streaming_enabled(void)
{
	return mouse_streaming_flag;
}

unsigned int mouse_device_id(void)
{
	return mouse_id;
}

int mouse_cursor_visible(void)
{
	return mouse_cursor_flag;
}
