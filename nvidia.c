/*
 * NVIDIA GPU support.
 *
 * Unlike ATI Mach64/Rage which need manual CRTC_OFF_PITCH fixups, NVIDIA
 * cards handle VBE mode sets cleanly through their VBIOS: the real-mode stage
 * provides a working LFB and no display fixup is needed.  The standard VGA
 * CRTC registers at 0x3D4/0x3D5 must NOT be touched on NVIDIA -- the display
 * engine ignores or misinterprets legacy VGA CRTC writes, which can cause
 * display corruption or a reboot.
 *
 * What this file adds on top of the (VBE-only) no-op is identification: the
 * register aperture (BAR0, 16 MB on nv50+) is mapped and validated by reading
 * NV_PMC_BOOT_0, from which the chipset id is decoded.  That aperture is also
 * what the nv50-family display engine (ION/MCP79) is programmed through.
 *
 * NOTE: nv50-era VBIOSes under-report VBE TotalMemory (the AT3IONT-I reports
 * 14 MB for a mode that needs 5 MB); don't trust it for VRAM sizing.
 */

#include "nvidia.h"
#include "nv50.h"
#include "pci.h"
#include "process.h"
#include "serial.h"

#define NV_VENDOR_ID     0x10DE
#define NV_BAR0_SIZE     0x1000000   /* register aperture (16 MB on nv50+) */

static unsigned int nv_present;      /* BAR0 mapped + PMC_BOOT_0 sane */
static unsigned int nv_boot0;
static unsigned int nv_chipset;
static unsigned int nv_bar0;
static unsigned int nv_bar1;
static unsigned int nv_bar1_size;
static unsigned int nv_bar3;
static unsigned int nv_bar3_size;
static unsigned int nv_vram_base;
static unsigned int nv_vram_size;
static const char  *nv_name = "NVIDIA";

struct nv_name {
	unsigned short device_id;
	const char *name;
};

/* Known display devices.  Not exhaustive -- unknown ids fall back to
 * "NVIDIA"; the PCI id is printed separately by `display`. */
static const struct nv_name nv_names[] = {
	{ 0x0865, "C79 [GeForce 9300 / ION]" },
	{ 0x0869, "MCP7A [GeForce 9400]" },
	{ 0x086D, "C79 [GeForce 9200]" },
	{ 0x0870, "C79 [GeForce 9400M]" },
	{ 0x0874, "C79 [ION]" },
	{ 0x0876, "C79 [GeForce 9400M / ION]" },
	{ 0x087D, "C79 [ION]" },
	{ 0x087E, "C79 [ION LE]" },
	{ 0x0A64, "GT218 [ION]" },
	{ 0x0A6F, "GT218M [ION]" },
	{ 0x0A76, "GT218M [ION 2]" },
	{ 0x1401, "GM206 [GeForce GTX 960]" },
};

/* Read a memory BAR's size in bytes (probe-and-restore, like lspci; handles
 * 64-bit BARs by clearing/restoring the high dword as well). */
static unsigned int nv_bar_size(unsigned int bus, unsigned int dev,
				unsigned int reg, unsigned int is64)
{
	unsigned int val = pci_cfg_read32(bus, dev, 0, reg);
	unsigned int hi = 0, raw;

	if (val & 1)
		return 0;                       /* I/O BAR */
	if (is64)
		hi = pci_cfg_read32(bus, dev, 0, reg + 4);
	pci_cfg_write32(bus, dev, 0, reg, 0xFFFFFFFF);
	if (is64)
		pci_cfg_write32(bus, dev, 0, reg + 4, 0xFFFFFFFF);
	raw = pci_cfg_read32(bus, dev, 0, reg) & 0xFFFFFFF0u;
	pci_cfg_write32(bus, dev, 0, reg, val);
	if (is64)
		pci_cfg_write32(bus, dev, 0, reg + 4, hi);

	if (raw == 0)
		return 0;
	return ~raw + 1;
}

