/* AHCI (SATA) driver: PCI discovery, port bring-up, IDENTIFY and DMA
 * read/write (READ/WRITE DMA EXT, LBA48), all polled (no IRQs).
 *
 * Target: the ION/AT3IONT-I MCP79 SATA controller in AHCI mode (the same
 * hardware family Linux drives with board_ahci_noncq), and QEMU's ich9-ahci
 * for testing. Only non-NCQ commands are used.
 *
 * All buffers (command list, FIS receive area, command table, DMA bounce
 * buffer) come from the page allocator -- the kernel lives under a tight
 * linker bound, so nothing large may go into BSS. */

#include "ahci.h"
#include "ata.h"
#include "mm.h"
#include "paging.h"
#include "pci.h"
#include "process.h"
#include "serial.h"
#include "timer.h"

#define AHCI_DEBUG 0

/* ---- generic host control registers (ABAR-relative) ---- */
#define H_CAP       0x00
#define H_GHC       0x04
#define H_PI        0x0C

#define GHC_AE      0x80000000u   /* AHCI enable */

/* ---- port registers (0x100 + 0x80 * port) ---- */
#define P_CLB       0x00
#define P_CLBU      0x04
#define P_FB        0x08
#define P_FBU       0x0C
#define P_IS        0x10
#define P_IE        0x14
#define P_CMD       0x18
#define P_TFD       0x20
#define P_SIG       0x24
#define P_SSTS      0x28
#define P_SCTL      0x2C
#define P_SERR      0x30
#define P_CI        0x38

#define CMD_ST      0x00000001u   /* start */
#define CMD_FRE     0x00000010u   /* FIS receive enable */
#define CMD_FR      0x00004000u   /* FIS receive running */
#define CMD_CR      0x00008000u   /* command list running */

#define TFD_ERR     0x01
#define TFD_BSY     0x80
#define TFD_DRQ     0x08
#define TFD_ERR_BITS (TFD_ERR | TFD_BSY | TFD_DRQ)

#define IS_TFES     0x40000000u   /* task file error status */

#define SIG_ATA     0x00000101u

/* Register Host-to-Device FIS. */
#define FIS_H2D     0x27
#define FIS_C       0x80

#define ATA_IDENTIFY        0xEC
#define ATA_READ_DMA_EXT    0x25
#define ATA_WRITE_DMA_EXT   0x35

#define AHCI_CHUNK  255           /* sectors per command (128 KB-ish) */
#define BOUNCE_PAGES 33           /* 255 * 512 = 130560 B < 33 * 4096 */

static struct {
	int inited;
	int ok;
	unsigned int abar;
	unsigned int nports;
	unsigned int pi;
	unsigned int cl;              /* command list (1 page) */
	unsigned int fis;             /* FIS receive area (1 page) */
	unsigned int ct;              /* command table (1 page) */
	unsigned int bounce;          /* data bounce buffer (BOUNCE_PAGES) */
} ahci;

static unsigned int rd(unsigned int off)
{
	return *(volatile unsigned int *)(ahci.abar + off);
}

static void wr(unsigned int off, unsigned int v)
{
	*(volatile unsigned int *)(ahci.abar + off) = v;
}

static unsigned int prd(unsigned int port, unsigned int off)
{
	return *(volatile unsigned int *)(ahci.abar + 0x100 + 0x80 * port + off);
}

static void pwr(unsigned int port, unsigned int off, unsigned int v)
{
	*(volatile unsigned int *)(ahci.abar + 0x100 + 0x80 * port + off) = v;
}

static void mem_copy(unsigned char *dst, const unsigned char *src, unsigned int n)
{
	while (n--)
		*dst++ = *src++;
}

static void delay_ms(unsigned int ms)
{
	unsigned int t0 = get_ticks_ms();
	while (get_ticks_ms() - t0 < ms)
		;
}

/* ---- PCI discovery ---- */

