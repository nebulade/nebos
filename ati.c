/*
 * ATI GPU init.
 *
 * Two distinct ATI families need different handling:
 *
 *  - Mach64 / 3D Rage: the CRTC_OFF_PITCH register (MMIO offset 0x14) drives
 *    display start + scanline pitch, and ignores standard VGA CRTC writes and
 *    VBE 4F07h.  We zero the offset field directly through PCI MMIO or block
 *    I/O (see the header comment in the old ati.c for the method ladder).
 *
 *  - Radeon (R200 family, incl. the RV280 / Radeon 9200/9250): the VBIOS sets
 *    a clean mode and needs no CRTC fixup.  We instead set up the hardware
 *    cursor (radeon.c).
 *
 * Access is verified by reading CONFIG_CHIP_ID (byte offset 0xE0) and checking
 * the lower 16 bits match the PCI device ID.
 */

#include "ati.h"
#include "pci.h"
#include "radeon.h"

#define ATI_VENDOR_ID            0x1002
#define ATI_CRTC_OFF_PITCH_OFS   0x14
#define ATI_CONFIG_CHIP_ID_OFS   0xE0

static unsigned int ati_id;
static unsigned int ati_is_radeon_flag;
static unsigned int off_pitch_before;
static unsigned int off_pitch_after;
static unsigned int mmio_method;   /* 0 = not found, 1..4 per header */

static int ati_device_is_radeon(unsigned int device_id)
{
	switch (device_id) {
	case 0x5960:  /* Radeon 9200 PRO */
	case 0x5961:  /* Radeon 9200 */
	case 0x5962:  /* Radeon 9200 SE */
	case 0x5964:  /* Radeon 9200 / 9250 (RV280) */
	case 0x5965:  /* FireMV 2200 (RV280) */
		return 1;
	default:
		return 0;
	}
}

static int try_mmio(volatile unsigned int *mmio, unsigned int device_id)
{
	unsigned int chip_id = mmio[ATI_CONFIG_CHIP_ID_OFS / 4];
	if ((chip_id & 0xFFFF) != device_id)
		return 0;
	off_pitch_before = mmio[ATI_CRTC_OFF_PITCH_OFS / 4];
	mmio[ATI_CRTC_OFF_PITCH_OFS / 4] = off_pitch_before & 0xFFF00000;
	off_pitch_after = mmio[ATI_CRTC_OFF_PITCH_OFS / 4];
	return 1;
}

static void ati_mach64_fixup(unsigned int bus, unsigned int dev,
			     unsigned int device_id)
{
	unsigned int bar0, bar1, bar2;

	bar0 = pci_cfg_read32(bus, dev, 0, 0x10);
	bar2 = pci_cfg_read32(bus, dev, 0, 0x18);

	/* Method 1: BAR2 (separate MMIO aperture) */
	if (!(bar2 & 1) && (bar2 & 0xFFFFFFF0)) {
		if (try_mmio((volatile unsigned int *)(bar2 & 0xFFFFFFF0),
			     device_id)) {
			mmio_method = 1;
			return;
		}
	}

	/* Method 2-3: MMIO at end of LFB aperture (264VT/GT) */
	if (!(bar0 & 1) && (bar0 & 0xFFFFFFF0)) {
		unsigned int lfb = bar0 & 0xFFFFFFF0;
		if (try_mmio((volatile unsigned int *)(lfb + 0x7FFC00),
			     device_id)) {
			mmio_method = 2;
			return;
		}
		if (try_mmio((volatile unsigned int *)(lfb + 0x3FFC00),
			     device_id)) {
			mmio_method = 3;
			return;
		}
	}

	/* Method 4: BAR1 block I/O */
	bar1 = pci_cfg_read32(bus, dev, 0, 0x14);
	if ((bar1 & 1) && (bar1 & 0xFFFC)) {
		unsigned short io = (unsigned short)(bar1 & 0xFFFC);
		unsigned int chip_id = port_inl(io + ATI_CONFIG_CHIP_ID_OFS);
		if ((chip_id & 0xFFFF) == device_id) {
			off_pitch_before = port_inl(io + ATI_CRTC_OFF_PITCH_OFS);
			port_outl(io + ATI_CRTC_OFF_PITCH_OFS,
				  off_pitch_before & 0xFFF00000);
			off_pitch_after = port_inl(io + ATI_CRTC_OFF_PITCH_OFS);
			mmio_method = 4;
			return;
		}
	}
}

void ati_init(unsigned int bus, unsigned int dev)
{
	unsigned int id = pci_cfg_read32(bus, dev, 0, 0x00);
	unsigned int device_id = (id >> 16) & 0xFFFF;

	ati_id = id;
	off_pitch_before = 0;
	off_pitch_after = 0;
	mmio_method = 0;

	ati_is_radeon_flag = ati_device_is_radeon(device_id);

	if (ati_is_radeon_flag) {
		radeon_init(bus, dev, device_id);
		return;
	}

	ati_mach64_fixup(bus, dev, device_id);
}

int ati_is_radeon(void)          { return ati_is_radeon_flag; }
unsigned int ati_pci_id(void)    { return ati_id; }
unsigned int ati_off_pitch_before(void){ return off_pitch_before; }
unsigned int ati_off_pitch_after(void) { return off_pitch_after; }
unsigned int ati_mmio_method(void)     { return mmio_method; }
