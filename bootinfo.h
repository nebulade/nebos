/* Boot information block at 0x500.
 *
 * Written by the legacy VESA stage, the UEFI loader or the kexec receiver,
 * and read by the kernel very early.  It carries the ACPI RSDP, the boot
 * source / GOP pixel order and the physical location of the command-pack
 * rootfs that the boot path copied into high memory (see ramfs.c).
 *
 * A checksum over the twelve dwords (with the checksum field set so the sum
 * is zero) keeps stale low memory from being mistaken for a valid block
 * after a kexec or a legacy boot.
 */
#ifndef BOOTINFO_H
#define BOOTINFO_H

#define BOOT_INFO_ADDR    0x500
#define BOOT_INFO_MAGIC   0x4E425349u   /* "NBSI" */
#define BOOT_INFO_VERSION 2u
#define BOOT_INFO_WORDS   12u

#define BOOTINFO_FLAG_EFI  0x00000001u
#define BOOTINFO_FLAG_RGBX 0x00000002u  /* GOP 32bpp byte order R,G,B,X */
/* The kexec receiver carried the running IPv4 configuration across the
 * jump: reserved[0..3] hold ip/mask/gateway/DNS (byte 0 in the low byte). */
#define BOOTINFO_FLAG_NET  0x00000004u

struct boot_info {
	unsigned int magic;
	unsigned int version;
	unsigned int checksum;
	unsigned int rsdp;          /* physical ACPI RSDP address, 0 = scan */
	unsigned int flags;
	unsigned int pack_addr;     /* boot-time rootfs pack, 0 = none */
	unsigned int pack_size;     /* exact pack bytes (gzip or raw) */
	unsigned int reserved[5];
} __attribute__((packed));

/* Fixed low-memory pointer, hidden from -Warray-bounds like the other boot
 * structures (see vga.c / acpi.c). */
static inline const volatile unsigned int *bootinfo_words(void)
{
	const volatile unsigned int *p =
		(const volatile unsigned int *)BOOT_INFO_ADDR;

	__asm__ volatile ("" : "+r"(p));
	return p;
}

static inline int bootinfo_valid(void)
{
	const volatile unsigned int *p = bootinfo_words();
	unsigned int i, sum = 0;

	if (p[0] != BOOT_INFO_MAGIC || p[1] != BOOT_INFO_VERSION)
		return 0;
	for (i = 0; i < BOOT_INFO_WORDS; i++)
		sum += p[i];
	return sum == 0;
}

static inline unsigned int bootinfo_flags(void)
{
	return bootinfo_valid() ? bootinfo_words()[4] : 0;
}

static inline int bootinfo_efi(void)
{
	return (bootinfo_flags() & BOOTINFO_FLAG_EFI) != 0;
}

static inline int bootinfo_rgbx(void)
{
	return (bootinfo_flags() & BOOTINFO_FLAG_RGBX) != 0;
}

static inline unsigned int bootinfo_rsdp(void)
{
	return bootinfo_valid() ? bootinfo_words()[3] : 0;
}

static inline unsigned int bootinfo_pack_addr(void)
{
	return bootinfo_valid() ? bootinfo_words()[5] : 0;
}

static inline unsigned int bootinfo_pack_size(void)
{
	return bootinfo_valid() ? bootinfo_words()[6] : 0;
}

#endif
