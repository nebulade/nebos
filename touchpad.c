/* Elan HID-over-I2C touchpad on the Braswell LPSS I2C bus.
 *
 * The HP Chromebook 11 G5 EE (setzer) wires its touchpad to LPSS I2C6 at
 * 0xD1327000, slave address 0x15.  The LPSS I2C functions are hidden from
 * PCI in ACPI mode, so the driver scans the two firmware BAR windows for
 * DesignWare component types instead of enumerating PCI.
 *
 * The report line is a GPIO we have no driver for, so the input register is
 * polled.  A read with no pending report returns a zero length immediately.
 * Reading the register back-to-back stalls the device: 25 ms (40 Hz) is the
 * rate it keeps up with, and the pointer still moves smoothly.
 *
 * Init follows Linux's elan_i2c: reset, consume the 2-byte reset
 * acknowledgement, read the HID descriptor, select absolute mode (0x0300)
 * and wake the device (0x0800).  Resetting last leaves it silent until the
 * next power cycle.
 */

#include "touchpad.h"
#include "i2c.h"
#include "io.h"
#include "paging.h"
#include "timer.h"

#define TP_ADDR			0x15
#define TP_CTRL_MAX		16
#define TP_CTRL_STEP		0x1000u

#define TP_HID_DESC_REG		0x0001
#define TP_HID_DESC_LEN		30

#define TP_CMD_REG		0x0005
#define TP_CMD_RESET		0x0100
#define TP_CMD_WAKE		0x0800
#define TP_MODE_REG		0x0300
#define TP_MODE_ABS		0x0001

#define TP_DW_COMP_TYPE		0x44570140u
#define TP_REPORT_ID		0x5d

#define TP_MAX_INPUT		64
#define TP_POLL_MS		25

static unsigned int tp_base;
static unsigned int tp_addr = TP_ADDR;
static int tp_present;
static unsigned int tp_in_reg = 0x0003;
static unsigned int tp_max_in = 34;
static unsigned int tp_cmd_reg = TP_CMD_REG;
static unsigned short tp_vendor;
static unsigned short tp_product;
static const char *tp_err;
static unsigned int tp_err_val;

static int tp_last_x, tp_last_y, tp_acc_x, tp_acc_y;
static int tp_down, tp_last_buttons;
static unsigned int tp_next_poll;

static void put_hex16(unsigned int v)
{
	static const char hex[] = "0123456789ABCDEF";

	io_putchar(hex[(v >> 12) & 0xf]);
	io_putchar(hex[(v >> 8) & 0xf]);
	io_putchar(hex[(v >> 4) & 0xf]);
	io_putchar(hex[v & 0xf]);
}

static void print_summary(void)
{
	io_putstr("touchpad: elan ");
	put_hex16(tp_vendor);
	io_putchar(':');
	put_hex16(tp_product);
	io_putstr(" i2c 0x");
	io_puthex(tp_base);
	io_putstr(" addr 0x");
	io_puthex(tp_addr);
	io_putchar('\n');
}

static void msleep(unsigned int ms)
{
	unsigned int t0 = get_ticks_ms();

	while (get_ticks_ms() - t0 < ms)
		;
}

