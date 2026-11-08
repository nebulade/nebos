/* Minimal ACPI RSDP/RSDT/XSDT/MADT parser.  See acpi.h.
 *
 * ACPI tables are usually below the 128 MB identity map, but on larger
 * machines they can sit above it; every table page is mapped through
 * paging_map_ram() before it is read.  Anything malformed or unsupported is
 * treated as "no SMP", never as a fatal error.
 */

#include "acpi.h"
#include "bootinfo.h"
#include "paging.h"
#include "serial.h"

struct acpi_rsdp {
	char signature[8];
	unsigned char checksum;
	char oem_id[6];
	unsigned char revision;
	unsigned int rsdt;
	unsigned int length;
	unsigned long long xsdt;
	unsigned char ext_checksum;
	unsigned char reserved[3];
} __attribute__((packed));

struct acpi_sdt {
	char signature[4];
	unsigned int length;
	unsigned char revision;
	unsigned char checksum;
	char oem_id[6];
	char oem_table_id[8];
	unsigned int oem_revision;
	unsigned int creator_id;
	unsigned int creator_revision;
} __attribute__((packed));

#define RSDP_SIG "RSD PTR "
#define MADT_SIG "APIC"
#define LAPIC_DEFAULT 0xFEE00000u

static unsigned int ids[ACPI_MAX_CPUS];
static unsigned int ncpu;
static unsigned int bsp_apic;
static unsigned int lapic_base;
static int acpi_ok;
/* FADT "century" register (offset 108), 0 when absent.  The RTC uses it to
 * keep the two-digit CMOS year in the right century; rtc.c only writes it
 * when the firmware points at a real CMOS register. */
static unsigned int rtc_century_reg;

static void *phys_ptr(unsigned int addr)
{
	return (void *)addr;
}

static int range_ok(unsigned int addr, unsigned int len)
{
	if (addr >= 0xFFFFF000u || len > 0x100000u)
		return 0;
	return addr + len >= addr;
}

/* Map every 4 MB page touched by [addr, addr+len). */
static void map_table(unsigned int addr, unsigned int len)
{
	unsigned int p = addr & 0xFFC00000u;
	unsigned int last = (addr + len - 1u) & 0xFFC00000u;

	for (;;) {
		paging_map_ram(p, 0x400000u);
		if (p >= last)
			break;
		if (p > 0xFFC00000u - 0x400000u)
			break;
		p += 0x400000u;
	}
}

static int sum_ok(const unsigned char *p, unsigned int len)
{
	unsigned char sum = 0;
	unsigned int i;

	for (i = 0; i < len; i++)
		sum = (unsigned char)(sum + p[i]);
	return sum == 0;
}

static int rsdp_valid(const struct acpi_rsdp *r)
{
	unsigned int i;

	for (i = 0; i < 8; i++)
		if (r->signature[i] != RSDP_SIG[i])
			return 0;
	if (!sum_ok((const unsigned char *)r, 20))
		return 0;
	if (r->revision >= 2) {
		if (r->length < 36 || r->length > 4096)
			return 0;
		if (!sum_ok((const unsigned char *)r, r->length))
			return 0;
	}
	return 1;
}

