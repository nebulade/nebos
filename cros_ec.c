/* Chrome OS embedded controller: battery, AC adapter and board sensors.
 *
 * Chromebooks do not expose ACPI _BIF/_BST through an interpreter we could
 * run; instead the Chrome EC implements the standard ACPI EC command
 * interface at ports 0x62/0x66, and its ACPI memory window 0x20..0xFF
 * mirrors the EC memmap 0x00..0xDF.  That gives us the same data the
 * firmware's DSDT reads:
 *
 *   EC_MEMMAP_TEMP_SENSOR 0x00..0x0f   temperatures (value + 200 = Kelvin)
 *   EC_MEMMAP_FAN         0x10..0x17   fan tachometers (16-bit, 0xffff none)
 *   EC_MEMMAP_TEMP_SENSOR_B 0x18..0x1f more temperatures (thermal version 2)
 *   EC_MEMMAP_SWITCHES    0x30         lid open / power button
 *   EC_MEMMAP_BATT_VOLT   0x40         present voltage (mV)
 *   EC_MEMMAP_BATT_RATE   0x44         present rate (mA, ACPI power unit)
 *   EC_MEMMAP_BATT_CAP    0x48         remaining capacity (mAh)
 *   EC_MEMMAP_BATT_FLAG   0x4c         AC/battery/charging state
 *   EC_MEMMAP_BATT_COUNT  0x4d         battery count
 *   EC_MEMMAP_BATT_INDEX  0x4e         selected battery
 *   EC_MEMMAP_BATT_DCAP   0x50         design capacity (mAh)
 *   EC_MEMMAP_BATT_DVLT   0x54         design voltage (mV)
 *   EC_MEMMAP_BATT_LFCC   0x58         last full charge capacity (mAh)
 *   EC_MEMMAP_BATT_CCNT   0x5c         cycle count
 *   EC_MEMMAP_BATT_MFGR/MODEL/SERIAL/TYPE 0x60..0x7f  (8-byte strings)
 *
 * The battery index is selected through EC_ACPI_MEM_BATTERY_INDEX (0x12);
 * longer strings come from EC_ACPI_MEM_STRINGS_FIFO (0x16).  The memmap is
 * read-only from the AP, so the only writes are the documented selector and
 * FIFO commands.
 *
 * The probe is gated on the EC ACPI version and the 'E','C' memmap id; the
 * ThinkPad backend is tried first and skips this one on Lenovo machines.
 */
#include "cros_ec.h"
#include "ec.h"
#include "tpec.h"
#include "serial.h"

#define CROS_ACPI_VERSION        0x00
#define CROS_ACPI_BATTERY_INDEX  0x12
#define CROS_ACPI_STRINGS_FIFO   0x16
#define CROS_ACPI_FAN_DUTY       0x04

#define CROS_MEM_ACPI_BASE       0x20
#define CROS_MEM_TEMP_SENSOR     0x00
#define CROS_MEM_TEMP_SENSOR_B   0x18
#define CROS_MEM_FAN             0x10
#define CROS_MEM_ID              0x20
#define CROS_MEM_ID_VERSION      0x22
#define CROS_MEM_THERMAL_VERSION 0x23
#define CROS_MEM_BATTERY_VERSION 0x24
#define CROS_MEM_SWITCHES        0x30
#define CROS_MEM_BATT_VOLT       0x40
#define CROS_MEM_BATT_RATE       0x44
#define CROS_MEM_BATT_CAP        0x48
#define CROS_MEM_BATT_FLAG       0x4c
#define CROS_MEM_BATT_COUNT      0x4d
#define CROS_MEM_BATT_INDEX      0x4e
#define CROS_MEM_BATT_DCAP       0x50
#define CROS_MEM_BATT_DVLT       0x54
#define CROS_MEM_BATT_LFCC       0x58
#define CROS_MEM_BATT_CCNT       0x5c
#define CROS_MEM_BATT_MFGR       0x60
#define CROS_MEM_BATT_MODEL      0x68
#define CROS_MEM_BATT_SERIAL     0x70

/* EC_MEMMAP_BATT_FLAG bits. */
#define CROS_BATT_AC_PRESENT     0x01
#define CROS_BATT_PRESENT        0x02
#define CROS_BATT_DISCHARGING    0x04
#define CROS_BATT_CHARGING       0x08
#define CROS_BATT_INVALID_DATA   0x20
#define CROS_BATT_CUT_OFF        0x40

/* EC_MEMMAP_SWITCHES bits. */
#define CROS_SWITCH_LID_OPEN     0x01
#define CROS_SWITCH_POWER_BUTTON 0x02