static int ahci_find(unsigned int *out_bus, unsigned int *out_dev, unsigned int *out_func)
{
	unsigned int bus, dev, func;

	for (bus = 0; bus < 256; bus++) {
		for (dev = 0; dev < 32; dev++) {
			unsigned int id = pci_cfg_read32(bus, dev, 0, 0x00);
			unsigned int htype, nfunc;

			if (id == 0xFFFFFFFFu || id == 0)
				continue;
			htype = (pci_cfg_read32(bus, dev, 0, 0x0C) >> 16) & 0xFFu;
			nfunc = (htype & 0x80u) ? 8u : 1u;
			for (func = 0; func < nfunc; func++) {
				unsigned int idf = pci_cfg_read32(bus, dev, func, 0x00);
				unsigned int crev, cls, sub, prog;

				if (idf == 0xFFFFFFFFu || idf == 0)
					continue;
				crev = pci_cfg_read32(bus, dev, func, 0x08);
				cls  = (crev >> 24) & 0xFFu;
				sub  = (crev >> 16) & 0xFFu;
				prog = (crev >> 8) & 0xFFu;
				if (cls != 0x01)
					continue;
				/* One line per storage function so a board whose
				 * SATA mode does not match can be diagnosed from
				 * the serial/netlog output (class CCDD, prog-if,
				 * vendor:device). */
				serial_puts(SERIAL_COM1, "ahci: storage fn ");
				serial_putdec(SERIAL_COM1, bus);
				serial_putc(SERIAL_COM1, ':');
				serial_putdec(SERIAL_COM1, dev);
				serial_putc(SERIAL_COM1, '.');
				serial_putdec(SERIAL_COM1, func);
				serial_puts(SERIAL_COM1, " id ");
				serial_puthex(SERIAL_COM1, idf);
				serial_puts(SERIAL_COM1, " class ");
				serial_puthex(SERIAL_COM1, (cls << 8) | sub);
				serial_puts(SERIAL_COM1, " prog ");
				serial_puthex(SERIAL_COM1, prog);
				serial_putc(SERIAL_COM1, '\n');
				if (sub == 0x06 && prog == 0x01) {
					*out_bus = bus;
					*out_dev = dev;
					*out_func = func;
					return 0;
				}
			}
		}
	}
	return -1;
}

/* ---- port bring-up ---- */

static void ahci_port_init(unsigned int port)
{
	unsigned int t0, c;

	/* Stop the port and its FIS receive engine. */
	c = prd(port, P_CMD);
	if (c & CMD_ST) {
		pwr(port, P_CMD, c & ~CMD_ST);
		t0 = get_ticks_ms();
		while ((prd(port, P_CMD) & CMD_CR) && get_ticks_ms() - t0 < 500)
			;
	}
	c = prd(port, P_CMD);
	if (c & CMD_FRE) {
		pwr(port, P_CMD, c & ~CMD_FRE);
		t0 = get_ticks_ms();
		while ((prd(port, P_CMD) & CMD_FR) && get_ticks_ms() - t0 < 500)
			;
	}

	pwr(port, P_CLB, ahci.cl);
	pwr(port, P_CLBU, 0);
	pwr(port, P_FB, ahci.fis);
	pwr(port, P_FBU, 0);

	pwr(port, P_SERR, 0xFFFFFFFFu);
	pwr(port, P_IS, 0xFFFFFFFFu);
	pwr(port, P_IE, 0);

	pwr(port, P_CMD, prd(port, P_CMD) | CMD_FRE);
	t0 = get_ticks_ms();
	while (!(prd(port, P_CMD) & CMD_FR) && get_ticks_ms() - t0 < 500)
		;

	/* Cold plug: reset the link if it is not established yet. */
	if ((prd(port, P_SSTS) & 0x0Fu) != 3) {
		pwr(port, P_SCTL, 1);   /* DET = 1: initiate COMRESET */
		t0 = get_ticks_ms();
		while ((prd(port, P_SSTS) & 0x0Fu) != 3 && get_ticks_ms() - t0 < 1000)
			;
	}

	pwr(port, P_CMD, prd(port, P_CMD) | CMD_ST);
	t0 = get_ticks_ms();
	while (!(prd(port, P_CMD) & CMD_CR) && get_ticks_ms() - t0 < 500)
		;
}

