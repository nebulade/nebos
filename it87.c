/* ITE IT87xx hardware monitor (read-only).
 *
 * The ASUS AT3IONT-I exposes CPU/MB temperatures and its three fan tach
 * inputs through an IT87xx Super I/O.  This gives an independent cross-check
 * for the CPU DTS (whose TjMax is not published for the Atom 330) and shows
 * whether the CPU fan is actually turning.
 *
 * Only the standard config-space probe and the environment controller's
 * read-only registers are touched; nothing is written to the fan control.
 */
#include "it87.h"
#include "pci.h"   /* port_inb/port_outb */

static unsigned short cfg_port;

static unsigned char sio_inb(unsigned char reg)
{
	port_outb(cfg_port, reg);
	return port_inb(cfg_port + 1);
}

static void sio_outb(unsigned char reg, unsigned char val)
{
	port_outb(cfg_port, reg);
	port_outb(cfg_port + 1, val);
}

static void sio_enter(unsigned short port)
{
	cfg_port = port;
	port_outb(cfg_port, 0x87);
	port_outb(cfg_port, 0x01);
	port_outb(cfg_port, 0x55);
	port_outb(cfg_port, port == 0x4E ? 0xAA : 0x55);
}

static void sio_exit(void)
{
	port_outb(cfg_port, 0x02);
	port_outb(cfg_port + 1, 0x02);
}

static int has_16bit_fans(unsigned int id)
{
	/* IT8705F/IT8712F use 8-bit tachometers; IT8716F and later have
	 * 16-bit counters (low byte at 0x0D..0x0F, high at 0x18..0x1A). */
	return id != 0x8705 && id != 0x8712;
}

void it87_read(struct it87_info *out)
{
	unsigned int i, base;
	unsigned char div;

	out->present = 0;
	out->chip_id = 0;
	out->base = 0;
	out->temp_enable = 0;
	out->temp_extra = 0;
	for (i = 0; i < IT87_TEMP_COUNT; i++)
		out->temp_c[i] = -128;
	for (i = 0; i < IT87_FAN_COUNT; i++)
		out->fan_rpm[i] = 0;

	for (i = 0; i < 2; i++) {
		unsigned short port = i ? 0x4E : 0x2E;
		unsigned int id;

		sio_enter(port);
		id = ((unsigned int)sio_inb(0x20) << 8) | sio_inb(0x21);
		if ((id & 0xFF00u) != 0x8700u) {
			sio_exit();
			continue;
		}
		/* Environment controller (logical device 4) base address. */
		sio_outb(0x07, 0x04);
		base = ((unsigned int)sio_inb(0x60) << 8) | sio_inb(0x61);
		base &= ~7u;
		if (sio_inb(0x30) != 0x01) {
			sio_exit();
			continue;
		}
		sio_exit();
		if (base < 0x100 || base > 0xFFF0)
			continue;
		out->present = 1;
		out->chip_id = id;
		out->base = base;
		break;
	}
	if (!out->present)
		return;

	for (i = 0; i < IT87_TEMP_COUNT; i++)
		out->temp_c[i] =
			(signed char)port_inb((unsigned short)(base + 0x29 + i));
	out->temp_enable = port_inb((unsigned short)(base + 0x51));
	out->temp_extra = port_inb((unsigned short)(base + 0x55));

	div = port_inb((unsigned short)(base + 0x0B));
	for (i = 0; i < IT87_FAN_COUNT; i++) {
		unsigned int count, d;

		count = port_inb((unsigned short)(base + 0x0D + i));
		if (has_16bit_fans(out->chip_id)) {
			count |= (unsigned int)port_inb((unsigned short)(base + 0x18 + i)) << 8;
			if (count == 0 || count == 0xFFFF)
				out->fan_rpm[i] = 0;
			else
				out->fan_rpm[i] = 1350000u / (count * 2);
			continue;
		}
		d = 1u << ((div >> (2 + i * 2)) & 3u);
		if (count == 0 || count >= 0xFF)
			out->fan_rpm[i] = 0;
		else
			out->fan_rpm[i] = 1350000u / (count * d);
	}
}