/* Device feature bits (EC_ACPI_MEM_DEVICE_FEATURES, 8 bytes). */
#define CROS_FEATURE_BATT        (1u << 17)
#define CROS_FEATURE_DFUD        (1u << 0)

#define CROS_UNKNOWN32 0xFFFFFFFFu
#define CROS_TEMP_OFFSET 73   /* raw + 200 = Kelvin, so raw - 73 = C */

static int cros_state = -1;   /* -1 unprobed, 0 absent, 1 present */
static unsigned int cros_acpi_version;
static unsigned int cros_id_version;
static unsigned int cros_battery_version;
static unsigned int cros_thermal_version;
static unsigned int cros_features;
static int cros_fifo = -1;    /* -1 unprobed, 0 absent, 1 present */

static int mem_read(unsigned int off, unsigned char *out)
{
	return ec_read((unsigned char)(CROS_MEM_ACPI_BASE + off), out);
}

static int mem_read16(unsigned int off, unsigned int *out)
{
	unsigned char lo, hi;

	if (mem_read(off, &lo) != 0 || mem_read(off + 1, &hi) != 0)
		return -1;
	*out = (unsigned int)lo | ((unsigned int)hi << 8);
	return 0;
}

static int mem_read32(unsigned int off, unsigned int *out)
{
	unsigned char b0, b1, b2, b3;

	if (mem_read(off, &b0) != 0 || mem_read(off + 1, &b1) != 0 ||
	    mem_read(off + 2, &b2) != 0 || mem_read(off + 3, &b3) != 0)
		return -1;
	*out = (unsigned int)b0 | ((unsigned int)b1 << 8) |
	       ((unsigned int)b2 << 16) | ((unsigned int)b3 << 24);
	return 0;
}

/* Read one of the 8-byte fixed strings from the memmap. */
static void read_short_string(unsigned int off, char *dst, unsigned int size)
{
	unsigned int i, n = 8;

	if (n > size - 1)
		n = size - 1;
	for (i = 0; i < n; i++) {
		unsigned char b;

		if (mem_read(off + i, &b) != 0)
			break;
		if (b == 0 || b == 0xFF)
			break;
		dst[i] = (char)b;
	}
	while (i && dst[i - 1] == ' ')
		i--;
	dst[i] = '\0';
}

/* Long strings from EC_ACPI_MEM_STRINGS_FIFO, when the EC supports them. */
static void read_fifo_string(unsigned int id, char *dst, unsigned int size)
{
	unsigned int i;

	if (ec_write(CROS_ACPI_STRINGS_FIFO, (unsigned char)id) != 0) {
		dst[0] = '\0';
		return;
	}
	for (i = 0; i + 1 < size; i++) {
		unsigned char b;

		if (ec_read(CROS_ACPI_STRINGS_FIFO, &b) != 0)
			break;
		if (b == 0)
			break;
		dst[i] = (char)b;
	}
	while (i && dst[i - 1] == ' ')
		i--;
	dst[i] = '\0';
}

static void read_string(unsigned int off, unsigned int fifo_id, char *dst,
			unsigned int size)
{
	if (cros_fifo > 0) {
		read_fifo_string(fifo_id, dst, size);
		if (dst[0])
			return;
	}
	read_short_string(off, dst, size);
}

static int battery_select(unsigned int index)
{
	unsigned char cur;
	unsigned int i;

	if (mem_read(CROS_MEM_BATT_INDEX, &cur) == 0 &&
	    cur == (unsigned char)index)
		return 0;
	if (ec_write(CROS_ACPI_BATTERY_INDEX, (unsigned char)index) != 0)
		return -1;
	/* The DSDT's BTSW waits 5 ms for BTIX to follow BTID.  Each read is
	 * already several port handshakes, so a bounded retry is enough. */
	for (i = 0; i < 50; i++) {
		if (mem_read(CROS_MEM_BATT_INDEX, &cur) == 0 &&
		    cur == (unsigned char)index)
			return 0;
	}
	return -1;
}

static void read_temp(unsigned int off, int *out)
{
	unsigned char b;

	if (mem_read(off, &b) != 0 || b >= 0xFC) {
		*out = -128;
		return;
	}
	*out = (int)b - CROS_TEMP_OFFSET;
}

