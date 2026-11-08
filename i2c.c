/* Polled Synopsys DesignWare APB I2C master (Braswell LPSS).
 *
 * Only master transfers are implemented.  The bus runs in fast mode (the
 * SCL counts below give 300-400 kHz depending on the 100/133 MHz LPSS
 * clock), which is within the touchpad's 400 kHz rating and keeps a 34-byte
 * report read short.
 *
 * Register offsets follow the DesignWare databook; the component type at
 * 0xFC is the usual "DW" signature (0x44570140).  The Intel LPSS private
 * block at 0x800 holds the reset release register; firmware has already
 * released the controller, but the I2C FSM only drains its FIFOs after an
 * explicit assert/release cycle, so i2c_dw_init() always does one. */

#include "i2c.h"
#include "timer.h"

#define DW_IC_CON		0x00
#define DW_IC_TAR		0x04
#define DW_IC_DATA_CMD		0x10
#define DW_IC_SS_SCL_HCNT	0x14
#define DW_IC_SS_SCL_LCNT	0x18
#define DW_IC_FS_SCL_HCNT	0x1c
#define DW_IC_FS_SCL_LCNT	0x20
#define DW_IC_RAW_INTR_STAT	0x34
#define DW_IC_RX_TL		0x38
#define DW_IC_TX_TL		0x3c
#define DW_IC_CLR_INTR		0x40
#define DW_IC_CLR_TX_ABRT	0x54
#define DW_IC_ENABLE		0x6c
#define DW_IC_STATUS		0x70
#define DW_IC_TX_ABRT_SOURCE	0x80
#define DW_IC_TXFLR		0x74
#define DW_IC_RXFLR		0x78
#define DW_IC_ENABLE_STATUS	0x9c
#define DW_IC_FS_SPKLEN		0xa0
#define DW_IC_COMP_VERSION	0xf8
#define DW_IC_COMP_TYPE		0xfc
#define DW_IC_COMP_TYPE_VALUE	0x44570140u
#define DW_LPSS_RESET		0x804

#define IC_CON_MASTER		0x00000001u
#define IC_CON_SPEED_FS		0x00000004u
#define IC_CON_RESTART_EN	0x00000020u
#define IC_CON_SLAVE_DISABLE	0x00000040u

#define IC_DATA_CMD_READ	0x00000100u
#define IC_DATA_CMD_STOP	0x00000200u
#define IC_DATA_CMD_RESTART	0x00000400u

#define IC_STATUS_TFNF		0x00000002u
#define IC_STATUS_RFNE		0x00000004u
#define IC_STATUS_TFE		0x00000004u
#define IC_STATUS_MST_ACTIVITY	0x00000020u

#define IC_ENABLE_EN		0x00000001u
#define IC_ENABLE_ABORT		0x00000002u

#define IC_RAW_TX_ABRT		0x00000040u

unsigned int i2c_last_abort;

static unsigned int dw_rd(unsigned int base, unsigned int off)
{
	return *(volatile unsigned int *)(base + off);
}

static void dw_wr(unsigned int base, unsigned int off, unsigned int val)
{
	*(volatile unsigned int *)(base + off) = val;
}

static void dw_delay_ms(unsigned int ms)
{
	unsigned int t0 = get_ticks_ms();

	while (get_ticks_ms() - t0 < ms)
		;
}

/* Wait until (status & mask) == want.  A bus abort ends the wait early so a
 * missing device cannot burn the whole timeout. */
static int dw_wait(unsigned int base, unsigned int off, unsigned int mask,
		   unsigned int want, unsigned int ms)
{
	unsigned int t0 = get_ticks_ms();

	while (get_ticks_ms() - t0 < ms) {
		if ((dw_rd(base, off) & mask) == want)
			return 0;
		if (dw_rd(base, DW_IC_RAW_INTR_STAT) & IC_RAW_TX_ABRT)
			return -1;
	}
	return -1;
}

static void dw_clear_irq(unsigned int base)
{
	(void)dw_rd(base, DW_IC_CLR_TX_ABRT);
	(void)dw_rd(base, DW_IC_CLR_INTR);
}

/* Stop anything in flight and put the controller back in the idle state. */
static void dw_recover(unsigned int base)
{
	dw_wr(base, DW_IC_ENABLE, IC_ENABLE_EN | IC_ENABLE_ABORT);
	dw_delay_ms(1);
	dw_wr(base, DW_IC_ENABLE, 0);
	dw_clear_irq(base);
	dw_wr(base, DW_IC_ENABLE, IC_ENABLE_EN);
}

unsigned int i2c_dw_comp_type(unsigned int base)
{
	return dw_rd(base, DW_IC_COMP_TYPE);
}

void i2c_dw_reset_release(unsigned int base)
{
	/* Bay Trail/Braswell put the LPSS private block at 0x800 (reset at
	 * 0x804); newer LPSS layouts put it at 0x200 (reset at 0x204).  The
	 * Linux intel-lpss driver asserts the reset first and then releases
	 * it, so do both here: a block left in functional reset keeps the
	 * register file alive but never clocks the I2C master FSM, which
	 * looks exactly like "commands sit in the TX FIFO forever". */
	dw_wr(base, 0x804, 0);
	dw_wr(base, 0x204, 0);
	dw_delay_ms(1);
	dw_wr(base, 0x804, 3);
	dw_wr(base, 0x204, 3);
	dw_delay_ms(1);
}

