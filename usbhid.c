/* USB HID boot devices: mice + keyboards.
 *
 * The probe claims every HID interface it understands: boot mouse/keyboard
 * (class 3 / subclass 1 / protocol 2 or 1) or report-protocol HID
 * interfaces whose report descriptor says "mouse"/"pointer" or "keyboard"
 * (class 3, other subclasses — many modern and composite devices).  Each
 * claimed interface gets its own interrupt slot, so a composite device
 * (keyboard + mouse in one) and several devices can coexist.
 *
 * Mouse reports are handed back to mouse.c; keyboard reports are decoded
 * into PS/2 set-1 scancodes and injected into the regular input queue
 * (io_inject_scancode), so the terminal/WM see them exactly like a PS/2
 * keyboard.  Root ports are watched so devices can be plugged in later. */

#include "usbhid.h"
#include "io.h"
#include "usb.h"
#include "serial.h"
#include "timer.h"

/* Request types/requests used here. */
#define RT_HOST2DEV_STD 0x00u
#define RT_HOST2DEV_CLS 0x21u
#define RT_DEV2HOST_CLS_IF 0x81u   /* class request to an interface, IN */
#define REQ_GET_DESCRIPTOR 0x06u
#define REQ_SET_CONFIGURATION 0x09u
#define REQ_SET_PROTOCOL      0x0Bu

/* Hotplug watcher period (must match usb.c's internal throttle). */
#define WATCH_MS 250

/* Extended (0xE0-prefixed) PS/2 make codes in the mapping table. */
#define K_EXT 0xE000u

/* How much of a report descriptor to read for classification. */
#define REPORT_MAX 64

enum { KIND_FREE = 0, KIND_MOUSE, KIND_KBD };

struct hid_dev {
	unsigned int kind;        /* KIND_*, KIND_FREE = slot unused */
	int present;              /* endpoint armed */
	int claimed;              /* claimed by the scan in progress */
	unsigned int ctrl, addr, ep, mps, port;
	int low;
	unsigned int err;         /* consecutive transfer errors */
	/* keyboard report-diff state */
	unsigned char prev[6];
	unsigned char prev_mod;
};

static struct hid_dev devs[USB_HCD_INT_SLOTS];
static int hcd_ok;
static int need_rescan;
static unsigned int gen;
static unsigned int last_watch;

/* --- keyboard report decoding ------------------------------------------ */

/* HID keyboard usage -> PS/2 set-1 make code (0 = unmapped). */
static const unsigned short hid_to_sc[256] = {
	[0x04] = 0x1E, [0x05] = 0x30, [0x06] = 0x2E, [0x07] = 0x20,
	[0x08] = 0x12, [0x09] = 0x21, [0x0A] = 0x22, [0x0B] = 0x23,
	[0x0C] = 0x17, [0x0D] = 0x24, [0x0E] = 0x25, [0x0F] = 0x26,
	[0x10] = 0x32, [0x11] = 0x31, [0x12] = 0x18, [0x13] = 0x19,
	[0x14] = 0x10, [0x15] = 0x13, [0x16] = 0x1F, [0x17] = 0x14,
	[0x18] = 0x16, [0x19] = 0x2F, [0x1A] = 0x11, [0x1B] = 0x2D,
	[0x1C] = 0x15, [0x1D] = 0x2C,
	[0x1E] = 0x02, [0x1F] = 0x03, [0x20] = 0x04, [0x21] = 0x05,
	[0x22] = 0x06, [0x23] = 0x07, [0x24] = 0x08, [0x25] = 0x09,
	[0x26] = 0x0A, [0x27] = 0x0B,
	[0x28] = 0x1C, [0x29] = 0x01, [0x2A] = 0x0E, [0x2B] = 0x0F,
	[0x2C] = 0x39, [0x2D] = 0x0C, [0x2E] = 0x0D, [0x2F] = 0x1A,
	[0x30] = 0x1B, [0x31] = 0x2B, [0x33] = 0x27, [0x34] = 0x28,
	[0x35] = 0x29, [0x36] = 0x33, [0x37] = 0x34, [0x38] = 0x35,
	[0x39] = 0x3A,
	[0x3A] = 0x3B, [0x3B] = 0x3C, [0x3C] = 0x3D, [0x3D] = 0x3E,
	[0x3E] = 0x3F, [0x3F] = 0x40, [0x40] = 0x41, [0x41] = 0x42,
	[0x42] = 0x43, [0x43] = 0x44, [0x44] = 0x57, [0x45] = 0x58,
	[0x49] = K_EXT | 0x52, [0x4A] = K_EXT | 0x47, [0x4B] = K_EXT | 0x49,
	[0x4C] = K_EXT | 0x53, [0x4D] = K_EXT | 0x4F, [0x4E] = K_EXT | 0x51,
	[0x4F] = K_EXT | 0x4D, [0x50] = K_EXT | 0x4B, [0x51] = K_EXT | 0x50,
	[0x52] = K_EXT | 0x48,
};