static const struct acpi_rsdp *find_rsdp(void)
{
	volatile unsigned short *ebda_seg = (volatile unsigned short *)0x40E;
	unsigned int ebda, i;
	unsigned int given = bootinfo_rsdp();

	/* The UEFI loader passes the RSDP address from the system table.  UEFI
	 * places ACPI tables outside the 128 MB identity map often enough that
	 * the scan below would never see them, so map the page first. */
	if (given && given < 0xFFF00000u) {
		const struct acpi_rsdp *r;

		paging_map_ram(given & 0xFFC00000u, 0x800000u);
		r = (const struct acpi_rsdp *)given;
		if (rsdp_valid(r)) {
			serial_puts(SERIAL_COM1, "acpi: EFI RSDP at ");
			serial_puthex(SERIAL_COM1, given);
			serial_puts(SERIAL_COM1, "\n");
			return r;
		}
		serial_puts(SERIAL_COM1, "acpi: EFI RSDP invalid, scanning\n");
	}

	/* Opaque asm keeps -Warray-bounds quiet about the fixed BIOS pointer. */
	__asm__ volatile ("" : "+r"(ebda_seg));
	ebda = (unsigned int)(*ebda_seg) << 4;

	if (ebda) {
		for (i = 0; i < 1024; i += 16) {
			const struct acpi_rsdp *r =
				(const struct acpi_rsdp *)(ebda + i);
			if (rsdp_valid(r))
				return r;
		}
	}
	for (i = 0xE0000; i < 0x100000; i += 16) {
		const struct acpi_rsdp *r =
			(const struct acpi_rsdp *)(i);
		if (rsdp_valid(r))
			return r;
	}
	return 0;
}

static const struct acpi_sdt *find_madt(const struct acpi_rsdp *rsdp)
{
	unsigned int addr, len, i, step;
	const struct acpi_sdt *sdt;
	const struct acpi_sdt *madt = 0;

	/* Prefer the 64-bit XSDT when it is reachable from the 32-bit
	 * identity map; otherwise fall back to the ACPI 1.0 RSDT. */
	if (rsdp->revision >= 2 && rsdp->xsdt &&
	    (unsigned int)(rsdp->xsdt >> 32) == 0) {
		addr = (unsigned int)rsdp->xsdt;
		step = 8;
	} else {
		addr = rsdp->rsdt;
		step = 4;
	}
	if (!addr || !range_ok(addr, sizeof(struct acpi_sdt)))
		return 0;
	map_table(addr, sizeof(struct acpi_sdt));
	sdt = (const struct acpi_sdt *)phys_ptr(addr);
	len = sdt->length;
	if (len < sizeof(struct acpi_sdt) || len > 0x100000u)
		return 0;
	map_table(addr, len);
	if (!sum_ok((const unsigned char *)sdt, len))
		return 0;

	for (i = sizeof(struct acpi_sdt); i + step <= len; i += step) {
		unsigned int taddr;
		const struct acpi_sdt *t;
		unsigned int tlen;

		if (step == 8) {
			unsigned long long a =
				*(const unsigned long long *)((const unsigned char *)sdt + i);
			if ((unsigned int)(a >> 32) != 0)
				continue;
			taddr = (unsigned int)a;
		} else {
			taddr = *(const unsigned int *)((const unsigned char *)sdt + i);
		}
		if (!taddr || !range_ok(taddr, sizeof(struct acpi_sdt)))
			continue;
		map_table(taddr, sizeof(struct acpi_sdt));
		t = (const struct acpi_sdt *)phys_ptr(taddr);
		tlen = t->length;
		if (tlen < sizeof(struct acpi_sdt) || tlen > 0x100000u)
			continue;
		map_table(taddr, tlen);
		if (!sum_ok((const unsigned char *)t, tlen))
			continue;
		if (t->signature[0] == 'F' && t->signature[1] == 'A' &&
		    t->signature[2] == 'C' && t->signature[3] == 'P') {
			/* FADT "century" is at offset 108 (ACPI 2.0+); 0 means
			 * the firmware does not provide a century register. */
			if (tlen >= 109) {
				unsigned char c =
					((const unsigned char *)t)[108];

				if (c >= 0x0E && c <= 0x7F)
					rtc_century_reg = c;
			}
			continue;
		}
		if (t->signature[0] == 'A' && t->signature[1] == 'P' &&
		    t->signature[2] == 'I' && t->signature[3] == 'C' &&
		    !madt)
			madt = t;
	}
	return madt;
}

