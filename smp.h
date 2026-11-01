/* SMP boot: ACPI CPU discovery, AP trampoline and INIT-SIPI. */
#ifndef SMP_H
#define SMP_H

extern unsigned int smp_sched_enabled;

void smp_init(void);
/* Let APs take user work, after boot-time device/FS setup is complete. */
void smp_enable_sched(void);

/* AP C entry (called by the trampoline). */
void smp_ap_main(unsigned int cpu);

/* Ask another CPU to run its scheduler soon (LAPIC IPI). */
void smp_resched_cpu(unsigned int cpu);

/* Park APs before kexec overwrites the kernel image. */
void smp_stop_aps(void);

void smp_dump(void);

#endif
