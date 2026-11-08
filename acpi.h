/* Minimal ACPI table discovery: RSDP -> RSDT/XSDT -> MADT.
 *
 * Only the CPU/LAPIC information needed to start APs is parsed.  Absence of
 * ACPI or of a usable MADT is not an error: the kernel then stays UP.
 */
#ifndef ACPI_H
#define ACPI_H

#define ACPI_MAX_CPUS 8

/* Returns 1 when a usable MADT with at least one local APIC entry was found.
 * acpi_cpu_count() then includes the BSP as entry 0. */
int acpi_init(void);

unsigned int acpi_cpu_count(void);
unsigned int acpi_cpu_apic_id(unsigned int index);
unsigned int acpi_bsp_apic_id(void);
unsigned int acpi_lapic_base(void);

/* CMOS register holding the RTC century byte from the FADT, or 0 when the
 * firmware does not provide one (old non-ACPI hardware). */
unsigned int acpi_rtc_century_reg(void);

void acpi_dump(void);

#endif
