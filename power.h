/* Intel Nehalem/Westmere P-state and deep C-state management. */
#ifndef POWER_H
#define POWER_H

#define POWER_CPUFREQ_AUTO  0
#define POWER_CPUFREQ_MIN   1
#define POWER_CPUFREQ_MAX   2
#define POWER_CPUFREQ_FIXED 3
#define POWER_CPUFREQ_OFF   4

/* Probe the CPU and MSRs, enable SpeedStep/C1E/C6 and start at the maximum
 * non-turbo ratio.  Called once on the BSP before idle_init(); logs to COM1
 * and is a no-op on CPUs outside the Nehalem/Westmere allowlist. */
void power_init(void);

/* Once per scheduler tick with the CPU's idle flag and the tick length. */
void power_tick(int idle, unsigned int ms);

/* Deep C-state hint for idle.c's automatic mode: -1 = use HLT, otherwise
 * the MWAIT hint (0x20 C6; 0x01 C1E on an AP whose local timer stops). */
int power_idle_hint(void);

/* `power` / `cpufreq` command support (terminal.c). */
void power_print(void);
int power_cpufreq_command(const char *arg);
const char *power_cpufreq_name(void);

#endif
