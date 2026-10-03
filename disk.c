/* Disk layer: read/write sectors via direct FDC + ISA DMA in protected mode. */

#include "disk.h"
#include "io.h"
#include "timer.h"

#define SECTOR_SIZE       512
#define SECTORS_PER_TRACK 18
#define HEADS             2
#define DMA_BUF_ADDR      0x30000

/* FDC I/O ports */
#define FDC_DOR   0x3F2
#define FDC_MSR   0x3F4
#define FDC_FIFO  0x3F5
#define FDC_CCR   0x3F7

/* ISA DMA channel 2 ports */
#define DMA_CH2_ADDR      0x04
#define DMA_CH2_COUNT     0x05
#define DMA_CH2_PAGE      0x81
#define DMA_SINGLEMASK    0x0A
#define DMA_MODE          0x0B
#define DMA_FLIPFLOP      0x0C

/* MSR bits */
#define MSR_RQM  0x80
#define MSR_DIO  0x40
#define MSR_CB   0x10

/* FDC commands */
#define CMD_SPECIFY      0x03
#define CMD_RECALIBRATE  0x07
#define CMD_SENSE_INT    0x08
#define CMD_READ_DATA    0x66   /* READ | MFM | SK  (no Multi-Track) */
#define CMD_WRITE_DATA   0xC5   /* WRITE | MFM */
#define CMD_SEEK         0x0F

#define MOTOR_DELAY_MS   300
#define IRQ_TIMEOUT_MS   2000
#define MAX_RETRIES      10
#define SEEK_SETTLE_MS   25   /* head settle after seek */

/* PIC master mask port */
#define PIC1_DATA  0x21

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

volatile int fdc_irq_fired;

void fdc_irq(void)
{
	fdc_irq_fired = 1;
	outb(0x20, 0x20);
}

static int fdc_initialized;
static int motor_on;
static unsigned int cur_cylinder;
static unsigned int current_drive;

/* ---- low-level FDC helpers ---- */

static int fdc_wait_rqm(void)
{
	unsigned int t0 = get_ticks_ms();
	while ((get_ticks_ms() - t0) < 500) {
		if (inb(FDC_MSR) & MSR_RQM)
			return 0;
	}
	return -1;
}

static int fdc_send(unsigned char b)
{
	if (fdc_wait_rqm() != 0)
		return -1;
	if (inb(FDC_MSR) & MSR_DIO)
		return -1;
	outb(FDC_FIFO, b);
	return 0;
}

static int fdc_recv(unsigned char *b)
{
	if (fdc_wait_rqm() != 0)
		return -1;
	if (!(inb(FDC_MSR) & MSR_DIO))
		return -1;
	*b = inb(FDC_FIFO);
	return 0;
}

static int fdc_wait_irq(void)
{
	unsigned int t0 = get_ticks_ms();
	while (!fdc_irq_fired) {
		if ((get_ticks_ms() - t0) >= IRQ_TIMEOUT_MS)
			return -1;
		__asm__ volatile ("hlt");
	}
	return 0;
}

static int fdc_sense_interrupt(unsigned char *st0, unsigned char *cyl)
{
	if (fdc_send(CMD_SENSE_INT) != 0)
		return -1;
	if (fdc_recv(st0) != 0)
		return -1;
	if (fdc_recv(cyl) != 0)
		return -1;
	return 0;
}

/* ---- motor control ---- */

static void fdc_motor_on(void)
{
	if (motor_on)
		return;
	outb(FDC_DOR, (unsigned char)((1 << (4 + current_drive)) | 0x0C | current_drive));
	{
		unsigned int t0 = get_ticks_ms();
		while ((get_ticks_ms() - t0) < MOTOR_DELAY_MS)
			__asm__ volatile ("hlt");
	}
	motor_on = 1;
}

/* ---- reset ---- */

static int fdc_reset(void)
{
	unsigned char st0, cyl;
	int i;

	/* Pull reset line: DOR = 0 turns off controller + motor */
	outb(FDC_DOR, 0x00);
	motor_on = 0;
	{
		unsigned int t0 = get_ticks_ms();
		while ((get_ticks_ms() - t0) < 10)
			;
	}

	/* Release reset: controller on, DMA/IRQ enabled, no motor.
	 * The FDC fires IRQ 6 once the reset completes. */
	fdc_irq_fired = 0;
	outb(FDC_DOR, (unsigned char)(0x0C | current_drive));

	if (fdc_wait_irq() != 0)
		return -1;

	for (i = 0; i < 4; i++)
		fdc_sense_interrupt(&st0, &cyl);

	outb(FDC_CCR, 0x00); /* 500 kbps for 1.44 MB */

	/* SPECIFY: SRT=12ms, HUT=240ms, HLT=30ms, DMA mode
	 * Conservative timings for real hardware compatibility.
	 * Byte 1: SRT_nibble=4 (16-4=12ms), HUT_nibble=F (15*16=240ms) → 0x4F
	 * Byte 2: HLT=15 (15*2=30ms)<<1 | NDMA=0 → 0x1E */
	if (fdc_send(CMD_SPECIFY) != 0)
		return -1;
	if (fdc_send(0x4F) != 0)
		return -1;
	if (fdc_send(0x1E) != 0)
		return -1;

	return 0;
}