void nvidia_init(unsigned int bus, unsigned int dev, unsigned int device_id)
{
	unsigned int bar0, boot0, i;

	nv_present = 0;
	nv_boot0 = 0;
	nv_chipset = 0;
	nv_bar0 = 0;
	nv_bar1 = 0;
	nv_bar1_size = 0;
	nv_bar3 = 0;
	nv_bar3_size = 0;
	nv_vram_base = 0;
	nv_vram_size = 0;
	nv_name = "NVIDIA";

	for (i = 0; i < sizeof(nv_names) / sizeof(nv_names[0]); i++) {
		if (nv_names[i].device_id == device_id) {
			nv_name = nv_names[i].name;
			break;
		}
	}

	/* BAR0: register aperture.  The nv50+ display engine registers
	 * (EVO/PDISPLAY) live in it starting at 0x00610000. */
	bar0 = pci_cfg_read32(bus, dev, 0, 0x10);
	if (!(bar0 & 1) && (bar0 & 0xFFFFFFF0)) {
		nv_bar0 = bar0 & 0xFFFFFFF0;
		/* Map into every process page directory: gpu_init() runs
		 * before the first process exists, but drivers may run in a
		 * terminal context later (same reason r8168 does this). */
		process_map_mmio_all(nv_bar0, NV_BAR0_SIZE);

		boot0 = *(volatile unsigned int *)nv_bar0;
		if (boot0 != 0 && boot0 != 0xFFFFFFFF) {
			nv_boot0 = boot0;
			nv_chipset = (boot0 >> 20) & 0xFF;
			nv_present = 1;
		}
	}

	/* BAR1: VRAM window (64-bit prefetchable on nv50+); BAR3: a second
	 * (32 MB) VRAM alias the VBIOS uses for the VBE linear framebuffer. */
	{
		unsigned int b1 = pci_cfg_read32(bus, dev, 0, 0x14);
		unsigned int b1hi = pci_cfg_read32(bus, dev, 0, 0x18);
		unsigned int b3, b3hi;

		if (!(b1 & 1) && (b1 & 0xFFFFFFF0) && b1hi == 0)
			nv_bar1 = b1 & 0xFFFFFFF0;
		if (nv_bar1) {
			nv_bar1_size = nv_bar_size(bus, dev, 0x14, b1 & 4);
			if (nv_bar1_size > 0x10000000u)   /* cap the mapping */
				nv_bar1_size = 0x10000000u;
			process_map_mmio_all(nv_bar1, nv_bar1_size);
		}

		b3 = pci_cfg_read32(bus, dev, 0, 0x1C);
		b3hi = pci_cfg_read32(bus, dev, 0, 0x20);
		if (!(b3 & 1) && (b3 & 0xFFFFFFF0) && b3hi == 0)
			nv_bar3 = b3 & 0xFFFFFFF0;
		if (nv_bar3) {
			nv_bar3_size = nv_bar_size(bus, dev, 0x1C, b3 & 4);
			if (nv_bar3_size > 0x04000000u)
				nv_bar3_size = 0x04000000u;
			process_map_mmio_all(nv_bar3, nv_bar3_size);
		}
	}

	/* MCP77/78/79/89 (UMA): the memory controller reports the stolen
	 * region's physical base and size at 0x100E10/0x100E14 (1 MB units).
	 * The VBE framebuffer sits at its start, so subtracting the base gives
	 * the display engine's VRAM address for the LFB. */
	if (nv_present && (nv_chipset == 0xaa || nv_chipset == 0xac ||
			   nv_chipset == 0xaf)) {
		unsigned int vb = *(volatile unsigned int *)(nv_bar0 + 0x100E10);
		unsigned int vs = *(volatile unsigned int *)(nv_bar0 + 0x100E14);

		nv_vram_base = vb << 12;
		nv_vram_size = vs << 12;
	}

	serial_puts(SERIAL_COM1, "nvidia: ");
	serial_puts(SERIAL_COM1, nv_name);
	serial_puts(SERIAL_COM1, " dev ");
	serial_puthex(SERIAL_COM1, (NV_VENDOR_ID << 16) | device_id);
	if (nv_present) {
		serial_puts(SERIAL_COM1, " chipset ");
		serial_puthex(SERIAL_COM1, nv_chipset);
		serial_puts(SERIAL_COM1, " boot0 ");
		serial_puthex(SERIAL_COM1, nv_boot0);
		serial_puts(SERIAL_COM1, " regs ");
		serial_puthex(SERIAL_COM1, nv_bar0);
		serial_puts(SERIAL_COM1, " bar1 ");
		serial_puthex(SERIAL_COM1, nv_bar1);
		serial_puts(SERIAL_COM1, " bar3 ");
		serial_puthex(SERIAL_COM1, nv_bar3);
	} else {
		serial_puts(SERIAL_COM1, " regs unavailable");
	}
	serial_puts(SERIAL_COM1, "\n");

	if (nv_vram_size) {
		serial_puts(SERIAL_COM1, "nvidia: vram base ");
		serial_puthex(SERIAL_COM1, nv_vram_base);
		serial_puts(SERIAL_COM1, " size ");
		serial_puthex(SERIAL_COM1, nv_vram_size);
		serial_puts(SERIAL_COM1, "\n");
	}

	/* Read-only look at the display engine (EVO channels, object table,
	 * cursor state) for boards with no serial console (netmon mirror). */
	nv50_probe();
}

int nvidia_available(void)
{
	return (int)nv_present;
}

const char *nvidia_chip_name(void)
{
	return nv_name;
}

unsigned int nvidia_chipset(void)
{
	return nv_chipset;
}

unsigned int nvidia_bar0(void)
{
	return nv_bar0;
}

unsigned int nvidia_bar1(void)
{
	return nv_bar1;
}

unsigned int nvidia_bar1_size(void)
{
	return nv_bar1_size;
}

unsigned int nvidia_bar3(void)
{
	return nv_bar3;
}

unsigned int nvidia_bar3_size(void)
{
	return nv_bar3_size;
}

unsigned int nvidia_vram_base(void)
{
	return nv_vram_base;
}

unsigned int nvidia_vram_size(void)
{
	return nv_vram_size;
}