int ahci_init(void)
{
	unsigned int bus, dev, func, bar, cmd, cap, i;

	if (ahci.inited)
		return ahci.ok ? 0 : -1;
	ahci.inited = 1;

	if (ahci_find(&bus, &dev, &func) != 0) {
		serial_puts(SERIAL_COM1, "ahci: no matching controller (need 0106:01)\n");
		return -1;
	}

	bar = pci_cfg_read32(bus, dev, func, 0x24);
	if (bar & 1u) {
		serial_puts(SERIAL_COM1, "ahci: BAR5 is not MMIO\n");
		return -1;
	}
	bar &= 0xFFFFFFF0u;
	if (!bar || bar == 0xFFFFFFF0u) {
		serial_puts(SERIAL_COM1, "ahci: BAR5 is unassigned\n");
		return -1;
	}

	cmd = pci_cfg_read32(bus, dev, func, 0x04);
	pci_cfg_write32(bus, dev, func, 0x04, cmd | 0x0006u);  /* mem + bus master */

	process_map_mmio_all(bar, 0x2000);
	ahci.abar = bar;

	cap = rd(H_CAP);
	if (cap == 0 || cap == 0xFFFFFFFFu) {
		serial_puts(SERIAL_COM1, "ahci: no register response\n");
		return -1;
	}

	ahci.cl     = mm_alloc_page();
	ahci.fis    = mm_alloc_page();
	ahci.ct     = mm_alloc_page();
	ahci.bounce = mm_alloc_pages(BOUNCE_PAGES);
	if (!ahci.cl || !ahci.fis || !ahci.ct || !ahci.bounce) {
		serial_puts(SERIAL_COM1, "ahci: no memory for DMA buffers\n");
		return -1;
	}
	{
		unsigned int *p;
		unsigned int n;
		for (p = (unsigned int *)ahci.cl, n = 0; n < 0x400; n++) p[n] = 0;
		for (p = (unsigned int *)ahci.fis, n = 0; n < 0x400; n++) p[n] = 0;
		for (p = (unsigned int *)ahci.ct, n = 0; n < 0x400; n++) p[n] = 0;
		for (p = (unsigned int *)ahci.bounce, n = 0; n < BOUNCE_PAGES * 0x400; n++)
			p[n] = 0;
	}

	wr(H_GHC, rd(H_GHC) | GHC_AE);

	ahci.nports = (cap & 0x1Fu) + 1;
	if (ahci.nports > 32)
		ahci.nports = 32;
	ahci.pi = rd(H_PI);

	serial_puts(SERIAL_COM1, "ahci: ctrl ");
	serial_putdec(SERIAL_COM1, bus);
	serial_putc(SERIAL_COM1, ':');
	serial_putdec(SERIAL_COM1, dev);
	serial_putc(SERIAL_COM1, '.');
	serial_putdec(SERIAL_COM1, func);
	serial_puts(SERIAL_COM1, " bar=");
	serial_puthex(SERIAL_COM1, bar);
	serial_puts(SERIAL_COM1, " pi=");
	serial_puthex(SERIAL_COM1, ahci.pi);
	serial_putc(SERIAL_COM1, '\n');

	for (i = 0; i < ahci.nports; i++) {
		if (ahci.pi & (1u << i))
			ahci_port_init(i);
	}

	ahci.ok = 1;
	return 0;
}

int ahci_controller_present(void)
{
	ahci_init();
	return ahci.ok;
}

/* ---- command issue (slot 0, polled) ---- */

static int ahci_issue(unsigned int port, const unsigned char fis[20], int write,
		      unsigned int dma_addr, unsigned int bytes)
{
	unsigned int *hdr = (unsigned int *)ahci.cl;
	unsigned char *ct = (unsigned char *)ahci.ct;
	unsigned int t0, i, prdtl = bytes ? 1u : 0u;

	/* Wait for the port to be idle (previous command finished). */
	t0 = get_ticks_ms();
	while (prd(port, P_CI) & 1u) {
		if (get_ticks_ms() - t0 > 4000)
			return -1;
	}
	if (prd(port, P_TFD) & TFD_BSY)
		return -1;

	for (i = 0; i < 64; i++)
		ct[i] = 0;
	for (i = 0; i < 20; i++)
		ct[i] = fis[i];
	/* PRDT entry 0: data buffer + byte count (no data: PRDTL = 0). */
	if (prdtl) {
		((unsigned int *)(ct + 0x80))[0] = dma_addr;
		((unsigned int *)(ct + 0x80))[1] = 0;
		((unsigned int *)(ct + 0x80))[2] = 0;
		((unsigned int *)(ct + 0x80))[3] = ((bytes - 1) & 0x3FFFFFu) | 0x80000000u;
	}

	/* Command header: CFL=5 dwords, W (write), PRDTL. */
	hdr[0] = 5u | (write ? (1u << 6) : 0) | (prdtl << 16);
	hdr[1] = 0;
	hdr[2] = ahci.ct;
	hdr[3] = 0;

	pwr(port, P_IS, 0xFFFFFFFFu);
	pwr(port, P_CI, 1u);

	t0 = get_ticks_ms();
	while (prd(port, P_CI) & 1u) {
		if (get_ticks_ms() - t0 > 4000) {
#if AHCI_DEBUG
			serial_puts(SERIAL_COM1, "ahci: command timeout\n");
#endif
			return -1;
		}
	}
	if (prd(port, P_IS) & IS_TFES) {
#if AHCI_DEBUG
		serial_puts(SERIAL_COM1, "ahci: task file error, tfd=");
		serial_puthex(SERIAL_COM1, prd(port, P_TFD));
		serial_putc(SERIAL_COM1, '\n');
#endif
		return -1;
	}
	if (prd(port, P_TFD) & TFD_ERR_BITS)
		return -1;
	return 0;
}