/* Modifier bit -> PS/2 make code: L/R ctrl, shift, alt, GUI. */
static const unsigned short hid_mod_sc[8] = {
	0x1D, K_EXT | 0x1D,   /* L Ctrl, R Ctrl */
	0x2A, 0x36,           /* L Shift, R Shift */
	0x38, K_EXT | 0x38,   /* L Alt, R Alt */
	K_EXT | 0x5B, K_EXT | 0x5C,   /* L GUI, R GUI */
};
static const unsigned char hid_mod_bit[8] = {
	0x01, 0x10, 0x02, 0x20, 0x04, 0x40, 0x08, 0x80,
};

static void kbd_inject(unsigned int code, int down)
{
	if (!code)
		return;
	if (code & K_EXT) {
		io_inject_scancode(0xE0);
		io_inject_scancode((unsigned char)((code & 0xFFu) |
						   (down ? 0 : 0x80)));
	} else {
		io_inject_scancode((unsigned char)(code | (down ? 0 : 0x80)));
	}
}

/* Boot keyboard report: [modifiers][reserved][key0..key5].  Diff against
   the previous report to derive make/break scancodes. */
static void kbd_decode(struct hid_dev *d, const unsigned char *r)
{
	unsigned int i, j;

	for (i = 0; i < 8; i++) {
		int was = (d->prev_mod & hid_mod_bit[i]) != 0;
		int now = (r[0] & hid_mod_bit[i]) != 0;
		if (was != now)
			kbd_inject(hid_mod_sc[i], now);
	}
	d->prev_mod = r[0];

	/* Releases: in the previous report, gone from this one. */
	for (i = 0; i < 6; i++) {
		if (!d->prev[i])
			continue;
		for (j = 0; j < 6; j++) {
			if (r[2 + j] == d->prev[i])
				break;
		}
		if (j == 6)
			kbd_inject(hid_to_sc[d->prev[i]], 0);
	}
	/* Presses: new in this report. */
	for (i = 0; i < 6; i++) {
		if (!r[2 + i])
			continue;
		for (j = 0; j < 6; j++) {
			if (d->prev[j] == r[2 + i])
				break;
		}
		if (j == 6)
			kbd_inject(hid_to_sc[r[2 + i]], 1);
	}
	for (i = 0; i < 6; i++)
		d->prev[i] = r[2 + i];
}

/* --- report-descriptor classification ----------------------------------- */

/* Minimal HID item walk: find a Generic Desktop (usage page 1) mouse or
   keyboard collection.  Mouse detection accepts a Mouse/Pointer collection
   or a relative X/Y input item (so absolute tablets are not claimed). */
static unsigned int hid_classify(const struct usb_iface_info *in)
{
	static unsigned char rep[REPORT_MAX];
	int n, mouse = 0, kbd = 0;
	unsigned int i = 0, page = 0;
	int loc_mouse = 0, loc_kbd = 0, loc_xy = 0;

	n = usb_control_in(in->ctrl, in->addr, in->mps, in->low_speed,
			   RT_DEV2HOST_CLS_IF, REQ_GET_DESCRIPTOR,
			   (unsigned short)(0x22u << 8),
			   (unsigned short)in->iface_num, rep, sizeof(rep));
	if (n <= 0)
		return KIND_FREE;

	while (i < (unsigned int)n) {
		unsigned int item = rep[i++];
		unsigned int size = item & 3u;
		unsigned int type = (item >> 2) & 3u;
		unsigned int tag = (item >> 4) & 0xFu;
		unsigned int data = 0, k;

		if (size == 3u)
			size = 4u;
		for (k = 0; k < size; k++) {
			if (i + k < (unsigned int)n)
				data |= (unsigned int)rep[i + k] << (8u * k);
		}
		i += size;

		if (type == 1u && tag == 0u) {          /* Global Usage Page */
			page = data;
		} else if (type == 2u && tag == 0u && page == 0x01u) {
			if (data == 0x01u || data == 0x02u)     /* Pointer/Mouse */
				loc_mouse = 1;
			else if (data == 0x06u)                 /* Keyboard */
				loc_kbd = 1;
			else if (data == 0x30u || data == 0x31u) /* X/Y */
				loc_xy = 1;
		} else if (type == 0u) {                /* Main item */
			if (tag == 0xAu) {              /* Collection */
				if (loc_mouse)
					mouse = 1;
				if (loc_kbd)
					kbd = 1;
			} else if (tag == 0x8u && loc_xy &&
				   (data & 0x04u)) {    /* Input, relative */
				mouse = 1;
			}
			loc_mouse = loc_kbd = loc_xy = 0;
		}
	}

	if (mouse)
		return KIND_MOUSE;
	if (kbd)
		return KIND_KBD;
	return KIND_FREE;
}

