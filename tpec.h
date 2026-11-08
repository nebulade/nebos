/* ThinkPad embedded-controller sensors and power sources (X201/Arrandale class). */
#ifndef TPEC_H
#define TPEC_H

#define TPEC_TEMP_COUNT 16
#define TPEC_BATTERY_COUNT 2

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

/* One battery from the H8 battery information area (EC page selector 0x81). */
struct tpec_battery {
	int present;
	int error;                /* at least one EC read failed */
	unsigned int status;      /* raw EC 0x38/0x39 status byte */
	int charging;
	int discharging;
	int percent;              /* 0..100 when percent_valid */
	int percent_valid;
	unsigned int remaining;   /* reported remaining capacity */
	unsigned int full;        /* reported last full charge capacity */
	unsigned int design;      /* reported design capacity */
	unsigned int voltage_mv;
	int rate;                 /* signed reported present rate */
	int unit_known;
	int unit_mwh;             /* 1 = mWh/mW, 0 = mAh/mA */
	char manufacturer[16];
	char model[16];
};

struct tpec_power {
	int present;              /* ThinkPad EC found */
	int ac_online;            /* -1 unknown, 0 offline, 1 online */
	unsigned int ec_rev;
	char vendor[16];
	char product[32];
	char version[32];
	unsigned int battery_count;
	struct tpec_battery battery[TPEC_BATTERY_COUNT];
};

/* Probe SMBIOS + the EC once on the BSP; logs the result to COM1.  Does
 * nothing (present = 0) unless SMBIOS identifies a Lenovo/IBM machine and the
 * EC answers, so boards without one are never touched. */
void tpec_init(void);

/* Read the current temperatures, fan speed and fan level.  present is 0 when
 * no ThinkPad EC was found. */
void tpec_read(struct tpec_info *out);

/* Read the AC adapter and battery state through the H8 battery information
 * pages; detail also reads design capacity, the capacity unit and the
 * manufacturer/model strings.  The only EC write is the page selector at
 * 0x81, and it is restored when it had to be changed. */
void tpec_power_read(struct tpec_power *out, int detail);

#endif
