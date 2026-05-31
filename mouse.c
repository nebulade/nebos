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

/* Simple triangle cursor bitmap, 16×16.
   Hotspot is the top-left pixel of the 16×16 box (x0,y0).
   Each row is encoded as 16 bits; MSB (bit 15) maps to c=0. */
static const unsigned short mouse_pointer_bits[MOUSE_CUR_H] = {
	0x8000,  /* 1....... ........  row 0: tip */
	0xC000,  /* 11...... ........  row 1 */
	0xE000,  /* 111..... ........  row 2 */
	0xF000,  /* 1111.... ........  row 3 */
	0xF800,  /* 11111... ........  row 4 */
	0xFC00,  /* 111111.. ........  row 5 */
	0xFE00,  /* 1111111. ........  row 6 */
	0xFF00,  /* 11111111 ........  row 7 */
	0xFC00,  /* 111111.. ........  row 8 */
	0xE000,  /* 111..... ........  row 9 */
	0x8000,  /* 1....... ........  row 10 */
	0x0000,  /* ........ ........  row 11 */
	0x0000,  /* ........ ........  row 12 */
	0x0000,  /* ........ ........  row 13 */
	0x0000,  /* ........ ........  row 14 */
	0x0000   /* ........ ........  row 15 */
};

static int mouse_present_flag;
static int mouse_streaming_flag;
static unsigned int mouse_id;
static int mouse_cursor_flag;
static unsigned int mouse_pkt_size;

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

	/* Draw only the cursor fill bitmap (no outline). */
	for (r = 0; r < MOUSE_CUR_H; r++) {
		for (c = 0; c < MOUSE_CUR_W; c++) {
			if (mouse_bit_at((int)r, (int)c))
				vga_put_pixel_rgb(x0 + c, y0 + r, 255, 255, 255);
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

static int mouse_intellimouse_init(void)
{
	static const unsigned char rates[] = { 0xC8, 0x64, 0x50, 0x32 };
	unsigned int i;
	for (i = 0; i < sizeof(rates); i++) {
		if (mouse_send_cmd_ack(0xF3) != 0)
			return -1;
		if (mouse_send_cmd_ack(rates[i]) != 0)
			return -1;
	}
	return 0;
}

void mouse_init(void)
{
	unsigned char id = 0;

	mouse_present_flag = 0;
	mouse_streaming_flag = 0;
	mouse_id = 0;
	mouse_cursor_flag = 0;
	mouse_pkt_size = 3;
	pkt_idx = 0;

	enable_aux_port();

	if (mouse_send_cmd_ack(0xF5) != 0)
		return;

	mouse_intellimouse_init();

	mouse_cmd_write(0xF2);
	if (mouse_expect_ack() != 0)
		return;
	if (mouse_read_aux(&id, 500) != 0)
		return;

	mouse_present_flag = 1;
	mouse_id = id;
	mouse_pkt_size = (id == 0x03 || id == 0x04) ? 4u : 3u;
	mouse_cursor_setup();

	if (mouse_send_cmd_ack(0xF4) != 0)
		return;
	mouse_streaming_flag = 1;
}

void mouse_poll_update(void)
{
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
		if (pkt_idx < mouse_pkt_size)
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

unsigned int mouse_packet_size(void)
{
	return mouse_pkt_size;
}