/* --- manager ------------------------------------------------------------ */

static int hid_probe(const struct usb_iface_info *in)
{
	struct hid_dev *d = 0;
	unsigned int kind, i;

	if (in->cls != 0x03u || !in->ep_addr || !(in->ep_addr & 0x80u))
		return 0;

	if (in->subcls == 0x01u && in->proto == 0x02u)
		kind = KIND_MOUSE;
	else if (in->subcls == 0x01u && in->proto == 0x01u)
		kind = KIND_KBD;
	else
		kind = hid_classify(in);
	if (kind == KIND_FREE)
		return 0;

	for (i = 0; i < USB_HCD_INT_SLOTS; i++) {
		if (!devs[i].present && !devs[i].claimed) {
			d = &devs[i];
			break;
		}
	}
	if (!d)
		return 0;   /* every slot is busy */

	/* The endpoint is unusable until the device is configured. */
	usb_control_out(in->ctrl, in->addr, in->mps, in->low_speed,
			RT_HOST2DEV_STD, REQ_SET_CONFIGURATION,
			(unsigned short)(in->config_value & 0xFFu), 0);
	/* Boot protocol for boot-class devices; a stall is tolerated. */
	if (in->subcls == 0x01u)
		usb_control_out(in->ctrl, in->addr, in->mps, in->low_speed,
				RT_HOST2DEV_CLS, REQ_SET_PROTOCOL, 0,
				in->iface_num);

	d->kind = kind;
	d->ctrl = in->ctrl;
	d->addr = in->addr;
	d->ep = in->ep_addr & 0x0Fu;
	d->mps = in->ep_mps ? in->ep_mps : 8;
	d->port = in->port;
	d->low = in->low_speed;
	d->claimed = 1;
	return 1;
}

static void dev_attach(struct hid_dev *d, unsigned int slot)
{
	unsigned int i;

	if (usb_int_in_start(d->ctrl, slot, d->addr, d->ep, d->mps, d->low) != 0)
		return;
	d->err = 0;
	d->present = 1;
	d->prev_mod = 0;
	for (i = 0; i < 6; i++)
		d->prev[i] = 0;

	serial_puts(SERIAL_COM1, d->kind == KIND_MOUSE
				   ? "usbhid: mouse attached ("
				   : "usbhid: keyboard attached (");
	serial_puts(SERIAL_COM1, usb_hcd_name_ctrl(d->ctrl));
	serial_puts(SERIAL_COM1, ", addr ");
	serial_putdec(SERIAL_COM1, d->addr);
	serial_puts(SERIAL_COM1, " ep ");
	serial_putdec(SERIAL_COM1, 0x80u | d->ep);
	serial_puts(SERIAL_COM1, ")\n");
}

static void dev_detach(struct hid_dev *d, unsigned int slot)
{
	usb_int_in_stop(slot);
	d->present = 0;
	d->claimed = 0;
	last_watch = get_ticks_ms() - WATCH_MS;   /* watch again soon */
	serial_puts(SERIAL_COM1, d->kind == KIND_MOUSE
				   ? "usbhid: mouse detached (addr "
				   : "usbhid: keyboard detached (addr ");
	serial_putdec(SERIAL_COM1, d->addr);
	serial_puts(SERIAL_COM1, ")\n");
	d->kind = KIND_FREE;
}

static void arm_claimed(void)
{
	unsigned int i;

	for (i = 0; i < USB_HCD_INT_SLOTS; i++) {
		if (devs[i].claimed) {
			dev_attach(&devs[i], i);
			devs[i].claimed = 0;
		}
	}
}

static void hid_scan(void)
{
	unsigned int i;

	/* A full scan resets every root port: existing handles are stale. */
	for (i = 0; i < USB_HCD_INT_SLOTS; i++) {
		if (devs[i].kind != KIND_FREE) {
			usb_int_in_stop(i);
			devs[i].kind = KIND_FREE;
			devs[i].present = 0;
			devs[i].claimed = 0;
		}
	}

	if (usb_scan(hid_probe) != 0)
		return;   /* busy: retried from the watcher */
	need_rescan = 0;
	gen = usb_generation();
	arm_claimed();
}

