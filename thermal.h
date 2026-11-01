/* Intel digital thermal sensor (DTS) access. */
#ifndef THERMAL_H
#define THERMAL_H

/* Probe once on the BSP (before the timer starts); logs the result to COM1. */
void thermal_init(void);

/* Sample this CPU's DTS.  Called from the per-CPU scheduler tick, at most
 * once per second. */
void thermal_tick(void);

/* Read this CPU's DTS now: degrees C, or -1 when unavailable. */
int thermal_read_this_cpu(void);

/* Read this CPU's raw IA32_THERM_STATUS: 0 on success, -1 when unavailable. */
int thermal_status_this_cpu(unsigned int *status);

/* Hottest per-CPU reading cached by the ticks, or -1 when unavailable. */
int thermal_max_c(void);

/* TjMax used for the conversion, or 0 when DTS is unavailable. */
int thermal_tjmax(void);

#endif
