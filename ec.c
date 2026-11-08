/* ACPI embedded-controller access; see ec.h. */
#include "ec.h"
#include "pci.h"

#define EC_DATA 0x62
#define EC_STS  0x66

#define EC_OBF  0x01   /* output buffer full */
#define EC_IBF  0x02   /* input buffer full */
#define EC_READ 0x80
#define EC_WRITE 0x81

/* Enough for a slow EC command (tens of milliseconds of port polls) but
 * short enough that a dead controller cannot stall the boot or a syscall. */
#define EC_POLL_MAX 20000

static int ec_wait(unsigned char mask, int set)
{
	unsigned int i;

	for (i = 0; i < EC_POLL_MAX; i++) {
		unsigned char st = port_inb(EC_STS);

		if (st == 0xFF)           /* no controller on these ports */
			return -1;
		if (set ? (st & mask) != 0 : (st & mask) == 0)
			return 0;
	}
	return -1;
}

/* Keep a stale output byte (a hotkey event) from shadowing the register we
 * are about to read. */
static int ec_drain(void)
{
	unsigned int i;

	for (i = 0; i < 64; i++) {
		unsigned char st = port_inb(EC_STS);

		if (st == 0xFF)
			return -1;
		if (!(st & EC_OBF))
			return 0;
		(void)port_inb(EC_DATA);
	}
	return -1;
}

int ec_read(unsigned char reg, unsigned char *out)
{
	if (ec_drain())
		return -1;
	if (ec_wait(EC_IBF, 0))
		return -1;
	port_outb(EC_STS, EC_READ);
	if (ec_wait(EC_IBF, 0))
		return -1;
	port_outb(EC_DATA, reg);
	if (ec_wait(EC_OBF, 1))
		return -1;
	*out = port_inb(EC_DATA);
	return 0;
}

int ec_write(unsigned char reg, unsigned char val)
{
	if (ec_drain())
		return -1;
	if (ec_wait(EC_IBF, 0))
		return -1;
	port_outb(EC_STS, EC_WRITE);
	if (ec_wait(EC_IBF, 0))
		return -1;
	port_outb(EC_DATA, reg);
	if (ec_wait(EC_IBF, 0))
		return -1;
	port_outb(EC_DATA, val);
	if (ec_wait(EC_IBF, 0))
		return -1;
	return 0;
}

int ec_read16(unsigned char reg, unsigned int *out)
{
	unsigned char lo, hi;

	if (ec_read(reg, &lo) != 0 || ec_read((unsigned char)(reg + 1), &hi) != 0)
		return -1;
	*out = (unsigned int)lo | ((unsigned int)hi << 8);
	return 0;
}