void cros_ec_init(void)
{
	unsigned char ver = 0, id0 = 0, id1 = 0, b;
	unsigned int i, feature = 0;

	cros_state = 0;
	cros_acpi_version = 0;
	cros_id_version = 0;
	cros_battery_version = 0;
	cros_thermal_version = 0;
	cros_features = 0;
	cros_fifo = -1;

	/* The ThinkPad H8 has its own driver and the first claim on 0x62/0x66. */
	if (tpec_present()) {
		serial_puts(SERIAL_COM1, "cros_ec: ThinkPad EC present, skipped\n");
		return;
	}

	if (ec_read(CROS_ACPI_VERSION, &ver) != 0) {
		serial_puts(SERIAL_COM1, "cros_ec: no ACPI EC at 0x62/0x66\n");
		return;
	}
	if (mem_read(CROS_MEM_ID, &id0) != 0 ||
	    mem_read(CROS_MEM_ID + 1, &id1) != 0 ||
	    id0 != 'E' || id1 != 'C') {
		serial_puts(SERIAL_COM1, "cros_ec: no Chrome EC signature\n");
		return;
	}

	for (i = 0; i < 4; i++) {
		unsigned char f;

		if (mem_read(0x0A + i, &f) != 0) {
			feature = 0;
			break;
		}
		feature |= (unsigned int)f << (8 * i);
	}
	if (feature & CROS_FEATURE_DFUD)
		feature = 0;
	if (mem_read(CROS_MEM_ID_VERSION, &b) == 0)
		cros_id_version = b;
	if (mem_read(CROS_MEM_THERMAL_VERSION, &b) == 0)
		cros_thermal_version = b;
	if (mem_read(CROS_MEM_BATTERY_VERSION, &b) == 0)
		cros_battery_version = b;

	cros_acpi_version = ver;
	cros_features = feature;
	cros_state = 1;

	/* Probe the extended-string FIFO once (write 0, read the version). */
	{
		unsigned char fifo = 0xFF;

		if (ec_write(CROS_ACPI_STRINGS_FIFO, 0) == 0)
			(void)ec_read(CROS_ACPI_STRINGS_FIFO, &fifo);
		cros_fifo = (fifo >= 1 && fifo < 0xFF) ? 1 : 0;
	}

	serial_puts(SERIAL_COM1, "cros_ec: Chrome EC, ACPI v");
	serial_putdec(SERIAL_COM1, cros_acpi_version);
	serial_puts(SERIAL_COM1, ", id v");
	serial_putdec(SERIAL_COM1, cros_id_version);
	serial_puts(SERIAL_COM1, ", battery v");
	serial_putdec(SERIAL_COM1, cros_battery_version);
	serial_puts(SERIAL_COM1, ", thermal v");
	serial_putdec(SERIAL_COM1, cros_thermal_version);
	serial_puts(SERIAL_COM1, ", features ");
	serial_puthex(SERIAL_COM1, cros_features);
	serial_puts(SERIAL_COM1, ", fifo ");
	serial_puts(SERIAL_COM1, cros_fifo ? "yes" : "no");
	serial_puts(SERIAL_COM1, "\n");

	if (mem_read(CROS_MEM_BATT_COUNT, &b) == 0) {
		serial_puts(SERIAL_COM1, "cros_ec: batteries ");
		serial_putdec(SERIAL_COM1, b);
		serial_puts(SERIAL_COM1, ", feature batt ");
		serial_puts(SERIAL_COM1,
			    (cros_features & CROS_FEATURE_BATT) ? "yes" : "no");
		serial_puts(SERIAL_COM1, "\n");
	}
}

int cros_ec_present(void)
{
	if (cros_state < 0)
		cros_ec_init();
	return cros_state > 0;
}

