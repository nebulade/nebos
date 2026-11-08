/* Shared AC/battery snapshot for the board embedded controllers.
 *
 * Two backends fill this: the ThinkPad H8 EC (tpec.c) and the Chrome OS EC
 * (cros_ec.c).  sys_info.c copies it into struct sys_powerinfo for
 * SYS_POWER_INFO, so the WM top bar and the powerinfo command do not need to
 * know which laptop they are running on.
 */
#ifndef POWER_SRC_H
#define POWER_SRC_H

#include "syscall.h"

#define POWER_BATT_MAX SYS_POWER_BATT_MAX

/* One battery.  rate is the signed present rate in the unit selected by
 * unit_mwh: mW when set, mA when clear.  Capacities use the matching
 * mWh/mAh unit.  percent is valid only when percent_valid is set. */
struct power_battery {
	int present;
	int error;                /* at least one EC read failed */
	unsigned int status;      /* raw EC status/flags byte */
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
	unsigned int cycle_count;
	char manufacturer[16];
	char model[16];
	char serial[16];
};

struct power_info {
	int present;
	int source;               /* SYS_POWER_SRC_* */
	int ac_online;            /* -1 unknown, 0 offline, 1 online */
	unsigned int ec_rev;
	char vendor[16];
	char product[32];
	char version[32];
	unsigned int battery_count;
	struct power_battery battery[POWER_BATT_MAX];
};

/* Fill out from the first available backend (ThinkPad EC, then Chrome EC).
 * detail also reads design data and the manufacturer/model/serial strings;
 * the periodic WM query passes 0. */
void power_source_read(struct power_info *out, int detail);

#endif