static int fdc_recalibrate(void)
{
	unsigned char st0, cyl;

	fdc_irq_fired = 0;
	if (fdc_send(CMD_RECALIBRATE) != 0)
		return -1;
	if (fdc_send((unsigned char)current_drive) != 0)
		return -1;

	if (fdc_wait_irq() != 0)
		return -1;
	if (fdc_sense_interrupt(&st0, &cyl) != 0)
		return -1;
	if (st0 & 0xC0)
		return -1;

	cur_cylinder = 0;
	return 0;
}

static int fdc_seek(unsigned int cylinder, unsigned int head)
{
	unsigned char st0, cyl;

	fdc_irq_fired = 0;
	if (fdc_send(CMD_SEEK) != 0)
		return -1;
	if (fdc_send((unsigned char)((head << 2) | current_drive)) != 0)
		return -1;
	if (fdc_send((unsigned char)cylinder) != 0)
		return -1;

	if (fdc_wait_irq() != 0)
		return -1;
	if (fdc_sense_interrupt(&st0, &cyl) != 0)
		return -1;
	if ((st0 & 0xC0) || cyl != (unsigned char)cylinder)
		return -1;

	cur_cylinder = cylinder;
	/* Let head settle on real hardware before issuing read (avoids LBA 33+ read failures). */
	{
		unsigned int t0 = get_ticks_ms();
		while ((get_ticks_ms() - t0) < SEEK_SETTLE_MS)
			;
	}
	return 0;
}

/* ---- DMA channel 2 setup ---- */

static void dma_setup_read(unsigned int phys, unsigned int len)
{
	unsigned int count = len - 1;

	outb(DMA_SINGLEMASK, 0x06);        /* mask channel 2 */
	outb(DMA_FLIPFLOP, 0xFF);         /* reset flip-flop */
	outb(DMA_CH2_ADDR, (unsigned char)(phys & 0xFF));
	outb(DMA_CH2_ADDR, (unsigned char)((phys >> 8) & 0xFF));
	outb(DMA_CH2_PAGE, (unsigned char)((phys >> 16) & 0xFF));
	outb(DMA_FLIPFLOP, 0xFF);         /* reset flip-flop */
	outb(DMA_CH2_COUNT, (unsigned char)(count & 0xFF));
	outb(DMA_CH2_COUNT, (unsigned char)((count >> 8) & 0xFF));
	outb(DMA_MODE, 0x46);             /* single, increment, write, ch2 */
	outb(DMA_SINGLEMASK, 0x02);        /* unmask channel 2 */
}

static void dma_setup_write(unsigned int phys, unsigned int len)
{
	unsigned int count = len - 1;

	outb(DMA_SINGLEMASK, 0x06);        /* mask channel 2 */
	outb(DMA_FLIPFLOP, 0xFF);         /* reset flip-flop */
	outb(DMA_CH2_ADDR, (unsigned char)(phys & 0xFF));
	outb(DMA_CH2_ADDR, (unsigned char)((phys >> 8) & 0xFF));
	outb(DMA_CH2_PAGE, (unsigned char)((phys >> 16) & 0xFF));
	outb(DMA_FLIPFLOP, 0xFF);         /* reset flip-flop */
	outb(DMA_CH2_COUNT, (unsigned char)(count & 0xFF));
	outb(DMA_CH2_COUNT, (unsigned char)((count >> 8) & 0xFF));
	outb(DMA_MODE, 0x4A);             /* single, increment, read (mem->dev), ch2 */
	outb(DMA_SINGLEMASK, 0x02);        /* unmask channel 2 */
}

/* ---- read one sector ---- */

static void lba_to_chs(unsigned int lba,
			unsigned int *cyl, unsigned int *head, unsigned int *sec)
{
	unsigned int tmp = lba / SECTORS_PER_TRACK;
	*sec  = (lba % SECTORS_PER_TRACK) + 1;
	*head = tmp % HEADS;
	*cyl  = tmp / HEADS;
}

