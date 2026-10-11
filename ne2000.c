/* NE2000-compatible (Realtek RTL8029AS) Ethernet driver.
   PCI discovery, byte-mode remote DMA, on-card SRAM ring, polling (no IRQ).

   Register map (byte offsets from the I/O base; page selected by CR bits
   PS0/PS1). Page 0: 0x00 CR, 0x01 PSTART, 0x02 PSTOP, 0x03 BNRY, 0x04
   TPSR(TSR), 0x05/0x06 TBCR, 0x07 ISR, 0x08/0x09 RSAR, 0x0A/0x0B RBCR,
   0x0C RCR, 0x0D TCR, 0x0E DCR, 0x0F IMR. Page 1: 0x01-0x06 PAR0-5,
   0x07 CURR. Data port 0x10, reset port 0x1F. */

#include "io.h"
#include "pci.h"
#include "timer.h"
#include "ne2000.h"

/* Register offsets. */
#define NE_CR      0x00
#define NE_PSTART  0x01
#define NE_PSTOP   0x02
#define NE_BNRY    0x03
#define NE_TSR     0x04   /* read: transmit status */
#define NE_TPSR    0x04   /* write: transmit page start */
#define NE_TBCR0   0x05
#define NE_TBCR1   0x06
#define NE_ISR     0x07
#define NE_RSAR0   0x08
#define NE_RSAR1   0x09
#define NE_RBCR0   0x0A
#define NE_RBCR1   0x0B
#define NE_RCR     0x0C
#define NE_TCR     0x0D
#define NE_DCR     0x0E
#define NE_IMR     0x0F
#define NE_DATA    0x10
#define NE_RESET   0x1F

/* CR bits. */
#define CR_STP     0x01
#define CR_START   0x02
#define CR_TXP     0x04
#define CR_RREAD   0x08
#define CR_RWRITE  0x10
#define CR_NODMA   0x20
#define CR_PAGE1   0x40

/* ISR bits. */
#define ISR_PTX    0x02
#define ISR_TXE    0x08
#define ISR_OVW    0x10
#define ISR_RST    0x80

/* RCR bits. */
#define RCR_AB     0x04
#define RCR_PRO    0x10

/* TSR bits. */
#define TSR_PTX    0x01

/* On-card SRAM layout (16 KB at 0x4000): TX buffer 0x4000-0x45FF,
   RX ring 0x4600-0x7FFF. Pages are 256 bytes. */
#define TX_PAGE    0x40
#define RX_START   0x46
#define RX_STOP    0x80

/* PCI IDs. */
#define RTL8029_VENDOR 0x10ECu
#define RTL8029_DEVICE 0x8029u

static unsigned short iobase;
static unsigned char mac[6];
static int present;
static int inited;

static unsigned char nread(unsigned char reg)
{
	return port_inb((unsigned short)(iobase + reg));
}

static void nwrite(unsigned char reg, unsigned char val)
{
	port_outb((unsigned short)(iobase + reg), val);
}

static void delay_ms(unsigned int ms)
{
	unsigned int t0 = get_ticks_ms();
	while (get_ticks_ms() - t0 < ms)
		;
}

static void remote_read(unsigned short addr, unsigned char *dst, unsigned short len)
{
	unsigned short i;

	nwrite(NE_CR, CR_NODMA | CR_START);      /* page 0, keep NIC running */
	nwrite(NE_RBCR0, 0);
	nwrite(NE_RBCR1, 0);
	nwrite(NE_RSAR0, addr & 0xFF);
	nwrite(NE_RSAR1, addr >> 8);
	nwrite(NE_RBCR0, len & 0xFF);
	nwrite(NE_RBCR1, len >> 8);
	nwrite(NE_CR, CR_RREAD | CR_START);      /* remote read, page 0 */
	for (i = 0; i + 1 < len; i += 2) {
		unsigned short w = port_inw((unsigned short)(iobase + NE_DATA));
		dst[i] = (unsigned char)(w & 0xFF);
		dst[i + 1] = (unsigned char)(w >> 8);
	}
	if (len & 1)
		dst[len - 1] = port_inb((unsigned short)(iobase + NE_DATA));
}

static void remote_write(unsigned short addr, const unsigned char *src, unsigned short len)
{
	unsigned short i;

	nwrite(NE_CR, CR_NODMA | CR_START);
	nwrite(NE_RBCR0, 0);
	nwrite(NE_RBCR1, 0);
	nwrite(NE_RSAR0, addr & 0xFF);
	nwrite(NE_RSAR1, addr >> 8);
	nwrite(NE_RBCR0, len & 0xFF);
	nwrite(NE_RBCR1, len >> 8);
	nwrite(NE_CR, CR_RWRITE | CR_START);     /* remote write, page 0 */
	for (i = 0; i + 1 < len; i += 2)
		port_outw((unsigned short)(iobase + NE_DATA),
			  (unsigned short)(src[i] | (src[i + 1] << 8)));
	if (len & 1)
		port_outb((unsigned short)(iobase + NE_DATA), src[len - 1]);
}

