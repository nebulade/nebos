/* AC/battery backend selection; see power_src.h. */
#include "power_src.h"
#include "cros_ec.h"
#include "tpec.h"

void power_source_read(struct power_info *out, int detail)
{
	if (tpec_present()) {
		tpec_power_read(out, detail);
		return;
	}
	cros_ec_power_read(out, detail);
}
