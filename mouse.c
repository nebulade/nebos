#include "mouse.h"
#include "bootinfo.h"
#include "cirrus.h"
#include "cursor_shape.h"
#include "intel.h"
#include "radeon.h"
#include "timer.h"
#include "usbhid.h"
#include "vga.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

#define STATUS_OUT_FULL 0x01
#define STATUS_IN_FULL  0x02
#define STATUS_AUX_DATA 0x20

#define MOUSE_ACK     0xFA
#define MOUSE_RESEND  0xFE

#define MOUSE_CUR_W CURSOR_SHAPE_WIDTH
#define MOUSE_CUR_H CURSOR_SHAPE_HEIGHT

static int mouse_present_flag;
static int mouse_streaming_flag;
static int mouse_auto_draw = 1;   /* 0 while a WM frame owns the shadow */
static unsigned int mouse_id;
static int mouse_cursor_flag;
static unsigned int mouse_pkt_size;
static unsigned int mouse_buttons;  /* bit0=left bit1=right bit2=middle */
static int mouse_hw_cursor;         /* 1 = Cirrus, 2 = Radeon, 3 = Intel */
static int mouse_irq_mode;          /* PS/2 AUX IRQ12 owns packet input */
static int mouse_aux_irq_ok;        /* controller config accepted AUX IRQ */

static unsigned int fbw, fbh;
static unsigned int cur_x, cur_y;
static unsigned int saved_x, saved_y, saved_w, saved_h;
static unsigned char cur_saved[MOUSE_CUR_W * MOUSE_CUR_H * 4];
static unsigned char pkt[4];
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

static inline unsigned int mouse_irq_save(void)
{
	unsigned int flags;

	__asm__ volatile ("pushfl; popl %0; cli" : "=r"(flags));
	return flags;
}

static inline void mouse_irq_restore(unsigned int flags)
{
	__asm__ volatile ("pushl %0; popfl" :: "r"(flags));
}

/* Set the controller's AUX-interrupt bit.  The PIC line stays masked until
 * mouse_init() has a hardware cursor and streaming is enabled. */
static int ps2_aux_irq_config(void)
{
	unsigned int flags, spin = 100000;
	unsigned char cfg;

	flags = mouse_irq_save();
	ps2_wait_input_empty();
	outb(PS2_CMD, 0x20);
	while (spin-- && !(inb(PS2_STATUS) & STATUS_OUT_FULL))
		;
	if (!(inb(PS2_STATUS) & STATUS_OUT_FULL)) {
		mouse_irq_restore(flags);
		return -1;
	}
	cfg = inb(PS2_DATA);
	cfg |= 0x02;                    /* AUX (mouse) interrupt */
	ps2_wait_input_empty();
	outb(PS2_CMD, 0x60);
	ps2_wait_input_empty();
	outb(PS2_DATA, cfg);
	mouse_irq_restore(flags);
	return 0;
}

