/*
 * ATI Mach64 (264VT / 3D Rage) -- GPU-specific CRTC fixups.
 *
 * The CRTC_OFF_PITCH register (byte offset 0x14 in the MMIO block) controls
 * display start offset and scanline pitch.  Standard VGA CRTC writes and
 * VBE 4F07h are ignored by these chips.  We zero the offset field directly
 * through PCI MMIO or block I/O.
 *
 * CRTC_OFF_PITCH layout (32 bits):
 *   bits  0-19: CRTC_OFFSET  (display start in 8-byte units)
 *   bits 22-31: CRTC_PITCH   (scanline pitch in 8-pixel units)
 *
 * Access is verified by reading CONFIG_CHIP_ID (byte offset 0xE0) and
 * checking the lower 16 bits match the PCI device ID.
 *
 * Methods tried in order:
 *   1 - BAR2 separate MMIO aperture
 *   2 - MMIO at end of LFB aperture (LFB + 8 MB - 1 KB)
 *   3 - MMIO at end of LFB aperture (LFB + 4 MB - 1 KB)
 *   4 - BAR1 block I/O
 */

#include "ati.h"
#include "pci.h"

#define ATI_VENDOR_ID            0x1002
#define ATI_CRTC_OFF_PITCH_OFS   0x14
#define ATI_CONFIG_CHIP_ID_OFS   0xE0

static unsigned int ati_id;
static unsigned int off_pitch_before;
static unsigned int off_pitch_after;
static unsigned int mmio_method;   /* 0 = not found, 1..4 per header */

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

void ati_init(void)
{
	unsigned int bus, dev, id, class_rev;
	unsigned int bar0, bar1, bar2;
	unsigned int device_id;

	for (bus = 0; bus < 4; bus++) {
		for (dev = 0; dev < 32; dev++) {
			id = pci_cfg_read32(bus, dev, 0, 0x00);
			if ((id & 0xFFFF) != ATI_VENDOR_ID)
				continue;
			class_rev = pci_cfg_read32(bus, dev, 0, 0x08);
			if ((class_rev >> 24) != 0x03)
				continue;

			ati_id = id;
			device_id = (id >> 16) & 0xFFFF;

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
			return;
		}
	}
}

unsigned int ati_pci_id(void)          { return ati_id; }
unsigned int ati_off_pitch_before(void){ return off_pitch_before; }
unsigned int ati_off_pitch_after(void) { return off_pitch_after; }
unsigned int ati_mmio_method(void)     { return mmio_method; }