int ne2000_init(void)
{
	unsigned int bus, dev;

	if (inited)
		return present ? 0 : -1;
	inited = 1;

	present = 0;
	iobase = 0;

	/* PCI scan for Realtek RTL8029. */
	for (bus = 0; bus < 256 && !present; bus++) {
		for (dev = 0; dev < 32; dev++) {
			unsigned int id = pci_cfg_read32(bus, dev, 0, 0x00);
			unsigned int cmd, bar;

			if (id == 0xFFFFFFFFu || id == 0)
				continue;
			if ((id & 0xFFFFu) != RTL8029_VENDOR)
				continue;
			if (((id >> 16) & 0xFFFFu) != RTL8029_DEVICE)
				continue;

			/* Enable I/O space + bus master. */
			cmd = pci_cfg_read32(bus, dev, 0, 0x04);
			pci_cfg_write32(bus, dev, 0, 0x04, cmd | 0x0005u);

			bar = pci_cfg_read32(bus, dev, 0, 0x10);
			if (!(bar & 0x1u))
				continue;   /* not an I/O-space BAR */

			iobase = (unsigned short)(bar & 0xFFFCu);
			present = 1;
		}
	}

	if (!present)
		return -1;

	/* Reset (read then write the reset port). */
	nwrite(NE_RESET, nread(NE_RESET));
	delay_ms(20);

	/* Wait for reset completion (ISR RST bit), then clear ISR. */
	{
		unsigned int t0 = get_ticks_ms();
		while (!(nread(NE_ISR) & ISR_RST)) {
			if (get_ticks_ms() - t0 > 500)
				break;
		}
		nwrite(NE_ISR, 0xFF);
	}

	/* Stop, page 0. */
	nwrite(NE_CR, CR_NODMA | CR_STP);

	/* DCR: word-wide remote DMA (16-bit), normal (non-loopback), 8-byte FIFO. */
	nwrite(NE_DCR, 0x49);

	nwrite(NE_RBCR0, 0);
	nwrite(NE_RBCR1, 0);

	/* Read the station address PROM. The NE2000/RTL8029 maps the MAC into
	   the first 32 bytes of remote-DMA-addressable memory, at even offsets. */
	{
		unsigned char prom[32];
		unsigned int i;
		remote_read(0x0000, prom, 32);
		for (i = 0; i < 6; i++)
			mac[i] = prom[2 * i];
	}

	/* RX: promiscuous + broadcast (easiest to observe traffic). */
	nwrite(NE_RCR, RCR_PRO | RCR_AB);

	/* TX: normal, append CRC. */
	nwrite(NE_TCR, 0x00);

	/* Ring buffer. */
	nwrite(NE_PSTART, RX_START);
	nwrite(NE_PSTOP, RX_STOP);
	nwrite(NE_BNRY, RX_START);

	/* Clear ISR; mask all interrupts (polling). */
	nwrite(NE_ISR, 0xFF);
	nwrite(NE_IMR, 0x00);

	/* Page 1: write the MAC to PAR0-5 (so unicast frames to us are
	   accepted even if promiscuous mode is unreliable), then set CURR. */
	nwrite(NE_CR, CR_NODMA | CR_START | CR_PAGE1);
	{
		unsigned int i;
		for (i = 0; i < 6; i++)
			nwrite((unsigned char)(NE_PSTART + i), mac[i]);
	}
	nwrite(0x07, RX_START + 1);   /* CURR = first free page */

	/* Page 0, start the NIC. */
	nwrite(NE_CR, CR_NODMA | CR_START);

	return 0;
}

int ne2000_present(void)
{
	return present;
}

void ne2000_get_mac(unsigned char out[6])
{
	unsigned int i;
	for (i = 0; i < 6; i++)
		out[i] = mac[i];
}