static int ahci_identify_port(unsigned int port, unsigned int *sectors, char model[41])
{
	unsigned char fis[20];
	unsigned int attempt, i;
	int r = -1;

	for (i = 0; i < 20; i++)
		fis[i] = 0;
	fis[0] = FIS_H2D;
	fis[1] = FIS_C;
	fis[2] = ATA_IDENTIFY;
	fis[7] = 0xA0;                     /* device 0 */

	for (attempt = 0; attempt < 3; attempt++) {
		r = ahci_issue(port, fis, 0, ahci.bounce, 512);
		if (r == 0)
			break;
		delay_ms(20);
	}
	if (r != 0)
		return -1;

	return ata_identify_parse((const unsigned short *)ahci.bounce, sectors, model);
}

static int ahci_xfer(unsigned int port, unsigned int lba, void *buf,
		     unsigned int count, int write)
{
	unsigned char *p = buf;
	unsigned char *bounce = (unsigned char *)ahci.bounce;
	unsigned int done = 0;

	if (!ahci.ok || count == 0)
		return -1;

	while (done < count) {
		unsigned int chunk = count - done;
		unsigned int cur = lba + done;
		unsigned char fis[20];
		unsigned int i;

		if (chunk > AHCI_CHUNK)
			chunk = AHCI_CHUNK;

		for (i = 0; i < 20; i++)
			fis[i] = 0;
		fis[0] = FIS_H2D;
		fis[1] = FIS_C;
		fis[2] = write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT;
		fis[4] = (unsigned char)(cur & 0xFF);
		fis[5] = (unsigned char)((cur >> 8) & 0xFF);
		fis[6] = (unsigned char)((cur >> 16) & 0xFF);
		fis[7] = 0x40;                 /* LBA mode */
		fis[8] = (unsigned char)((cur >> 24) & 0xFF);
		fis[9] = 0;                    /* LBA4/LBA5: 32-bit LBA only */
		fis[10] = 0;
		fis[12] = (unsigned char)(chunk & 0xFF);
		fis[13] = (unsigned char)((chunk >> 8) & 0xFF);

		if (write)
			mem_copy(bounce, p + done * 512u, chunk * 512u);
		if (ahci_issue(port, fis, write, ahci.bounce, chunk * 512u) != 0) {
			serial_puts(SERIAL_COM1, "ahci: ");
			serial_puts(SERIAL_COM1, write ? "write" : "read");
			serial_puts(SERIAL_COM1, " failed port ");
			serial_putdec(SERIAL_COM1, port);
			serial_puts(SERIAL_COM1, " lba ");
			serial_putdec(SERIAL_COM1, cur);
			serial_puts(SERIAL_COM1, " count ");
			serial_putdec(SERIAL_COM1, chunk);
			serial_puts(SERIAL_COM1, " tfd ");
			serial_puthex(SERIAL_COM1, prd(port, P_TFD));
			serial_puts(SERIAL_COM1, " is ");
			serial_puthex(SERIAL_COM1, prd(port, P_IS));
			serial_putc(SERIAL_COM1, '\n');
			return -1;
		}
		if (!write)
			mem_copy(p + done * 512u, bounce, chunk * 512u);
		done += chunk;
	}
	return 0;
}

int ahci_read_sectors(unsigned int port, unsigned int lba, void *buf, unsigned int count)
{
	return ahci_xfer(port, lba, buf, count, 0);
}

int ahci_write_sectors(unsigned int port, unsigned int lba, const void *buf,
		       unsigned int count)
{
	return ahci_xfer(port, lba, (void *)buf, count, 1);
}

int ahci_flush(unsigned int port)
{
	unsigned char fis[20];
	unsigned int i;

	if (!ahci.ok || port >= ahci.nports)
		return -1;
	for (i = 0; i < 20; i++)
		fis[i] = 0;
	fis[0] = FIS_H2D;
	fis[1] = FIS_C;
	fis[2] = 0xEA;                 /* FLUSH CACHE EXT */
	return ahci_issue(port, fis, 0, 0, 0);
}

unsigned int ahci_scan(struct ahci_disk *out, unsigned int max)
{
	unsigned int port, n = 0;

	if (ahci_init() != 0)
		return 0;

	for (port = 0; port < ahci.nports && n < max; port++) {
		if (!(ahci.pi & (1u << port)))
			continue;
		if ((prd(port, P_SSTS) & 0x0Fu) != 3)
			continue;
		if (prd(port, P_SIG) != SIG_ATA)
			continue;              /* ATAPI / port multiplier / SEMB */
		if (ahci_identify_port(port, &out[n].sectors, out[n].model) != 0)
			continue;
		out[n].port = port;
		serial_puts(SERIAL_COM1, "ahci: port ");
		serial_putdec(SERIAL_COM1, port);
		serial_puts(SERIAL_COM1, " ");
		serial_puts(SERIAL_COM1, out[n].model);
		serial_puts(SERIAL_COM1, " ");
		serial_putdec(SERIAL_COM1, out[n].sectors);
		serial_puts(SERIAL_COM1, " sectors\n");
		n++;
	}
	return n;
}