static unsigned int cpuid_apic_id(void)
{
	unsigned int eax, ebx, ecx, edx;

	eax = 1;
	__asm__ volatile ("cpuid"
			  : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
	if (!(edx & (1u << 9)))       /* CPUID.1:EDX.APIC */
		return 0xFFFFFFFFu;
	return ebx >> 24;             /* initial local APIC ID */
}

static void parse_madt(const struct acpi_sdt *madt)
{
	const unsigned char *p = (const unsigned char *)madt;
	unsigned int len = madt->length;
	unsigned int i;

	lapic_base = LAPIC_DEFAULT;
	if (len >= 44) {
		unsigned int base = *(const unsigned int *)(p + 36);
		if (base)
			lapic_base = base;
	}

	for (i = 44; i + 2 <= len; ) {
		unsigned char type = p[i];
		unsigned char elen = p[i + 1];

		if (elen < 2 || i + elen > len)
			break;
		if (type == 0 && elen >= 8) {
			/* Processor Local APIC: flags bit 0 = enabled. */
			if ((p[i + 4] & 1) && ncpu < ACPI_MAX_CPUS)
				ids[ncpu++] = p[i + 3];
		} else if (type == 5 && elen >= 12) {
			/* Local APIC Address Override (64-bit). */
			unsigned int lo = *(const unsigned int *)(p + i + 4);
			unsigned int hi = *(const unsigned int *)(p + i + 8);
			if (!hi && lo)
				lapic_base = lo;
		}
		/* x2APIC (type 9), IOAPIC (1), NMI (4) and overrides are not
		 * needed for the first SMP step. */
		i += elen;
	}
}

int acpi_init(void)
{
	const struct acpi_rsdp *rsdp;
	const struct acpi_sdt *madt;
	unsigned int i;

	acpi_ok = 0;
	ncpu = 0;
	rtc_century_reg = 0;
	bsp_apic = cpuid_apic_id();
	if (bsp_apic == 0xFFFFFFFFu)
		return 0;

	rsdp = find_rsdp();
	if (!rsdp) {
		serial_puts(SERIAL_COM1, "acpi: no RSDP, staying UP\n");
		return 0;
	}
	madt = find_madt(rsdp);
	if (!madt) {
		serial_puts(SERIAL_COM1, "acpi: no MADT, staying UP\n");
		return 0;
	}
	parse_madt(madt);

	/* Put the BSP first.  If firmware omitted it, keep the discovered list
	 * but make sure entry 0 is a real CPU. */
	for (i = 0; i < ncpu; i++)
		if (ids[i] == bsp_apic)
			break;
	if (i < ncpu) {
		unsigned int t = ids[0];
		ids[0] = ids[i];
		ids[i] = t;
	} else if (ncpu) {
		ids[0] = bsp_apic;
	}
	if (!ncpu)
		return 0;

	acpi_ok = 1;
	return 1;
}

unsigned int acpi_cpu_count(void)
{
	return acpi_ok ? ncpu : 0;
}

unsigned int acpi_cpu_apic_id(unsigned int index)
{
	return (acpi_ok && index < ncpu) ? ids[index] : 0;
}

unsigned int acpi_bsp_apic_id(void)
{
	return bsp_apic;
}

unsigned int acpi_lapic_base(void)
{
	return lapic_base;
}

unsigned int acpi_rtc_century_reg(void)
{
	return rtc_century_reg;
}

void acpi_dump(void)
{
	unsigned int i;

	if (!acpi_ok) {
		serial_puts(SERIAL_COM1, "acpi: unavailable, 1 cpu\n");
		return;
	}
	serial_puts(SERIAL_COM1, "acpi: lapic ");
	serial_puthex(SERIAL_COM1, lapic_base);
	serial_puts(SERIAL_COM1, ", ");
	serial_putdec(SERIAL_COM1, ncpu);
	serial_puts(SERIAL_COM1, " cpus:");
	for (i = 0; i < ncpu; i++) {
		serial_puts(SERIAL_COM1, " ");
		serial_putdec(SERIAL_COM1, ids[i]);
	}
	serial_puts(SERIAL_COM1, "\n");
}