void cros_ec_power_read(struct power_info *out, int detail)
{
	unsigned int i, count;
	unsigned char flags = 0;
	unsigned char count_byte = 0;
	int ac = -1;

	for (i = 0; i < sizeof(*out); i++)
		((unsigned char *)out)[i] = 0;
	out->ac_online = -1;

	if (cros_state < 0)
		cros_ec_init();
	if (!cros_state)
		return;

	out->present = 1;
	out->source = SYS_POWER_SRC_CROS;
	out->ec_rev = cros_acpi_version;

	if (mem_read(CROS_MEM_BATT_FLAG, &flags) == 0)
		ac = (flags & CROS_BATT_AC_PRESENT) ? 1 : 0;
	out->ac_online = ac;

	if (mem_read(CROS_MEM_BATT_COUNT, &count_byte) != 0 ||
	    count_byte == 0 || count_byte > POWER_BATT_MAX)
		count = 1;
	else
		count = count_byte;

	for (i = 0; i < count; i++) {
		struct power_battery *b = &out->battery[i];
		unsigned int volt = 0, rate = 0, cap = 0, lfcc = 0;
		unsigned int design = 0, dvlt = 0, cycles = 0;
		unsigned int full;
		unsigned char f = 0;

		if (battery_select(i) != 0) {
			b->error = 1;
			continue;
		}
		if (mem_read(CROS_MEM_BATT_FLAG, &f) != 0) {
			b->error = 1;
			continue;
		}

		b->status = f;
		b->present = (f & CROS_BATT_PRESENT) ? 1 : 0;
		b->charging = (f & CROS_BATT_CHARGING) ? 1 : 0;
		b->discharging = (f & CROS_BATT_DISCHARGING) ? 1 : 0;
		if (f & (CROS_BATT_INVALID_DATA | CROS_BATT_CUT_OFF))
			b->error = 1;
		if (!b->present)
			continue;

		if (mem_read32(CROS_MEM_BATT_VOLT, &volt) != 0 ||
		    mem_read32(CROS_MEM_BATT_RATE, &rate) != 0 ||
		    mem_read32(CROS_MEM_BATT_CAP, &cap) != 0 ||
		    mem_read32(CROS_MEM_BATT_LFCC, &lfcc) != 0)
			b->error = 1;

		if (volt != CROS_UNKNOWN32)
			b->voltage_mv = volt;
		if (rate != CROS_UNKNOWN32)
			b->rate = (int)rate;
		if (cap != CROS_UNKNOWN32)
			b->remaining = cap;
		if (lfcc != CROS_UNKNOWN32)
			b->full = lfcc;

		if (detail) {
			if (mem_read32(CROS_MEM_BATT_DCAP, &design) != 0 ||
			    mem_read32(CROS_MEM_BATT_DVLT, &dvlt) != 0 ||
			    mem_read32(CROS_MEM_BATT_CCNT, &cycles) != 0)
				b->error = 1;
			if (design != CROS_UNKNOWN32)
				b->design = design;
			if (cycles != CROS_UNKNOWN32)
				b->cycle_count = cycles;
			(void)dvlt;   /* design voltage is informational */
		}

		/* _BIF declares mAh/mA on Chromebooks. */
		b->unit_known = 1;
		b->unit_mwh = 0;

		/* The Cyan DSDT compensates a full pack that the EC reports as
		 * neither charging nor discharging while AC is present. */
		full = b->full ? b->full : b->design;
		if (ac > 0 && !(b->discharging && b->charging) && full &&
		    b->remaining > full - full / 16 &&
		    b->remaining < full + full / 16)
			b->remaining = full;

		if (full && b->remaining &&
		    b->remaining <= full + full / 4) {
			unsigned int pct =
				(b->remaining * 100u + full / 2u) / full;

			if (pct > 100)
				pct = 100;
			b->percent = (int)pct;
			b->percent_valid = 1;
		}

		if (detail) {
			read_string(CROS_MEM_BATT_MFGR, 3, b->manufacturer,
				    sizeof(b->manufacturer));
			read_string(CROS_MEM_BATT_MODEL, 1, b->model,
				    sizeof(b->model));
			read_string(CROS_MEM_BATT_SERIAL, 2, b->serial,
				    sizeof(b->serial));
		}

		out->battery_count++;
	}
}

void cros_ec_sensors_read(struct cros_ec_sensors *out)
{
	unsigned int i;
	unsigned char b;

	for (i = 0; i < sizeof(*out); i++)
		((unsigned char *)out)[i] = 0;
	for (i = 0; i < CROS_EC_TEMP_MAX; i++)
		out->temp_c[i] = -128;

	if (cros_state < 0)
		cros_ec_init();
	if (!cros_state)
		return;

	out->present = 1;
	out->acpi_version = cros_acpi_version;
	out->battery_version = cros_battery_version;
	out->thermal_version = cros_thermal_version;

	for (i = 0; i < 16; i++)
		read_temp(CROS_MEM_TEMP_SENSOR + i, &out->temp_c[i]);
	for (i = 0; i < 8; i++)
		read_temp(CROS_MEM_TEMP_SENSOR_B + i, &out->temp_c[16 + i]);

	for (i = 0; i < CROS_EC_FAN_MAX; i++) {
		unsigned int v;

		if (mem_read16(CROS_MEM_FAN + 2 * i, &v) == 0 &&
		    v != 0xFFFF) {
			out->fan_rpm[i] = v;
			out->fan_present |= 1u << i;
		}
	}
	if (ec_read(CROS_ACPI_FAN_DUTY, &b) == 0)
		out->fan_duty = b;
	if (mem_read(CROS_MEM_SWITCHES, &b) == 0)
		out->switches = b;
}