static int fdc_do_read(unsigned int cyl, unsigned int head, unsigned int sec)
{
	unsigned char results[7];
	int i;

	dma_setup_read(DMA_BUF_ADDR, SECTOR_SIZE);

	fdc_irq_fired = 0;

	if (fdc_send(CMD_READ_DATA) != 0)   return -1;
	if (fdc_send((unsigned char)((head << 2) | current_drive)) != 0) return -1;
	if (fdc_send((unsigned char)cyl) != 0)  return -1;
	if (fdc_send((unsigned char)head) != 0) return -1;
	if (fdc_send((unsigned char)sec) != 0)  return -1;
	if (fdc_send(0x02) != 0)            return -1; /* 512 bytes/sector */
	if (fdc_send((unsigned char)SECTORS_PER_TRACK) != 0) return -1;
	if (fdc_send(0x1B) != 0)            return -1; /* gap3 */
	if (fdc_send(0xFF) != 0)            return -1; /* data length */

	if (fdc_wait_irq() != 0)
		return -2;

	for (i = 0; i < 7; i++) {
		if (fdc_recv(&results[i]) != 0)
			return -3;
	}

	if (results[0] & 0xC0) {
		io_putstr("fdc: CHS=");
		io_puthex(cyl);
		io_putchar('/');
		io_puthex(head);
		io_putchar('/');
		io_puthex(sec);
		io_putstr(" ST0=");
		io_puthex(results[0]);
		io_putstr(" ST1=");
		io_puthex(results[1]);
		io_putstr(" ST2=");
		io_puthex(results[2]);
		io_putchar('\n');
		return -1;
	}

	return 0;
}

static int fdc_do_write(unsigned int cyl, unsigned int head, unsigned int sec)
{
	unsigned char results[7];
	int i;

	dma_setup_write(DMA_BUF_ADDR, SECTOR_SIZE);

	fdc_irq_fired = 0;

	if (fdc_send(CMD_WRITE_DATA) != 0)   return -1;
	if (fdc_send((unsigned char)((head << 2) | current_drive)) != 0) return -1;
	if (fdc_send((unsigned char)cyl) != 0)  return -1;
	if (fdc_send((unsigned char)head) != 0) return -1;
	if (fdc_send((unsigned char)sec) != 0)  return -1;
	if (fdc_send(0x02) != 0)            return -1; /* 512 bytes/sector */
	if (fdc_send((unsigned char)SECTORS_PER_TRACK) != 0) return -1;
	if (fdc_send(0x1B) != 0)            return -1; /* gap3 */
	if (fdc_send(0xFF) != 0)            return -1; /* data length */

	if (fdc_wait_irq() != 0)
		return -2;

	for (i = 0; i < 7; i++) {
		if (fdc_recv(&results[i]) != 0)
			return -3;
	}

	if (results[0] & 0xC0) {
		io_putstr("fdc write: CHS=");
		io_puthex(cyl);
		io_putchar('/');
		io_puthex(head);
		io_putchar('/');
		io_puthex(sec);
		io_putstr(" ST0=");
		io_puthex(results[0]);
		io_putstr(" ST1=");
		io_puthex(results[1]);
		io_putstr(" ST2=");
		io_puthex(results[2]);
		io_putchar('\n');
		return -1;
	}

	return 0;
}

static int fdc_read_one(unsigned int lba)
{
	unsigned int cyl, head, sec;

	lba_to_chs(lba, &cyl, &head, &sec);

	if (cyl != cur_cylinder) {
		if (fdc_seek(cyl, head) != 0)
			return -1;
	}

	return fdc_do_read(cyl, head, sec);
}

static int fdc_write_one(unsigned int lba)
{
	unsigned int cyl, head, sec;

	lba_to_chs(lba, &cyl, &head, &sec);

	if (cyl != cur_cylinder) {
		if (fdc_seek(cyl, head) != 0)
			return -1;
	}

	return fdc_do_write(cyl, head, sec);
}

/* Aggressive retry: seek far away then back to improve head alignment. */
static int fdc_read_one_retry(unsigned int lba)
{
	unsigned int cyl, head, sec;

	lba_to_chs(lba, &cyl, &head, &sec);

	/* Seek to cylinder 40 (mid-disk) first, then to target.
	 * The long travel forces the stepper to settle accurately. */
	fdc_seek(cyl > 40 ? 0 : 40, 0);
	if (fdc_seek(cyl, head) != 0)
		return -1;

	return fdc_do_read(cyl, head, sec);
}

