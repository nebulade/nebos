/*
 * NVIDIA nv50-family (ION/MCP79) display engine diagnostics.
 *
 * The hardware cursor is deliberately NOT driven on this family.  What was
 * learned while trying (ASUS AT3IONT-I, C79/ION, VBIOS-owned display):
 *
 *  - the VBIOS never hands the engine over: INTR_1 bit 8 stays set and
 *    0x6194e8 reads 0x803, so nothing has "stolen" the display;
 *  - EVO DMA channels accept DMA_CB and the USER_PUT doorbell, but never
 *    fetch (USER_GET stays) until the display is taken over the way nouveau
 *    does in nv50_disp_init -- which includes re-programming the whole mode;
 *  - the cursor control/offset registers (0x610a58/0x610a9c) are engine-only
 *    mirrors: plain writes do not stick, and the PIO method interface
 *    (0x610300/0x610304) does not execute them either;
 *  - once an EVO channel has run a batch it cannot be restarted (its busy
 *    bit stays set), so even a takeover yields a single attempt per boot.
 *
 * The software cursor has none of these problems.  This module only probes
 * the engine state at boot (serial / `make netmon`) so the situation stays
 * visible for future work.
 */

#include "nv50.h"
#include "nvidia.h"
#include "serial.h"

#define NV_PDISP_OBJECTS      0x610010
#define NV_PDISP_INTR_1       0x610024
#define NV_PDISP_PIO_CTRL     0x610300
#define NV_PDISP_CURSOR_CTRL2(i)  (0x610270 + (i) * 0x10)
#define NV_PDISP_EVO_CTRL(i)      (0x610200 + (i) * 0x10)
#define NV_PDISP_EVO_DMA_CB(i)    (0x610204 + (i) * 0x10)
#define NV_PDISP_VBIOS_HANDOFF    0x6194e8

static unsigned int nv_mmio;

static unsigned int mmio_rd(unsigned int ofs)
{
	return *(volatile unsigned int *)(nv_mmio + ofs);
}

static void log_hex(const char *s, unsigned int v)
{
	serial_puts(SERIAL_COM1, s);
	serial_puthex(SERIAL_COM1, v);
}

/* Count the capability shadow/active register differences (the 0x6101XX
 * "main control area that enables things" is all-zero while its 0x61XXXX
 * shadow reads 0xFFFFFFFF on this VBIOS). */
static unsigned int caps_diff(void)
{
	unsigned int i, d = 0;

	if (mmio_rd(0x614004) != mmio_rd(0x610184))
		d++;
	for (i = 0; i < 2; i++) {
		if (mmio_rd(0x616100 + i * 0x800) != mmio_rd(0x610190 + i * 0x10))
			d++;
		if (mmio_rd(0x616104 + i * 0x800) != mmio_rd(0x610194 + i * 0x10))
			d++;
		if (mmio_rd(0x616108 + i * 0x800) != mmio_rd(0x610198 + i * 0x10))
			d++;
		if (mmio_rd(0x61610c + i * 0x800) != mmio_rd(0x61019c + i * 0x10))
			d++;
	}
	for (i = 0; i < 3; i++)
		if (mmio_rd(0x61a000 + i * 0x800) != mmio_rd(0x6101d0 + i * 4))
			d++;
	for (i = 0; i < 2; i++)
		if (mmio_rd(0x61c000 + i * 0x800) != mmio_rd(0x6101e0 + i * 4))
			d++;
	for (i = 0; i < 3; i++)
		if (mmio_rd(0x61e000 + i * 0x800) != mmio_rd(0x6101f0 + i * 4))
			d++;
	return d;
}

void nv50_probe(void)
{
	unsigned int i;

	if (!nvidia_available())
		return;
	nv_mmio = nvidia_bar0();

	log_hex("nv50: pdobj ", mmio_rd(NV_PDISP_OBJECTS));
	log_hex(" cursor2 ", mmio_rd(NV_PDISP_CURSOR_CTRL2(0)));
	log_hex("/", mmio_rd(NV_PDISP_CURSOR_CTRL2(1)));
	log_hex(" pio ", mmio_rd(NV_PDISP_PIO_CTRL));
	log_hex(" intr1 ", mmio_rd(NV_PDISP_INTR_1));
	log_hex(" vbios ", mmio_rd(NV_PDISP_VBIOS_HANDOFF));
	serial_puts(SERIAL_COM1, " caps ");
	serial_putdec(SERIAL_COM1, caps_diff());
	serial_puts(SERIAL_COM1, "\n");

	for (i = 0; i < 8; i++) {
		unsigned int ctl = mmio_rd(NV_PDISP_EVO_CTRL(i));
		unsigned int cb = mmio_rd(NV_PDISP_EVO_DMA_CB(i));

		if (!ctl && !cb)
			continue;
		serial_puts(SERIAL_COM1, "nv50: evo");
		serial_putdec(SERIAL_COM1, i);
		log_hex(" ctrl ", ctl);
		log_hex(" cb ", cb);
		serial_puts(SERIAL_COM1, "\n");
	}
}
