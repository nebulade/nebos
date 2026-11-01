/* Local APIC access: BSP/AP bring-up, IPIs and the per-CPU timer. */
#ifndef LAPIC_H
#define LAPIC_H

#define LAPIC_TIMER_VECTOR   0x30
#define LAPIC_RESCHED_VECTOR 0x31
#define LAPIC_SPURIOUS_VECTOR 0xFF

void lapic_set_base(unsigned int base);
unsigned int lapic_base(void);

int  lapic_init_bsp(void);
int  lapic_init_ap(void);
void lapic_eoi(void);
unsigned int lapic_id(void);

void lapic_send_ipi(unsigned int apic_id, unsigned int vector);
void lapic_send_init(unsigned int apic_id);
void lapic_send_sipi(unsigned int apic_id, unsigned int vector);

int  lapic_timer_start(unsigned int vector);
void lapic_timer_stop(void);

#endif
