/* ThinkPad embedded-controller sensors and power sources (see tpec.h).
 *
 * The X201 (i5 M 520, Arrandale) has no IT87xx Super I/O like the
 * AT3IONT-I.  Its board temperatures, fan tachometer, battery and AC state
 * live behind the ACPI embedded controller, which Linux's thinkpad_acpi
 * reads through the same ports: command/status 0x66, data 0x62.  The EC
 * protocol is simple enough to drive directly:
 *
 *   write 0x80 (read) to 0x66, then the register index to 0x62,
 *   wait for OBF (0x01) and read the byte back from 0x62.
 *
 * The SMBIOS type 1 entry gates the probe to Lenovo/IBM machines, so the
 * EC ports are never touched on the ASUS board, QEMU or other hardware.
 *
 * Two details matter on real hardware:
 *  - There is no SCI handler, so hotkey events queue up in the output buffer.
 *    A pending byte is drained before every command; otherwise the EC would
 *    never replace it with the register we asked for.
 *  - Firmware keeps running in legacy/SMM mode (we have no ACPI interpreter),
 *    so an SMI can access the EC between our port accesses.  The status
 *    handshake plus bounded polling keeps that to an occasional bad sample.
 *    Temperature/fan reads are read-only; the battery reader writes only the
 *    H8 information-page selector at 0x81 and restores it afterwards.
 *
 * EC register map (thinkpad_acpi/H8): 0x78..0x7F and 0xC0..0xC7 temperatures
 * (0x80 = sensor not present, 0x00 = register not implemented), 0x84/0x85
 * fan tachometer LSB/MSB and 0x2F fan control (bit 7 automatic, bit 6
 * disengaged/full speed, bits 2:0 level).  EC 0xEF is the functional
 * revision: revision 3 and later only implement the 0x78..0x7F sensors.
 *
 * Battery/AC: 0x38/0x39 are the battery 0/1 status bytes (bit 7 present,
 * bit 6 discharging, bit 5 charging) and 0x46 bit 4 is the AC-adapter
 * state.  Battery data is a 16-byte window at 0xA0..0xAF selected by 0x81:
 * the high nibble is the battery (0x00/0x10), the low nibble the page
 * (0 dynamic, 1 mode, 2 static design data, 5 manufacturer, 6 model).
 */
#include "tpec.h"
#include "ec.h"
#include "paging.h"
#include "serial.h"

#define TP_FAN_LEVEL  0x2F
#define TP_FAN_LSB    0x84
#define TP_FAN_MSB    0x85
#define TP_TEMP_LO    0x78   /* temp_c[0..7] */
#define TP_TEMP_HI    0xC0   /* temp_c[8..15] */
#define TP_EC_REV     0xEF   /* EC functional revision */

#define TP_BAT0_STATUS 0x38  /* bit 7 present, 6 discharging, 5 charging */
#define TP_BAT1_STATUS 0x39
#define TP_AC_STATUS   0x46  /* bit 4: AC adapter online */
#define TP_BAT_PAGE    0x81  /* information-page selector */
#define TP_BAT_DATA    0xA0  /* 16-byte selected page */

#define TP_BAT_PAGE_DYNAMIC 0
#define TP_BAT_PAGE_MODE    1
#define TP_BAT_PAGE_STATIC  2
#define TP_BAT_PAGE_MFR     5
#define TP_BAT_PAGE_MODEL   6

#define TP_BAT_REMAINING 0x00
#define TP_BAT_FULL      0x02
#define TP_BAT_RSOC      0x06
#define TP_BAT_RATE      0x08
#define TP_BAT_VOLTAGE   0x0A

static int tpec_state = -1;   /* -1 unprobed, 0 absent, 1 present */
static unsigned int tpec_ec_rev;
static char tpec_vendor[16];
static char tpec_product[32];
static char tpec_version[32];