static unsigned int get16(const unsigned char *p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

static int hid_read_reg(unsigned int addr, unsigned int reg,
			unsigned char *buf, unsigned int len)
{
	unsigned char r[2];

	r[0] = (unsigned char)(reg & 0xff);
	r[1] = (unsigned char)(reg >> 8);
	if (i2c_dw_transfer(tp_base, addr, r, 2, buf, len) == 0)
		return 0;
	/* Some devices do not answer the combined write+read (repeated
	 * start).  Retry with a STOP between the register address and the
	 * data read. */
	if (i2c_dw_transfer(tp_base, addr, r, 2, 0, 0) != 0)
		return -1;
	return i2c_dw_transfer(tp_base, addr, 0, 0, buf, len);
}

static int hid_write_reg(unsigned int addr, unsigned int reg, unsigned int val)
{
	unsigned char b[4];

	b[0] = (unsigned char)(reg & 0xff);
	b[1] = (unsigned char)(reg >> 8);
	b[2] = (unsigned char)(val & 0xff);
	b[3] = (unsigned char)(val >> 8);
	return i2c_dw_transfer(tp_base, addr, b, 4, 0, 0);
}

static int probe_controller(unsigned int base)
{
	unsigned char desc[TP_HID_DESC_LEN + 2];
	unsigned char ack[2];
	unsigned int off, len, ver;

	if (!i2c_dw_present(base))
		return 0;
	if (i2c_dw_init(base) != 0) {
		tp_err = "i2c init failed";
		return 0;
	}
	tp_base = base;

	/* The Elan address is fixed; a plain read is a harmless ACK check. */
	if (i2c_dw_transfer(base, TP_ADDR, 0, 0, ack, 1) != 0) {
		tp_err = "no device at 0x15";
		return 0;
	}

	/* Reset first and consume the acknowledgement, then read the HID
	 * descriptor.  The descriptor read is prefixed with two status
	 * bytes and the 2-byte length, so accept either offset. */
	hid_write_reg(TP_ADDR, TP_CMD_REG, TP_CMD_RESET);
	msleep(150);
	(void)i2c_dw_transfer(base, TP_ADDR, 0, 0, ack, 2);
	if (hid_read_reg(TP_ADDR, TP_HID_DESC_REG, desc, sizeof(desc)) != 0) {
		tp_err = "descriptor read failed";
		tp_err_val = i2c_last_abort;
		return 0;
	}

	off = 0;
	if (get16(desc + 0) != TP_HID_DESC_LEN ||
	    get16(desc + 2) != 0x0100) {
		if (get16(desc + 2) == TP_HID_DESC_LEN &&
		    get16(desc + 4) == 0x0100)
			off = 2;
	}
	len = get16(desc + off);
	ver = get16(desc + off + 2);
	if (len != TP_HID_DESC_LEN || ver != 0x0100) {
		tp_err = "bad hid descriptor";
		return 0;
	}

	tp_in_reg = get16(desc + off + 8);
	tp_max_in = get16(desc + off + 10);
	tp_cmd_reg = get16(desc + off + 16);
	tp_vendor = (unsigned short)get16(desc + off + 20);
	tp_product = (unsigned short)get16(desc + off + 22);
	if (tp_max_in < 12 || tp_max_in > TP_MAX_INPUT)
		tp_max_in = 34;

	/* Absolute reporting, then wake: after the reset the Elan stays
	 * quiet until the host selects a mode. */
	hid_write_reg(TP_ADDR, TP_MODE_REG, TP_MODE_ABS);
	msleep(40);
	hid_write_reg(TP_ADDR, tp_cmd_reg, TP_CMD_WAKE);
	msleep(60);

	tp_present = 1;
	tp_err = 0;
	print_summary();
	return 1;
}

int touchpad_init(void)
{
	/* The ACPI _CRS address for the LPSS I2C blocks is 0x9132xxxx on
	 * coreboot, but this firmware maps them at 0xD132xxxx.  Both
	 * windows are scanned; page 7 (I2C6) is tried first. */
	static const unsigned int ranges[2] = { 0x91320000u, 0xd1320000u };
	unsigned int r, k;

	if (tp_present)
		return 1;

	for (r = 0; r < 2; r++) {
		for (k = 0; k < TP_CTRL_MAX; k++) {
			unsigned int i = (k + 7) % TP_CTRL_MAX;
			unsigned int base = ranges[r] + i * TP_CTRL_STEP;

			paging_map_mmio(base, 0x1000);
			/* A block left in functional reset keeps its
			 * register file but never clocks the I2C FSM. */
			i2c_dw_reset_release(base);
			if (probe_controller(base))
				return 1;
		}
	}
	return 0;
}

int touchpad_present(void)
{
	return tp_present;
}

unsigned int touchpad_addr(void)
{
	return tp_addr;
}

unsigned short touchpad_vendor(void)
{
	return tp_vendor;
}

unsigned short touchpad_product(void)
{
	return tp_product;
}

int touchpad_poll(int *buttons, int *dx, int *dy)
{
	unsigned char buf[TP_MAX_INPUT];
	unsigned int now, b;

	if (!tp_present)
		return 0;

	now = get_ticks_ms();
	if ((int)(now - tp_next_poll) < 0)
		return 0;
	tp_next_poll = now + TP_POLL_MS;

	if (hid_read_reg(tp_addr, tp_in_reg, buf, tp_max_in) != 0)
		return 0;

	/* The addressed read returns two status bytes, the 2-byte length
	 * and then the report, so locate the 0x5D report ID instead of
	 * trusting a fixed offset.  Layout after it (Linux hid-elan.c):
	 * touch info (f5..f1 at bits 7..3, clickpad at bit 0), then 5
	 * bytes per finger: x11:8/y11:9, x7:0, y7:0, size, pressure. */
	for (b = 0; b < 6 && b + 4 < tp_max_in; b++)
		if (buf[b] == TP_REPORT_ID)
			break;
	if (b >= 6 || b + 4 >= tp_max_in)
		return 0;

	{
		unsigned int ti = buf[b + 1];
		unsigned int xh = buf[b + 2];
		int x = ((int)(xh & 0xf0) << 4) | buf[b + 3];
		int y = ((int)(xh & 0x0f) << 8) | buf[b + 4];

		*buttons = ti & 0x01;
		if (!(ti & 0x08)) {		/* finger 1 lifted */
			tp_down = 0;
			tp_acc_x = 0;
			tp_acc_y = 0;
			if (tp_last_buttons != *buttons) {
				tp_last_buttons = *buttons;
				return 1;
			}
			return 0;
		}
		if (!tp_down) {
			tp_down = 1;
			tp_last_x = x;
			tp_last_y = y;
			*dx = 0;
			*dy = 0;
		} else {
			tp_acc_x += x - tp_last_x;
			/* The Elan's Y axis increases upwards, already the
			 * PS/2 convention: dragging up gives a positive dy. */
			tp_acc_y += y - tp_last_y;
			tp_last_x = x;
			tp_last_y = y;
			*dx = tp_acc_x / 2;
			tp_acc_x -= *dx * 2;
			*dy = tp_acc_y / 2;
			tp_acc_y -= *dy * 2;
		}
		if (*dx > 64)
			*dx = 64;
		if (*dx < -64)
			*dx = -64;
		if (*dy > 64)
			*dy = 64;
		if (*dy < -64)
			*dy = -64;
		if (tp_last_buttons != *buttons) {
			tp_last_buttons = *buttons;
			return 1;
		}
		return *dx || *dy;
	}
}

void program_touchpad_main(void)
{
	if (!tp_present) {
		io_putstr("touchpad: absent (");
		io_putstr(tp_err ? tp_err : "unknown");
		if (tp_err_val) {
			io_putstr(" ab=");
			io_puthex(tp_err_val);
		}
		io_putstr(")\n");
		return;
	}
	print_summary();
}