int ne2000_rx(unsigned char *buf, unsigned int maxlen)
{
	unsigned char bnry, cur, pkt, next, status;
	unsigned short addr, len;
	unsigned int guard = 0;

	if (!present)
		return -1;

	for (;;) {
		/* Safety net: a full ring is only ~230 frames, so bail out
		   rather than spin forever if the card misbehaves. */
		if (++guard > 1024)
			return -1;

		/* BNRY (page 0) and CURR (page 1). */
		nwrite(NE_CR, CR_NODMA | CR_START);

		/* Ring overflow: the card wrapped and clobbered unread frames,
		   so the "next" pointers are unreliable. Discard the whole ring
		   and start fresh, or we'd re-read stale frames (duplicates). */
		if (nread(NE_ISR) & ISR_OVW) {
			nwrite(NE_CR, CR_NODMA | CR_START | CR_PAGE1);
			cur = nread(0x07);
			nwrite(NE_CR, CR_NODMA | CR_START);
			if (cur == RX_START)
				nwrite(NE_BNRY, RX_STOP - 1);
			else
				nwrite(NE_BNRY, cur - 1);
			nwrite(NE_ISR, ISR_OVW);
			return -1;
		}

		bnry = nread(NE_BNRY);
		nwrite(NE_CR, CR_NODMA | CR_START | CR_PAGE1);
		cur = nread(0x07);
		nwrite(NE_CR, CR_NODMA | CR_START);

		/* Next unread frame starts at page bnry+1 (wrapping). */
		pkt = bnry + 1;
		if (pkt >= RX_STOP)
			pkt = RX_START;

		/* No new frames once the next-unread page catches up to CURR. */
		if (pkt == cur)
			return -1;

		addr = (unsigned short)pkt << 8;

		/* 4-byte header: status, next page, length lo, length hi. */
		{
			unsigned char hdr[4];
			remote_read(addr, hdr, 4);
			status = hdr[0];
			next = hdr[1];
			len = (unsigned short)hdr[2] | ((unsigned short)hdr[3] << 8);
		}

		/* Advance boundary past this frame. */
		if (next == RX_START)
			nwrite(NE_BNRY, RX_STOP - 1);
		else
			nwrite(NE_BNRY, next - 1);

		/* Skip errored or oversized frames. */
		if (!(status & 0x01))
			continue;
		if (len < 4 || (unsigned int)(len - 4) > maxlen)
			continue;

		remote_read(addr + 4, buf, (unsigned short)(len - 4));
		return (int)(len - 4);
	}
}

int ne2000_tx(const unsigned char *buf, unsigned int len)
{
	static unsigned char pad[60];
	const unsigned char *p = buf;
	unsigned int txlen = len;
	unsigned int t0;
	unsigned char isr, tsr;

	if (!present)
		return -1;
	if (len < 14 || len > 1514)
		return -1;

	/* Pad to the 60-byte Ethernet minimum (the NIC does not auto-pad;
	   a runt frame would be dropped by the switch/peer). */
	if (len < 60) {
		unsigned int i;
		for (i = 0; i < len; i++)
			pad[i] = buf[i];
		for (i = len; i < 60; i++)
			pad[i] = 0;
		p = pad;
		txlen = 60;
	}

	/* Copy the frame into on-card SRAM. */
	remote_write((unsigned short)(TX_PAGE << 8), p, (unsigned short)txlen);

	/* Program transmit page + byte count. */
	nwrite(NE_CR, CR_NODMA | CR_START);
	nwrite(NE_TPSR, TX_PAGE);
	nwrite(NE_TBCR0, txlen & 0xFF);
	nwrite(NE_TBCR1, (txlen >> 8) & 0xFF);

	/* Clear TX status bits, then fire. */
	nwrite(NE_ISR, ISR_PTX | ISR_TXE);
	nwrite(NE_CR, CR_NODMA | CR_START | CR_TXP);

	/* Poll for completion. */
	t0 = get_ticks_ms();
	for (;;) {
		isr = nread(NE_ISR);
		if (isr & (ISR_PTX | ISR_TXE))
			break;
		if (get_ticks_ms() - t0 > 100)
			return -1;
	}

	tsr = nread(NE_TSR);
	if (!(tsr & TSR_PTX))
		return -1;   /* collision/abort/underrun */
	return 0;
}

static void put_hex2(unsigned char v)
{
	static const char hex[] = "0123456789ABCDEF";
	io_putchar(hex[(v >> 4) & 0xF]);
	io_putchar(hex[v & 0xF]);
}

void ne2000_print_info(void)
{
	unsigned int i;

	if (!present) {
		io_putstr("net: no RTL8029AS found\n");
		return;
	}

	io_putstr("RTL8029AS iobase=0x");
	io_puthex(iobase);
	io_putstr(" mac=");
	for (i = 0; i < 6; i++) {
		if (i)
			io_putchar(':');
		put_hex2(mac[i]);
	}
	io_putchar('\n');
}

const struct netdev ne2000_netdev = {
	"ne2000", "RTL8029AS",
	ne2000_init, ne2000_get_mac, ne2000_rx, ne2000_tx, ne2000_print_info, 0, 0, 0,
};
