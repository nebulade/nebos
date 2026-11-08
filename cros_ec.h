/* Chrome OS embedded controller power/sensor backend. */
#ifndef CROS_EC_H
#define CROS_EC_H

#include "power_src.h"

#define CROS_EC_TEMP_MAX 24
#define CROS_EC_FAN_MAX 4

/* -128 means the EC reports the sensor as absent/unavailable.  fan_present
 * is a bitmask over fan_rpm[]; fan_duty is the raw EC_ACPI_MEM_FAN_DUTY
 * value (0xff means automatic).  switches follows EC_MEMMAP_SWITCHES
 * (bit 0 lid open, bit 1 power button pressed). */
struct cros_ec_sensors {
	int present;
	unsigned int acpi_version;
	unsigned int battery_version;
	unsigned int thermal_version;
	int temp_c[CROS_EC_TEMP_MAX];
	unsigned int fan_rpm[CROS_EC_FAN_MAX];
	unsigned int fan_present;
	unsigned int fan_duty;
	unsigned int switches;
};

/* Probe once on the BSP; skips the probe when the ThinkPad EC is present.
 * Logs the result to COM1. */
void cros_ec_init(void);

int cros_ec_present(void);

/* Fill the generic AC/battery snapshot; detail also reads design data and
 * the manufacturer/model/serial strings. */
void cros_ec_power_read(struct power_info *out, int detail);

/* Read the EC temperature/fan/switch snapshot. */
void cros_ec_sensors_read(struct cros_ec_sensors *out);

#endif