static int fdc_write_one_retry(unsigned int lba)
{
	unsigned int cyl, head, sec;

	lba_to_chs(lba, &cyl, &head, &sec);

	fdc_seek(cyl > 40 ? 0 : 40, 0);
	if (fdc_seek(cyl, head) != 0)
		return -1;

	return fdc_do_write(cyl, head, sec);
}

/* ---- public API ---- */

static int fdc_init_once(void)
{
	/* Mask IRQ 6 before reset so the completion interrupt doesn't
	 * fire before we're ready (IRQ 6 may already be unmasked from
	 * a previous init).  Then clear the flag and release the reset. */
	outb(PIC1_DATA, inb(PIC1_DATA) | 0x40);

	outb(FDC_DOR, 0x00);
	motor_on = 0;
	{
		unsigned int t0 = get_ticks_ms();
		while ((get_ticks_ms() - t0) < 10)
			;
	}

	fdc_irq_fired = 0;
	outb(FDC_DOR, (unsigned char)(0x0C | current_drive));

	/* Unmask IRQ 6 — the latched reset-completion interrupt fires now. */
	outb(PIC1_DATA, inb(PIC1_DATA) & ~0x40);

	if (fdc_wait_irq() != 0)
		return -1;

	{
		unsigned char st0, cyl;
		int i;
		for (i = 0; i < 4; i++)
			fdc_sense_interrupt(&st0, &cyl);

		outb(FDC_CCR, 0x00);

		if (fdc_send(CMD_SPECIFY) != 0)
			return -1;
		if (fdc_send(0x4F) != 0)
			return -1;
		if (fdc_send(0x1E) != 0)
			return -1;
	}

	fdc_motor_on();
	if (fdc_recalibrate() != 0)
		return -1;

	fdc_initialized = 1;
	return 0;
}

int disk_read_sectors(unsigned int lba, void *buf, unsigned int count)
{
	unsigned int i;

	if (!fdc_initialized) {
		if (fdc_init_once() != 0)
			return -1;
	}

	for (i = 0; i < count; i++) {
		int attempt, ok = 0;
		for (attempt = 0; attempt < MAX_RETRIES; attempt++) {
			int rc;
			if (attempt < 3) {
				rc = fdc_read_one(lba + i);
			} else {
				/* Long-seek retry: seek far away then back to
				 * force accurate head positioning. */
				rc = fdc_read_one_retry(lba + i);
			}
			if (rc == 0) {
				ok = 1;
				break;
			}
			fdc_reset();
			fdc_motor_on();
			fdc_recalibrate();
		}
		if (!ok) {
			io_putstr("disk: read failed LBA=");
			io_puthex(lba + i);
			io_putstr(" (sector ");
			io_puthex(i);
			io_putstr(" of ");
			io_puthex(count);
			io_putstr(")\n");
			return -1;
		}
		{
			unsigned int j;
			unsigned char *dst = (unsigned char *)buf + (i * SECTOR_SIZE);
			const unsigned char *src = (const unsigned char *)DMA_BUF_ADDR;
			for (j = 0; j < SECTOR_SIZE; j++)
				dst[j] = src[j];
		}
	}
	return 0;
}

int disk_write_sectors(unsigned int lba, const void *buf, unsigned int count)
{
	unsigned int i;

	if (!fdc_initialized) {
		if (fdc_init_once() != 0)
			return -1;
	}

	for (i = 0; i < count; i++) {
		int attempt, ok = 0;

		/* Copy the sector to write into the ISA DMA buffer first. */
		{
			unsigned int j;
			const unsigned char *src = (const unsigned char *)buf + (i * SECTOR_SIZE);
			volatile unsigned char *dst = (volatile unsigned char *)DMA_BUF_ADDR;
			for (j = 0; j < SECTOR_SIZE; j++)
				dst[j] = src[j];
		}

		for (attempt = 0; attempt < MAX_RETRIES; attempt++) {
			int rc;
			if (attempt < 3) {
				rc = fdc_write_one(lba + i);
			} else {
				rc = fdc_write_one_retry(lba + i);
			}
			if (rc == 0) {
				ok = 1;
				break;
			}
			fdc_reset();
			fdc_motor_on();
			fdc_recalibrate();
		}
		if (!ok) {
			io_putstr("disk: write failed LBA=");
			io_puthex(lba + i);
			io_putstr(" (sector ");
			io_puthex(i);
			io_putstr(" of ");
			io_puthex(count);
			io_putstr(")\n");
			return -1;
		}
	}
	return 0;
}
