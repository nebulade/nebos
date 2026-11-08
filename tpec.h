/* ThinkPad embedded-controller sensors and power sources (X201/Arrandale class). */
#ifndef TPEC_H
#define TPEC_H

#include "power_src.h"

#define TPEC_TEMP_COUNT 16

/* Temperatures are indexed like thinkpad_acpi's temp1..temp16:
 * temp_c[0..7] come from EC 0x78..0x7F, temp_c[8..15] from EC 0xC0..0xC7.
 * -128 means the EC reports the sensor as not present. */
struct tpec_info {
	int present;
	char vendor[16];
	char product[32];
	char version[32];
	int temp_c[TPEC_TEMP_COUNT];
	unsigned int fan_rpm;
	unsigned int fan_level;   /* raw EC 0x2F value */
};

/* Probe SMBIOS + the EC once on the BSP; logs the result to COM1.  Does
 * nothing (present = 0) unless SMBIOS identifies a Lenovo/IBM machine and the
 * EC answers, so boards without one are never touched. */
void tpec_init(void);

/* Whether the ThinkPad EC was found; probes on first use. */
int tpec_present(void);

/* Read the current temperatures, fan speed and fan level.  present is 0 when
 * no ThinkPad EC was found. */
void tpec_read(struct tpec_info *out);

/* Read the AC adapter and battery state through the H8 battery information
 * pages into the shared snapshot; detail also reads design capacity, the
 * capacity unit and the manufacturer/model strings.  The only EC write is
 * the page selector at 0x81, and it is restored when it had to be changed. */
void tpec_power_read(struct power_info *out, int detail);

#endif