static void ps2_aux_irq_unmask(void)
{
	unsigned char mask;

	/* Master IRQ2 is the cascade, slave IRQ12 is the AUX line. */
	mask = inb(0x21);
	outb(0x21, (unsigned char)(mask & ~0x04));
	mask = inb(0xA1);
	outb(0xA1, (unsigned char)(mask & ~0x10));
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

enum {
	MOUSE_PIX_TRANSPARENT = 0,
	MOUSE_PIX_OUTLINE,
	MOUSE_PIX_FILL
};

static int mouse_pixel_at(int r, int c)
{
	if (r < 0 || c < 0 || r >= (int)MOUSE_CUR_H || c >= (int)MOUSE_CUR_W)
		return MOUSE_PIX_TRANSPARENT;
	if (cursor_shape_fill[(unsigned int)r] & (0x8000u >> c))
		return MOUSE_PIX_FILL;
	if (cursor_shape_outline[(unsigned int)r] & (0x8000u >> c))
		return MOUSE_PIX_OUTLINE;
	return MOUSE_PIX_TRANSPARENT;
}

static void cursor_save_under(void)
{
	if (!vga_shadow_is_active()) {
		/* Boot: the shadow buffer is not up yet, so there is no
		 * background to save (and the saved buffer is uninitialized). */
		saved_w = saved_h = 0;
		return;
	}
	saved_x = cur_x;
	saved_y = cur_y;
	saved_w = (cur_x + MOUSE_CUR_W <= fbw) ? MOUSE_CUR_W : fbw - cur_x;
	saved_h = (cur_y + MOUSE_CUR_H <= fbh) ? MOUSE_CUR_H : fbh - cur_y;
	if (saved_w && saved_h)
		vga_fb_read_rect(saved_x, saved_y, saved_w, saved_h, cur_saved);
}

static void cursor_restore_under(void)
{
	if (saved_w && saved_h)
		vga_fb_write_rect(saved_x, saved_y, saved_w, saved_h, cur_saved);
}

/* Push the rectangle the cursor last touched to VRAM right away.  The cursor
 * must not wait for the damage queue: over a large canvas (the browser) a
 * whole-window repaint can sit ahead of it for several flush iterations. */
static void cursor_present(void)
{
	if (saved_w && saved_h)
		vga_fb_present_rect(saved_x, saved_y, saved_w, saved_h);
}

static void mouse_draw_cursor(unsigned int x0, unsigned int y0)
{
	unsigned int r, c;

	/* White arrow body with a black outline around it. */
	for (r = 0; r < MOUSE_CUR_H; r++) {
		for (c = 0; c < MOUSE_CUR_W; c++) {
			int px = mouse_pixel_at((int)r, (int)c);
			if (px == MOUSE_PIX_FILL)
				vga_fb_put_pixel(x0 + c, y0 + r, 255, 255, 255);
			else if (px == MOUSE_PIX_OUTLINE)
				vga_fb_put_pixel(x0 + c, y0 + r, 0, 0, 0);
		}
	}
}

static void mouse_clamp_ptr(long *nx, long *ny)
{
	long maxx = (long)fbw - 1;
	long maxy = (long)fbh - 1;

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

	/* Cirrus hardware cursor: the card overlays it; no save/restore.
	 * UEFI GOP modes are not programmed through the legacy VGA sequencer,
	 * so use the software cursor there. */
	if (!bootinfo_efi() && cirrus_available()) {
		mouse_hw_cursor = 1;
		cur_x = 100;
		cur_y = 100;
		cirrus_cursor_setup();
		cirrus_cursor_move(cur_x, cur_y);
		mouse_cursor_flag = 1;
		return;
	}

	/* Radeon (R200/RV280) hardware cursor. */
	if (!bootinfo_efi() && radeon_available()) {
		mouse_hw_cursor = 2;
		cur_x = 100;
		cur_y = 100;
		radeon_cursor_setup();
		radeon_cursor_move(cur_x, cur_y);
		mouse_cursor_flag = 1;
		return;
	}

	/* Intel Ironlake (Arrandale/X201) cursor plane.  The cursor image is
	 * mapped through the GTT by intel_init(); only enable it once a mouse
	 * is actually present. */
	if (!bootinfo_efi() && intel_available()) {
		long nx = 100;
		long ny = 100;

		mouse_clamp_ptr(&nx, &ny);
		cur_x = (unsigned int)nx;
		cur_y = (unsigned int)ny;
		if (intel_cursor_setup(cur_x, cur_y) == 0) {
			mouse_hw_cursor = 3;
			mouse_cursor_flag = 1;
			return;
		}
		mouse_hw_cursor = 0;
	}

	/* NVIDIA nv50-family (ION/MCP79): no hardware cursor -- the VBIOS keeps
	   the display engine (see nv50.c); the software cursor is used. */

	mouse_hw_cursor = 0;
	cur_x = 100;
	cur_y = 100;
	{
		long nx = (long)cur_x;
		long ny = (long)cur_y;
		mouse_clamp_ptr(&nx, &ny);
		cur_x = (unsigned int)nx;
		cur_y = (unsigned int)ny;
	}
	cursor_save_under();
	mouse_draw_cursor(cur_x, cur_y);
	cursor_present();
	mouse_cursor_flag = 1;
}

static int mouse_intellimouse_init(void)
{
	/* First three rates are the Intellimouse enable magic (200/100/80);
	   the last is the final report rate.  0xC8 = 200 Hz for a smooth cursor. */
	static const unsigned char rates[] = { 0xC8, 0x64, 0x50, 0xC8 };
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
	mouse_irq_mode = 0;
	mouse_aux_irq_ok = 0;
	pkt_idx = 0;

	enable_aux_port();
	mouse_aux_irq_ok = (ps2_aux_irq_config() == 0);

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

	/* A hardware cursor makes the IRQ12 path safe: the handler only has
	 * to write the cursor registers, not touch the shadow buffer or the
	 * damage list.  Otherwise keep the kworker polling the controller. */
	if (mouse_hw_cursor && mouse_aux_irq_ok) {
		pkt_idx = 0;
		mouse_irq_mode = 1;
		ps2_aux_irq_unmask();
	}
}

/* Move the cursor by a relative delta (PS/2 sign convention: +dx right,
   +dy up), redrawing it with the current backend (hardware cursor or
   shadow-buffer save/restore). */
static void mouse_move(int dx, int dy)
{
	long nx, ny;

	if (!mouse_cursor_flag)
		return;

	nx = (long)cur_x + dx;
	ny = (long)cur_y - dy;
	mouse_clamp_ptr(&nx, &ny);
	if ((unsigned int)nx == cur_x && (unsigned int)ny == cur_y)
		return;

	if (mouse_hw_cursor) {
		/* The PS/2 AUX IRQ can preempt a USB-driven update in the
		 * kworker; keep the position read/modify/write atomic. */
		unsigned int flags = mouse_irq_save();

		cur_x = (unsigned int)nx;
		cur_y = (unsigned int)ny;
		if (mouse_hw_cursor == 3)
			intel_cursor_move(cur_x, cur_y);
		else if (mouse_hw_cursor == 2)
			radeon_cursor_move(cur_x, cur_y);
		else
			cirrus_cursor_move(cur_x, cur_y);
		mouse_irq_restore(flags);
	} else if (!mouse_auto_draw) {
		/* A WM frame is between BEGIN and PRESENT: it erased the
		 * software cursor and will redraw it at the new position. */
		cur_x = (unsigned int)nx;
		cur_y = (unsigned int)ny;
	} else {
		cursor_restore_under();
		cursor_present();          /* erase the old position in VRAM */
		cur_x = (unsigned int)nx;
		cur_y = (unsigned int)ny;
		cursor_save_under();
		mouse_draw_cursor(cur_x, cur_y);
		cursor_present();          /* draw the new position in VRAM */
	}
}

/* Decode one complete PS/2 packet from pkt[] (3 or 4 bytes). */
static void mouse_handle_packet(void)
{
	unsigned char b0 = pkt[0];
	int dx = (int)(signed char)pkt[1];
	int dy = (int)(signed char)pkt[2];

	mouse_buttons = b0 & 0x07;
	if (b0 & 0x10)
		dx |= ~0xFF;
	if (b0 & 0x20)
		dy |= ~0xFF;
	if ((b0 & 0xC0) != 0)
		return;
	mouse_move(dx, dy);
}

/* IRQ12: PS/2 AUX data.  With a hardware cursor the position update is just
 * a few MMIO writes, so it is safe and fast in interrupt context.  This path
 * is only enabled when mouse_irq_mode is set (hardware cursor present). */
void mouse_irq(void)
{
	unsigned int guard = 0;

	if (mouse_irq_mode) {
		for (;;) {
			unsigned char st;

			if (guard++ >= 16)
				break;
			st = inb(PS2_STATUS);
			if ((st & (STATUS_OUT_FULL | STATUS_AUX_DATA)) !=
			    (STATUS_OUT_FULL | STATUS_AUX_DATA))
				break;
			pkt[pkt_idx++] = inb(PS2_DATA);
			if (pkt_idx < mouse_pkt_size)
				continue;
			pkt_idx = 0;
			mouse_handle_packet();
		}
	}

	/* Slave first, then the master cascade. */
	outb(0xA0, 0x20);
	outb(0x20, 0x20);
}

void mouse_poll_update(void)
{
	unsigned int guard = 0;
	unsigned int usb_buttons = 0;
	int usb_dx = 0, usb_dy = 0;

	/* The cursor is set up for whichever backend shows up first (PS/2
	   is probed at boot, USB may attach later). */
	if ((mouse_present_flag || usbhid_mouse_attached()) && !mouse_cursor_flag)
		mouse_cursor_setup();

	/* USB HID mouse: reports arrive through the armed interrupt
	   endpoint; the backend also handles hotplug/detach. */
	if (usbhid_poll(&usb_buttons, &usb_dx, &usb_dy)) {
		mouse_buttons = usb_buttons;
		mouse_move(usb_dx, usb_dy);
	}

	if (mouse_irq_mode)
		return;                 /* IRQ12 owns the PS/2 packets */
	if (!mouse_streaming_flag)
		return;

	for (;;) {
		unsigned char st;

		/* Bound the drain: a stuck/emulated controller must not
		 * wedge the WM loop; leftovers are read next iteration. */
		if (guard++ >= 64)
			return;

		st = inb(PS2_STATUS);
		if ((st & (STATUS_OUT_FULL | STATUS_AUX_DATA)) !=
		    (STATUS_OUT_FULL | STATUS_AUX_DATA))
			return;

		pkt[pkt_idx++] = inb(PS2_DATA);
		if (pkt_idx < mouse_pkt_size)
			continue;
		pkt_idx = 0;
		mouse_handle_packet();
	}
}

int mouse_is_present(void)
{
	return mouse_present_flag || usbhid_mouse_attached();
}

int mouse_ps2_present(void)
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

int mouse_cursor_backend(void)
{
	return mouse_hw_cursor;
}

const char *mouse_cursor_backend_name(void)
{
	switch (mouse_hw_cursor) {
	case 1:  return "cirrus hw";
	case 2:  return "radeon hw";
	case 3:  return "intel hw";
	default: return "software";
	}
}

unsigned int mouse_packet_size(void)
{
	return mouse_pkt_size;
}

unsigned int mouse_get_x(void)
{
	return cur_x;
}

unsigned int mouse_get_y(void)
{
	return cur_y;
}

unsigned int mouse_buttons_state(void)
{
	return mouse_buttons;
}

void mouse_redraw_cursor(void)
{
	if (!mouse_cursor_flag || mouse_hw_cursor)
		return;
	/* If the pointer moved while a WM frame owned the shadow, the saved
	 * rectangle is still the old position and has been restored to the
	 * background there.  Erase it in VRAM before drawing the new one, so
	 * a moving cursor does not leave a trail behind a slow frame. */
	if ((saved_x != cur_x || saved_y != cur_y) && saved_w && saved_h)
		vga_fb_present_rect(saved_x, saved_y, saved_w, saved_h);
	cursor_save_under();
	mouse_draw_cursor(cur_x, cur_y);
	cursor_present();
}

void mouse_cursor_erase(void)
{
	if (!mouse_cursor_flag || mouse_hw_cursor)
		return;
	cursor_restore_under();
	/* No direct present here: the caller is about to redraw the scene.
	 * Keeping the old cursor on screen until that flush avoids a visible
	 * blink at the start of every window frame.  The restore already
	 * marked the rectangle dirty, so the flush erases it. */
}

void mouse_set_auto_draw(int on)
{
	mouse_auto_draw = on ? 1 : 0;
}