int i2c_dw_present(unsigned int base)
{
	return i2c_dw_comp_type(base) == DW_IC_COMP_TYPE_VALUE;
}

int i2c_dw_init(unsigned int base)
{
	unsigned int t0;

	if (!i2c_dw_present(base))
		return -1;

	/* Release the LPSS block from reset. */
	i2c_dw_reset_release(base);

	dw_wr(base, DW_IC_ENABLE, 0);
	t0 = get_ticks_ms();
	while (get_ticks_ms() - t0 < 20) {
		if (!(dw_rd(base, DW_IC_ENABLE_STATUS) & 1))
			break;
	}

	dw_wr(base, DW_IC_CON, IC_CON_MASTER | IC_CON_SPEED_FS |
	       IC_CON_RESTART_EN | IC_CON_SLAVE_DISABLE);
	/* 400 kHz at 133 MHz; 300 kHz at 100 MHz.  The LPSS clock is one
	 * of those, so the bus never exceeds the touchpad's 400 kHz. */
	dw_wr(base, DW_IC_FS_SCL_HCNT, 112);
	dw_wr(base, DW_IC_FS_SCL_LCNT, 211);
	dw_wr(base, DW_IC_FS_SPKLEN, 1);
	dw_wr(base, DW_IC_RX_TL, 0);
	dw_wr(base, DW_IC_TX_TL, 0);
	dw_clear_irq(base);

	dw_wr(base, DW_IC_ENABLE, IC_ENABLE_EN);
	t0 = get_ticks_ms();
	while (get_ticks_ms() - t0 < 20) {
		if (dw_rd(base, DW_IC_ENABLE_STATUS) & 1)
			return 0;
	}
	return -1;
}

int i2c_dw_transfer(unsigned int base, unsigned int addr,
		    const unsigned char *wr, unsigned int wrlen,
		    unsigned char *rd, unsigned int rdlen)
{
	unsigned int i, cmd, t0, deadline;

	if (!wrlen && !rdlen)
		return 0;

	/* Worst case at 300 kHz, plus slack for clock stretching.  Keep it
	 * short: a failed poll must not block the housekeeping loop. */
	deadline = 10 + (wrlen + rdlen);

	dw_wr(base, DW_IC_TAR, addr & 0x7f);
	dw_clear_irq(base);
	i2c_last_abort = 0;

	for (i = 0; i < wrlen; i++) {
		if (dw_wait(base, DW_IC_STATUS, IC_STATUS_TFNF,
			    IC_STATUS_TFNF, deadline))
			goto fail;
		cmd = wr[i];
		if (i + 1 == wrlen && !rdlen)
			cmd |= IC_DATA_CMD_STOP;
		dw_wr(base, DW_IC_DATA_CMD, cmd);
	}

	for (i = 0; i < rdlen; i++) {
		if (dw_wait(base, DW_IC_STATUS, IC_STATUS_TFNF,
			    IC_STATUS_TFNF, deadline))
			goto fail;
		cmd = IC_DATA_CMD_READ;
		if (i == 0 && wrlen)
			cmd |= IC_DATA_CMD_RESTART;
		if (i + 1 == rdlen)
			cmd |= IC_DATA_CMD_STOP;
		dw_wr(base, DW_IC_DATA_CMD, cmd);
		if (dw_wait(base, DW_IC_STATUS, IC_STATUS_RFNE,
			    IC_STATUS_RFNE, deadline))
			goto fail;
		rd[i] = (unsigned char)(dw_rd(base, DW_IC_DATA_CMD) & 0xff);
	}

	if (!rdlen) {
		/* A write-only transfer is done when the controller has
		 * consumed every command (TX FIFO empty).  MST_ACTIVITY is
		 * already low before the transfer starts, so checking it
		 * alone would report a hung FIFO as success. */
		t0 = get_ticks_ms();
		while (get_ticks_ms() - t0 < deadline) {
			if (dw_rd(base, DW_IC_STATUS) & IC_STATUS_TFE)
				break;
			if (dw_rd(base, DW_IC_RAW_INTR_STAT) & IC_RAW_TX_ABRT)
				goto fail;
		}
		if (!(dw_rd(base, DW_IC_STATUS) & IC_STATUS_TFE))
			goto fail;
		dw_clear_irq(base);
		return 0;
	}

	/* Wait for the STOP to finish. */
	t0 = get_ticks_ms();
	while (get_ticks_ms() - t0 < deadline) {
		if (!(dw_rd(base, DW_IC_STATUS) & IC_STATUS_MST_ACTIVITY))
			break;
	}
	if (dw_rd(base, DW_IC_RAW_INTR_STAT) & IC_RAW_TX_ABRT)
		goto fail;

	dw_clear_irq(base);
	return 0;

fail:
	i2c_last_abort = dw_rd(base, DW_IC_TX_ABRT_SOURCE);
	dw_recover(base);
	return -1;
}