static unsigned int rd32(const unsigned char *p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
	       ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static void copy_trim(char *dst, unsigned int size, const unsigned char *src)
{
	unsigned int i = 0;

	while (i + 1 < size && src && src[i]) {
		dst[i] = (char)src[i];
		i++;
	}
	while (i && dst[i - 1] == ' ')
		i--;
	dst[i] = '\0';
}

/* Map every 4 MB page touched by [addr, addr+len); SMBIOS tables can sit
 * above the 128 MB identity map on larger machines (same approach as
 * acpi.c). */
static void map_range(unsigned int addr, unsigned int len)
{
	unsigned int p = addr & 0xFFC00000u;
	unsigned int last = (addr + len - 1u) & 0xFFC00000u;

	for (;;) {
		paging_map_ram(p, 0x400000u);
		if (p >= last || p > 0xFFC00000u - 0x400000u)
			break;
		p += 0x400000u;
	}
}

/* The n-th (1-based) string that follows a formatted SMBIOS structure. */
static const unsigned char *smbios_string(const unsigned char *p,
					  const unsigned char *end,
					  unsigned int index)
{
	unsigned int n = 1;

	if (!index)
		return 0;
	while (p < end && *p) {
		const unsigned char *s = p;

		if (n == index)
			return s;
		while (p < end && *p)
			p++;
		p++;
		n++;
	}
	return 0;
}

static int smbios_type1(char vendor[16], char product[32], char version[32])
{
	const unsigned char *ep = 0;
	const unsigned char *table, *end, *p;
	unsigned int addr, table_addr, table_len, i;

	for (addr = 0xF0000; addr < 0x100000; addr += 16) {
		const unsigned char *q = (const unsigned char *)addr;
		unsigned char sum = 0;
		unsigned int len;

		if (q[0] != '_' || q[1] != 'S' || q[2] != 'M' || q[3] != '_')
			continue;
		len = q[5];
		if (len < 0x1F || len > 0x40)
			continue;
		for (i = 0; i < len; i++)
			sum = (unsigned char)(sum + q[i]);
		if (sum)
			continue;
		if (q[0x10] != '_' || q[0x11] != 'D' || q[0x12] != 'M' ||
		    q[0x13] != 'I' || q[0x14] != '_')
			continue;
		ep = q;
		break;
	}
	if (!ep)
		return -1;

	table_len = (unsigned int)ep[0x16] | ((unsigned int)ep[0x17] << 8);
	table_addr = rd32(ep + 0x18);
	if (table_addr < 0x1000 || table_addr >= 0xFF000000u ||
	    table_len < 4 || table_len > 0x10000u ||
	    table_addr + table_len < table_addr)
		return -1;

	map_range(table_addr, table_len);
	table = (const unsigned char *)table_addr;
	end = table + table_len;

	for (p = table; p + 4 <= end; ) {
		unsigned char type = p[0];
		unsigned int len = p[1];
		const unsigned char *strs, *next;

		if (len < 4)
			break;
		if (type == 127)          /* end-of-table */
			break;
		strs = p + len;
		if (strs > end)
			break;
		if (type == 1) {
			if (len >= 8) {
				copy_trim(vendor, 16,
					  smbios_string(strs, end, p[4]));
				copy_trim(product, 32,
					  smbios_string(strs, end, p[5]));
				copy_trim(version, 32,
					  smbios_string(strs, end, p[6]));
				return 0;
			}
			break;
		}
		next = strs;
		while (next + 1 < end && !(next[0] == 0 && next[1] == 0))
			next++;
		next += 2;
		if (next > end)
			break;
		p = next;
	}
	return -1;
}

/* Select an H8 battery information page.  Only writes 0x81 when the page
 * already differs; *dirty remembers that the original page must be restored. */
static int ec_battery_page(unsigned int battery, unsigned int page, int *dirty)
{
	unsigned char cur;
	unsigned char want = (unsigned char)((battery << 4) | page);

	if (ec_read(TP_BAT_PAGE, &cur) == 0 && cur == want)
		return 0;
	if (ec_write(TP_BAT_PAGE, want) != 0)
		return -1;
	*dirty = 1;
	return 0;
}

static void ec_read_string(char *dst, unsigned int size, unsigned char reg)
{
	unsigned int i = 0;

	while (i + 1 < size) {
		unsigned char b;

		if (ec_read((unsigned char)(reg + i), &b) != 0)
			break;
		if (b == 0x00 || b == 0xFF)
			break;
		dst[i++] = (char)b;
	}
	while (i && dst[i - 1] == ' ')
		i--;
	dst[i] = '\0';
}

static int str_eq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static void log_info(void)
{
	serial_puts(SERIAL_COM1, "tpec: ");
	if (tpec_vendor[0]) {
		serial_puts(SERIAL_COM1, tpec_vendor);
		serial_puts(SERIAL_COM1, " ");
	}
	if (tpec_product[0]) {
		serial_puts(SERIAL_COM1, tpec_product);
		serial_puts(SERIAL_COM1, " ");
	}
	if (tpec_version[0])
		serial_puts(SERIAL_COM1, tpec_version);
}

void tpec_init(void)
{
	char vendor[16], product[32], version[32];
	unsigned char probe, rev;

	tpec_state = 0;
	tpec_vendor[0] = '\0';
	tpec_product[0] = '\0';
	tpec_version[0] = '\0';

	if (smbios_type1(vendor, product, version) != 0) {
		serial_puts(SERIAL_COM1, "tpec: no SMBIOS type 1, disabled\n");
		return;
	}
	copy_trim(tpec_vendor, sizeof(tpec_vendor), (const unsigned char *)vendor);
	copy_trim(tpec_product, sizeof(tpec_product),
		  (const unsigned char *)product);
	copy_trim(tpec_version, sizeof(tpec_version),
		  (const unsigned char *)version);

	if (!str_eq(vendor, "LENOVO") && !str_eq(vendor, "IBM")) {
		log_info();
		serial_puts(SERIAL_COM1, ": not a ThinkPad vendor, no EC probe\n");
		return;
	}

	if (ec_read(TP_FAN_LEVEL, &probe) != 0) {
		log_info();
		serial_puts(SERIAL_COM1, ": EC not responding\n");
		return;
	}
	if (ec_read(TP_EC_REV, &rev) == 0)
		tpec_ec_rev = rev;
	tpec_state = 1;

	log_info();
	serial_puts(SERIAL_COM1, ": EC ok, fw rev ");
	serial_putdec(SERIAL_COM1, tpec_ec_rev);
	serial_puts(SERIAL_COM1, ", fan level ");
	serial_puthex(SERIAL_COM1, probe);
	serial_puts(SERIAL_COM1, "\n");
}

int tpec_present(void)
{
	if (tpec_state < 0)
		tpec_init();
	return tpec_state > 0;
}

void tpec_read(struct tpec_info *out)
{
	unsigned char b;
	unsigned int i;

	out->present = 0;
	out->vendor[0] = '\0';
	out->product[0] = '\0';
	out->version[0] = '\0';
	for (i = 0; i < TPEC_TEMP_COUNT; i++)
		out->temp_c[i] = -128;
	out->fan_rpm = 0;
	out->fan_level = 0;

	if (tpec_state < 0)
		tpec_init();
	if (!tpec_state)
		return;

	out->present = 1;
	for (i = 0; i < sizeof(out->vendor) - 1 && tpec_vendor[i]; i++)
		out->vendor[i] = tpec_vendor[i];
	out->vendor[i] = '\0';
	for (i = 0; i < sizeof(out->product) - 1 && tpec_product[i]; i++)
		out->product[i] = tpec_product[i];
	out->product[i] = '\0';
	for (i = 0; i < sizeof(out->version) - 1 && tpec_version[i]; i++)
		out->version[i] = tpec_version[i];
	out->version[i] = '\0';

	for (i = 0; i < 8; i++) {
		if (ec_read((unsigned char)(TP_TEMP_LO + i), &b) != 0)
			continue;
		/* 0x80 = sensor present but unavailable, 0x00 = register not
		 * implemented (thinkpad_acpi treats a block of zeros as an EC
		 * that does not expose the register map at all). */
		if (b != 0x80 && b != 0x00)
			out->temp_c[i] = (signed char)b;
	}
	/* EC functional revision 3 and later only uses 0x78..0x7F; the 0xC0
	 * block may hold unrelated values (e.g. a power supply ID) there. */
	if (tpec_ec_rev < 3) {
		for (i = 0; i < 8; i++) {
			if (ec_read((unsigned char)(TP_TEMP_HI + i), &b) != 0)
				continue;
			if (b != 0x80 && b != 0x00)
				out->temp_c[8 + i] = (signed char)b;
		}
	}

	{
		unsigned char lo = 0, hi = 0;

		if (ec_read(TP_FAN_LSB, &lo) == 0 &&
		    ec_read(TP_FAN_MSB, &hi) == 0) {
			if (!(lo == 0xFF && hi == 0xFF))
				out->fan_rpm = ((unsigned int)hi << 8) | lo;
		}
	}
	if (ec_read(TP_FAN_LEVEL, &b) == 0)
		out->fan_level = b;
}

static void tpec_copy_ident(char *dst, unsigned int size, const char *src)
{
	unsigned int i;

	for (i = 0; i + 1 < size && src[i]; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

static void tpec_read_battery(struct power_battery *b, unsigned int index,
			      int detail, int *page_dirty)
{
	unsigned char st;
	unsigned int v;
	unsigned int remaining = 0, full = 0, rsoc = 0xFFFF;
	int have_remaining = 0, have_full = 0, have_rsoc = 0;
	int have_voltage = 0;

	if (ec_read((unsigned char)(TP_BAT0_STATUS + index), &st) != 0) {
		b->error = 1;
		return;
	}
	b->status = st;
	b->present = (st & 0x80) ? 1 : 0;
	b->charging = (st & 0x20) ? 1 : 0;
	b->discharging = (st & 0x40) ? 1 : 0;
	if (!b->present)
		return;

	if (ec_battery_page(index, TP_BAT_PAGE_DYNAMIC, page_dirty) != 0) {
		b->error = 1;
		return;
	}
	if (ec_read16((unsigned char)(TP_BAT_DATA + TP_BAT_REMAINING), &v) == 0) {
		remaining = v;
		have_remaining = (v != 0xFFFF);
	}
	if (ec_read16((unsigned char)(TP_BAT_DATA + TP_BAT_FULL), &v) == 0) {
		full = v;
		have_full = (v != 0xFFFF);
	}
	if (ec_read16((unsigned char)(TP_BAT_DATA + TP_BAT_RSOC), &v) == 0) {
		rsoc = v;
		have_rsoc = (v != 0xFFFF);
	}
	if (ec_read16((unsigned char)(TP_BAT_DATA + TP_BAT_RATE), &v) == 0) {
		if (v != 0xFFFF)
			b->rate = (int)(short)v;
	} else {
		b->error = 1;
	}
	if (ec_read16((unsigned char)(TP_BAT_DATA + TP_BAT_VOLTAGE), &v) == 0 &&
	    v != 0xFFFF) {
		b->voltage_mv = v;
		have_voltage = 1;
	} else {
		b->error = 1;
	}
	b->remaining = have_remaining ? remaining : 0;
	b->full = have_full ? full : 0;
	if (!have_remaining || !have_full || !have_voltage)
		b->error = 1;

	/* Percentage is unit-independent (both capacities use the same unit).
	 * Reject the 0xFFFF "unknown" marker and obviously inconsistent reads;
	 * fall back to the SBS relative state of charge if that is sane. */
	if (have_full && have_remaining && full > 0 && full != 0xFFFF &&
	    remaining != 0xFFFF && remaining <= full + full / 4) {
		b->percent = (int)((remaining * 100u + full / 2u) / full);
		if (b->percent > 100)
			b->percent = 100;
		b->percent_valid = 1;
	} else if (have_rsoc && rsoc <= 100) {
		b->percent = (int)rsoc;
		b->percent_valid = 1;
	}

	if (!detail)
		return;
	if (ec_battery_page(index, TP_BAT_PAGE_MODE, page_dirty) == 0) {
		if (ec_read16(TP_BAT_DATA, &v) == 0 && v != 0xFFFF) {
			b->unit_known = 1;
			b->unit_mwh = (int)((v >> 15) & 1);
		}
	} else {
		b->error = 1;
	}
	if (ec_battery_page(index, TP_BAT_PAGE_STATIC, page_dirty) == 0) {
		if (ec_read16(TP_BAT_DATA, &v) == 0 && v != 0xFFFF)
			b->design = v;
	} else {
		b->error = 1;
	}

	/* The vendor DSDT's GBIF/GBST report mWh/mW when the mode bit is set:
	 * capacities are scaled by 10 (the 10.8 V pack) and the rate by the
	 * measured voltage.  Match that so powerinfo shows the same units as
	 * the firmware's ACPI battery interface.  The percentage above was
	 * already computed from the raw values, so the scale does not matter. */
	if (b->unit_mwh) {
		b->remaining *= 10;
		b->full *= 10;
		b->design *= 10;
		if (b->voltage_mv)
			b->rate = (b->rate * (int)b->voltage_mv) / 1000;
	}
	if (ec_battery_page(index, TP_BAT_PAGE_MFR, page_dirty) == 0)
		ec_read_string(b->manufacturer, sizeof(b->manufacturer),
			       TP_BAT_DATA);
	else
		b->error = 1;
	if (ec_battery_page(index, TP_BAT_PAGE_MODEL, page_dirty) == 0)
		ec_read_string(b->model, sizeof(b->model), TP_BAT_DATA);
	else
		b->error = 1;
}

void tpec_power_read(struct power_info *out, int detail)
{
	unsigned int i;
	unsigned char saved_page = 0, ac;
	int have_saved = 0, page_dirty = 0;

	for (i = 0; i < sizeof(*out); i++)
		((unsigned char *)out)[i] = 0;
	out->ac_online = -1;

	if (tpec_state < 0)
		tpec_init();
	if (!tpec_state)
		return;

	out->present = 1;
	out->source = SYS_POWER_SRC_TPEC;
	out->ec_rev = tpec_ec_rev;
	tpec_copy_ident(out->vendor, sizeof(out->vendor), tpec_vendor);
	tpec_copy_ident(out->product, sizeof(out->product), tpec_product);
	tpec_copy_ident(out->version, sizeof(out->version), tpec_version);

	if (ec_read(TP_AC_STATUS, &ac) == 0)
		out->ac_online = (ac & 0x10) ? 1 : 0;
	if (ec_read(TP_BAT_PAGE, &saved_page) == 0)
		have_saved = 1;

	for (i = 0; i < POWER_BATT_MAX; i++) {
		tpec_read_battery(&out->battery[i], i, detail, &page_dirty);
		if (out->battery[i].present)
			out->battery_count++;
	}
	if (page_dirty)
		(void)ec_write(TP_BAT_PAGE, have_saved ? saved_page : 0);
}