void usbhid_init(void)
{
	if (usb_hcd_init() != 0) {
		serial_puts(SERIAL_COM1, "usbhid: no USB host controller\n");
		hcd_ok = 0;
		return;
	}
	hcd_ok = 1;

	if (usb_scan(hid_probe) != 0) {
		/* A scan is already running (unlikely at boot): finish from
		   the watcher. */
		need_rescan = 1;
		gen = usb_generation();
		return;
	}
	gen = usb_generation();
	arm_claimed();
	if (!usbhid_mouse_attached() && !usbhid_keyboard_attached())
		serial_puts(SERIAL_COM1,
			    "usbhid: no HID boot mouse/keyboard found\n");
}

/* Service one slot: detach on disconnect/errors, return report length. */
static int dev_poll(struct hid_dev *d, unsigned int slot, unsigned char *buf,
		    unsigned int len)
{
	int n;

	if (!d->present)
		return 0;
	if (d->port != 0xFFu && !usb_port_connected(d->ctrl, d->port)) {
		dev_detach(d, slot);
		return 0;
	}
	n = usb_int_in_poll(slot, buf, len);
	if (n < 0) {
		if (++d->err >= 5)
			dev_detach(d, slot);
		return 0;
	}
	d->err = 0;
	return n;
}

int usbhid_poll(unsigned int *buttons, int *dx, int *dy)
{
	unsigned char buf[8];
	unsigned int now, i, all_busy = 1;
	unsigned int b = 0;
	int n, x = 0, y = 0, got = 0;

	if (!hcd_ok)
		return 0;

	/* A full bus re-scan (`lsusb`) invalidates every handle. */
	if (usb_generation() != gen) {
		for (i = 0; i < USB_HCD_INT_SLOTS; i++) {
			if (devs[i].present)
				dev_detach(&devs[i], i);
		}
		need_rescan = 1;
	}

	/* Throttled: finish a pending rescan, or watch for hotplug. */
	for (i = 0; i < USB_HCD_INT_SLOTS; i++) {
		if (devs[i].kind == KIND_FREE)
			all_busy = 0;
	}
	now = get_ticks_ms();
	if (now - last_watch >= WATCH_MS) {
		last_watch = now;
		if (need_rescan) {
			hid_scan();
		} else if (!all_busy) {
			for (i = 0; i < USB_HCD_INT_SLOTS; i++)
				devs[i].claimed = 0;
			if (usb_hotplug_poll(hid_probe) > 0)
				arm_claimed();
		}
	}

	for (i = 0; i < USB_HCD_INT_SLOTS; i++) {
		struct hid_dev *d = &devs[i];

		if (d->kind == KIND_FREE)
			continue;
		n = dev_poll(d, i, buf, sizeof(buf));
		if (n >= 3 && d->kind == KIND_MOUSE) {
			b |= buf[0] & 0x07u;
			x += (int)(signed char)buf[1];
			/* HID: +y is down; mouse.c expects +y = up. */
			y -= (int)(signed char)buf[2];
			got = 1;
		} else if (n >= 8 && d->kind == KIND_KBD) {
			kbd_decode(d, buf);
		}
	}

	if (got) {
		*buttons = b;
		*dx = x;
		*dy = y;
	}
	return got;
}

int usbhid_mouse_attached(void)
{
	unsigned int i;

	for (i = 0; i < USB_HCD_INT_SLOTS; i++) {
		if (devs[i].kind == KIND_MOUSE && devs[i].present)
			return 1;
	}
	return 0;
}

int usbhid_keyboard_attached(void)
{
	unsigned int i;

	for (i = 0; i < USB_HCD_INT_SLOTS; i++) {
		if (devs[i].kind == KIND_KBD && devs[i].present)
			return 1;
	}
	return 0;
}

void usbhid_get_info(unsigned int *ctrl, unsigned int *addr,
		     unsigned int *ep, unsigned int *mps, int *low)
{
	unsigned int i;

	for (i = 0; i < USB_HCD_INT_SLOTS; i++) {
		struct hid_dev *d = &devs[i];

		if (d->kind == KIND_MOUSE && d->present) {
			*ctrl = d->ctrl;
			*addr = d->addr;
			*ep = d->ep;
			*mps = d->mps;
			*low = d->low;
			return;
		}
	}
	*ctrl = 0;
	*addr = 0;
	*ep = 0;
	*mps = 0;
	*low = 0;
}
